#include "StdAfx.h"

#include "addon_user_data.h"
#include "addon_host.h"
#include "addon_lua_entry.h"

#include "xrScriptEngine/script_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(XR_PLATFORM_WINDOWS)
#include <shlobj.h> // SHGetKnownFolderPath: the folder of the launcher data, as Tauri finds it
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib") // CoTaskMemFree
#else
#include <fcntl.h> // open
#include <pwd.h> // getpwuid: the home folder without $HOME
#include <sys/file.h> // flock
#include <sys/stat.h> // fstat
#include <unistd.h> // read, write, fsync, close, getpid, unlink
#endif

namespace gw::addons::user_data
{
namespace
{
namespace fs = std::filesystem;

constexpr u32 kMaxFileSize = GWP_USER_DATA_MAX_FILE_SIZE;
constexpr size_t kMaxNameLength = GWP_USER_DATA_MAX_NAME;
constexpr size_t kMaxIdLength = 64; // the ids of the addons: [a-z0-9_]{1,64} (addon_host.cpp IsValidId)
constexpr pcstr kCoreOwner = "core"; // the folder of the scripts of the base
// The Tauri identifier of the launcher (launcher-new/src-tauri/tauri.conf.json): its data lies in
// <config>/<identifier>/, the game puts its own next to it, in game/
constexpr pcstr kVendorFolder = "com.ruut.stalker";
constexpr pcstr kGameFolder = "game";
constexpr pcstr kAddonsFolder = "addons";
constexpr pcstr kTemporarySuffix = ".tmp";
constexpr pcstr kLockSuffix = ".lock";
constexpr auto kStaleTemporary = std::chrono::hours(24); // a temporary file older than this is a leftover of a crash
constexpr auto kLockRetry = std::chrono::milliseconds(5);

enum class EError
{
    None,
    InvalidArgument, // a bad name, id, argument
    NotFound, // no such file
    TooBig, // over kMaxFileSize
    Io, // the OS failed (logged)
    Timeout, // the lock is held by another
    AccessDenied, // the folder of another addon
    Callback, // the Lua function of update failed (logged)
};

pcstr ErrorName(EError error)
{
    switch (error)
    {
    case EError::None: return "";
    case EError::InvalidArgument: return "invalid_argument";
    case EError::NotFound: return "not_found";
    case EError::TooBig: return "too_big";
    case EError::Io: return "io";
    case EError::Timeout: return "timeout";
    case EError::AccessDenied: return "access_denied";
    case EError::Callback: return "callback";
    }
    return "io";
}

GwpResult ResultOf(EError error)
{
    switch (error)
    {
    case EError::None: return GWP_OK;
    case EError::InvalidArgument: return GWP_ERROR_INVALID_ARGUMENT;
    case EError::NotFound: return GWP_ERROR_NOT_FOUND;
    case EError::TooBig: return GWP_ERROR_NOT_SUPPORTED;
    case EError::Timeout: return GWP_ERROR_INVALID_STATE;
    case EError::AccessDenied: return GWP_ERROR_ACCESS_DENIED;
    case EError::Io:
    case EError::Callback: return GWP_ERROR;
    }
    return GWP_ERROR;
}

bool EndsWith(std::string_view text, std::string_view tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// con, prn, aux, nul, com0..9, lpt0..9 - with any extension Windows opens the device instead of a file
bool IsDeviceName(std::string_view name)
{
    const std::string_view stem = name.substr(0, name.find('.'));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    return stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") && stem[3] >= '0' &&
        stem[3] <= '9';
}

// The rule of the names of the files (gwp_api.h, user_data_*): [a-z0-9_.-]{1,64}, no separator, no "..", no leading
// or trailing dot, not a file of the engine (.tmp, .lock), not a device of Windows. Lower case only: the same name
// on Windows (case-insensitive) and on Linux
bool IsValidName(std::string_view name)
{
    if (name.empty() || name.size() > kMaxNameLength || name.front() == '.' || name.back() == '.')
        return false;
    const bool characters = std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
    if (!characters || name.find("..") != std::string_view::npos)
        return false;
    if (EndsWith(name, kTemporarySuffix) || EndsWith(name, kLockSuffix))
        return false;
    return !IsDeviceName(name);
}

bool IsValidOwner(std::string_view id)
{
    if (id.empty() || id.size() > kMaxIdLength || IsDeviceName(id))
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

xr_string LastErrorText()
{
#if defined(XR_PLATFORM_WINDOWS)
    return ErrorText(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
#else
    return ErrorText(std::error_code(errno, std::generic_category()));
#endif
}

// "! [addon:<id>] user_data: <what> <path>: <the error of the OS>": the error is taken before anything else runs
void LogOsFailure(const xr_string& id, pcstr what, const fs::path& path)
{
    const xr_string reason = LastErrorText();
    Msg("! [addon:%s] user_data: %s %s: %s", id.c_str(), what, PathToUtf8(path).c_str(), reason.c_str());
}

// ---------------------------------------------------------------------------------------------
// The root
// ---------------------------------------------------------------------------------------------

std::mutex g_root_mutex;
bool g_root_resolved = false;
fs::path g_root; // empty: no root (logged once)
xr_vector<xr_string> g_swept; // the addons whose leftovers of crashes were removed in this process

// -user_data <folder> (the first one; the folder may be quoted), "" without the key
xr_string CommandLineRoot()
{
    constexpr pcstr key = "-user_data ";
    pcstr p = strstr(Core.Params, key);
    if (!p)
        return {};
    p += xr_strlen(key);
    while (*p == ' ')
        ++p;
    if (*p == '"')
    {
        const pcstr end = strchr(p + 1, '"');
        return xr_string(p + 1, end ? end : p + xr_strlen(p));
    }
    const pcstr end = strchr(p, ' ');
    return xr_string(p, end ? end : p + xr_strlen(p));
}

#if !defined(XR_PLATFORM_WINDOWS)
// $HOME, else the home folder of the user from the password database; empty - none
fs::path HomeFolder()
{
    const char* home = getenv("HOME");
    if (home && home[0] == '/')
        return fs::path(home);
    const passwd* entry = getpwuid(getuid());
    if (entry && entry->pw_dir && entry->pw_dir[0] == '/')
        return fs::path(entry->pw_dir);
    return {};
}
#endif

// The folder of the user data of the platform the launcher keeps its data in (Tauri BaseDirectory::AppConfig without
// the identifier: dirs::config_dir). `how` - for the log
fs::path PlatformBase(xr_string& how)
{
#if defined(XR_PLATFORM_WINDOWS)
    PWSTR wide = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &wide)) && wide)
        base = fs::path(wide);
    if (wide)
        CoTaskMemFree(wide);
    how = "FOLDERID_RoamingAppData";
    if (base.empty())
    {
        // The known folder is missing (a broken profile): %APPDATA%
        wchar_t buffer[MAX_PATH * 4];
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
        if (length > 0 && length < std::size(buffer))
            base = fs::path(buffer);
        if (!base.is_absolute())
            base.clear(); // a relative value would put the data next to the current folder of the process
        how = "%APPDATA%";
    }
    return base;
#elif defined(XR_PLATFORM_APPLE)
    how = "~/Library/Application Support";
    const fs::path home = HomeFolder();
    return home.empty() ? fs::path() : home / "Library" / "Application Support";
#else
    // XDG: an absolute $XDG_CONFIG_HOME, else ~/.config (a relative value is invalid by the specification)
    const char* config = getenv("XDG_CONFIG_HOME");
    if (config && config[0] == '/')
    {
        how = "$XDG_CONFIG_HOME";
        return fs::path(config);
    }
    how = "~/.config";
    const fs::path home = HomeFolder();
    return home.empty() ? fs::path() : home / ".config";
#endif
}

// The root, resolved once per process. false - none (the log says why)
bool Root(fs::path& out)
{
    std::lock_guard<std::mutex> guard(g_root_mutex);
    if (!g_root_resolved)
    {
        g_root_resolved = true;
        std::error_code ec;
        const xr_string from_command_line = CommandLineRoot();
        if (!from_command_line.empty())
        {
            g_root = fs::absolute(PathFromUtf8(from_command_line), ec);
            if (ec)
                g_root = PathFromUtf8(from_command_line);
            g_root = g_root.lexically_normal();
            Msg("* [addons] user data: the root is %s (-user_data)", PathToUtf8(g_root).c_str());
        }
        else
        {
            xr_string how;
            const fs::path base = PlatformBase(how);
            if (base.empty())
            {
                Msg("! [addons] user data: no folder of the user (%s is not set): addons.user_data and user_data_* "
                    "do not work; run the game with -user_data <folder>", how.c_str());
            }
            else
            {
                g_root = (base / kVendorFolder / kGameFolder).lexically_normal();
                Msg("* [addons] user data: the root is %s (%s, next to the data of the launcher)",
                    PathToUtf8(g_root).c_str(), how.c_str());
            }
        }
    }
    out = g_root;
    return !out.empty();
}

// The leftovers of crashes in the folder of an addon: "*.tmp" older than a day, once per addon and process. A younger
// one may be the write of another copy of the game in progress
void SweepOnce(const xr_string& id, const fs::path& folder)
{
    {
        std::lock_guard<std::mutex> guard(g_root_mutex);
        if (std::find(g_swept.begin(), g_swept.end(), id) != g_swept.end())
            return;
        g_swept.push_back(id);
    }
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || !EndsWith(PathToUtf8(it->path().filename()), kTemporarySuffix))
            continue;
        const auto written = fs::last_write_time(it->path(), entry_ec);
        if (entry_ec || now - written < kStaleTemporary)
            continue;
        if (fs::remove(it->path(), entry_ec))
            Msg("~ [addon:%s] user_data: removed %s, left by a write that did not finish", id.c_str(),
                PathToUtf8(it->path().filename()).c_str());
    }
}

// The folder of the addon: <root>/addons/<id>/. create - make it when missing. EError::Io - no root, or the folder
// cannot be made (logged)
EError AddonFolder(const xr_string& id, bool create, fs::path& out)
{
    fs::path root;
    if (!Root(root))
        return EError::Io;
    out = root / kAddonsFolder / id.c_str();
    std::error_code ec;
    if (create)
    {
        fs::create_directories(out, ec);
        if (ec)
        {
            Msg("! [addon:%s] user_data: cannot create the folder %s: %s", id.c_str(), PathToUtf8(out).c_str(),
                ErrorText(ec).c_str());
            return EError::Io;
        }
    }
    else if (!fs::is_directory(out, ec))
        return EError::None; // nothing there yet: read / list / remove find no file
    SweepOnce(id, out);
    return EError::None;
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

// The whole file into `out`; `size` - the size of the file whenever it exists (also when too big)
EError ReadUserFile(const xr_string& id, const fs::path& file, xr_string& out, u64& size)
{
    out.clear();
    size = 0;
#if defined(XR_PLATFORM_WINDOWS)
    // Shared for writing and deleting: a reader never stops the atomic replace of another copy of the game
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return EError::NotFound;
        LogOsFailure(id, "cannot read", file);
        return EError::Io;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length))
    {
        LogOsFailure(id, "cannot read", file);
        CloseHandle(handle);
        return EError::Io;
    }
    size = static_cast<u64>(length.QuadPart);
    if (size > kMaxFileSize)
    {
        CloseHandle(handle);
        return EError::TooBig;
    }
    out.resize(static_cast<size_t>(size));
    size_t done = 0;
    while (done < out.size())
    {
        DWORD read = 0;
        if (!::ReadFile(handle, &out[done], static_cast<DWORD>(out.size() - done), &read, nullptr))
        {
            LogOsFailure(id, "cannot read", file);
            CloseHandle(handle);
            out.clear();
            return EError::Io;
        }
        if (read == 0)
            break; // the file became shorter
        done += read;
    }
    CloseHandle(handle);
#else
    const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        if (errno == ENOENT || errno == ENOTDIR)
            return EError::NotFound;
        LogOsFailure(id, "cannot read", file);
        return EError::Io;
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
    {
        Msg("! [addon:%s] user_data: cannot read %s: not a file", id.c_str(), PathToUtf8(file).c_str());
        close(fd);
        return EError::Io;
    }
    size = static_cast<u64>(info.st_size);
    if (size > kMaxFileSize)
    {
        close(fd);
        return EError::TooBig;
    }
    out.resize(static_cast<size_t>(size));
    size_t done = 0;
    while (done < out.size())
    {
        const ssize_t read_now = read(fd, &out[done], out.size() - done);
        if (read_now < 0 && errno == EINTR)
            continue;
        if (read_now < 0)
        {
            LogOsFailure(id, "cannot read", file);
            close(fd);
            out.clear();
            return EError::Io;
        }
        if (read_now == 0)
            break; // the file became shorter
        done += static_cast<size_t>(read_now);
    }
    close(fd);
#endif
    out.resize(done);
    size = done;
    return EError::None;
}

// A temporary file of its own for every write: the process and a counter (two threads of one plugin, two copies of
// the game never share one)
fs::path TemporaryOf(const fs::path& folder, const xr_string& name)
{
    static std::atomic<u32> counter{ 0 };
#if defined(XR_PLATFORM_WINDOWS)
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    string128 suffix;
    xr_sprintf(suffix, ".%lu.%u%s", pid, static_cast<unsigned>(++counter), kTemporarySuffix);
    return folder / (name + suffix).c_str();
}

// Replaces folder/name with `data` atomically: a temporary file, onto the disk, then over the old file in one step
EError WriteUserFile(const xr_string& id, const fs::path& folder, const xr_string& name, const void* data, size_t size)
{
    const fs::path file = folder / name.c_str();
    const fs::path temporary = TemporaryOf(folder, name);
    const u8* bytes = static_cast<const u8*>(data);
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE handle =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        LogOsFailure(id, "cannot write", temporary);
        return EError::Io;
    }
    size_t done = 0;
    bool written = true;
    while (done < size)
    {
        DWORD now = 0;
        const DWORD part = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));
        if (!::WriteFile(handle, bytes + done, part, &now, nullptr) || now == 0)
        {
            written = false;
            break;
        }
        done += now;
    }
    // On the disk before the replace: else a power loss right after it may leave an empty file under the name
    if (written && !FlushFileBuffers(handle))
        written = false;
    const xr_string write_error = written ? xr_string() : LastErrorText();
    CloseHandle(handle);
    if (!written)
    {
        Msg("! [addon:%s] user_data: cannot write %s: %s", id.c_str(), PathToUtf8(temporary).c_str(),
            write_error.c_str());
        DeleteFileW(temporary.c_str());
        return EError::Io;
    }
    // A reader of another program without FILE_SHARE_DELETE (an antivirus, an indexer) holds the old file for a
    // moment: a few tries
    BOOL moved = FALSE;
    DWORD move_error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 10 && !moved; ++attempt)
    {
        moved = MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!moved)
        {
            move_error = GetLastError();
            if (move_error != ERROR_ACCESS_DENIED && move_error != ERROR_SHARING_VIOLATION &&
                move_error != ERROR_LOCK_VIOLATION)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!moved)
    {
        SetLastError(move_error); // the wait between the tries may have changed it
        LogOsFailure(id, "cannot replace", file);
        DeleteFileW(temporary.c_str());
        return EError::Io;
    }
#else
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        LogOsFailure(id, "cannot write", temporary);
        return EError::Io;
    }
    size_t done = 0;
    bool written = true;
    while (done < size)
    {
        const ssize_t now = write(fd, bytes + done, size - done);
        if (now < 0 && errno == EINTR)
            continue;
        if (now <= 0)
        {
            written = false;
            break;
        }
        done += static_cast<size_t>(now);
    }
    if (written)
    {
        int result;
        do
            result = fsync(fd);
        while (result != 0 && errno == EINTR);
        written = result == 0;
    }
    const xr_string write_error = written ? xr_string() : LastErrorText();
    close(fd);
    if (!written)
    {
        Msg("! [addon:%s] user_data: cannot write %s: %s", id.c_str(), PathToUtf8(temporary).c_str(),
            write_error.c_str());
        unlink(temporary.c_str());
        return EError::Io;
    }
    if (rename(temporary.c_str(), file.c_str()) != 0)
    {
        LogOsFailure(id, "cannot replace", file);
        unlink(temporary.c_str());
        return EError::Io;
    }
    // The rename itself onto the disk (the entry of the folder); best effort
    const int folder_fd = open(folder.c_str(), O_RDONLY | O_CLOEXEC);
    if (folder_fd >= 0)
    {
        fsync(folder_fd);
        close(folder_fd);
    }
#endif
    if (IsDebugLog())
        Msg("  [addon:%s] user_data: wrote %s (%u bytes)", id.c_str(), name.c_str(), static_cast<u32>(size));
    return EError::None;
}

EError RemoveUserFile(const xr_string& id, const fs::path& folder, const xr_string& name)
{
    const fs::path file = folder / name.c_str();
#if defined(XR_PLATFORM_WINDOWS)
    if (!DeleteFileW(file.c_str()))
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return EError::NotFound;
#else
    if (unlink(file.c_str()) != 0)
    {
        if (errno == ENOENT || errno == ENOTDIR)
            return EError::NotFound;
#endif
        LogOsFailure(id, "cannot remove", file);
        return EError::Io;
    }
    if (IsDebugLog())
        Msg("  [addon:%s] user_data: removed %s", id.c_str(), name.c_str());
    return EError::None;
}

// The files of the folder with valid names, sorted
EError ListUserFiles(const xr_string& id, const fs::path& folder, xr_vector<xr_string>& out)
{
    out.clear();
    std::error_code ec;
    if (!fs::is_directory(folder, ec))
        return EError::None;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec))
            continue;
        xr_string name = PathToUtf8(it->path().filename());
        if (IsValidName(name))
            out.push_back(std::move(name));
    }
    if (ec)
    {
        Msg("! [addon:%s] user_data: cannot list %s: %s", id.c_str(), PathToUtf8(folder).c_str(),
            ErrorText(ec).c_str());
        out.clear();
        return EError::Io;
    }
    std::sort(out.begin(), out.end());
    return EError::None;
}

// ---------------------------------------------------------------------------------------------
// Locks
// ---------------------------------------------------------------------------------------------

struct LockFile
{
#if defined(XR_PLATFORM_WINDOWS)
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool held() const { return handle != INVALID_HANDLE_VALUE; }
#else
    int fd = -1;
    bool held() const { return fd >= 0; }
#endif
};

// The lock of folder/name between the processes and the threads, waiting up to timeout_ms
EError AcquireLock(const xr_string& id, const fs::path& folder, const xr_string& name, u32 timeout_ms, LockFile& out)
{
    const fs::path file = folder / (name + kLockSuffix).c_str();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
#if defined(XR_PLATFORM_WINDOWS)
        // No sharing: any other open (another copy of the game, another thread) fails while it is held; deleted when
        // the handle closes, also when the process dies
        const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
        {
            out.handle = handle;
            return EError::None;
        }
        const DWORD code = GetLastError();
        // ERROR_ACCESS_DENIED: the file of the last holder is being deleted
        if (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED && code != ERROR_LOCK_VIOLATION)
        {
            LogOsFailure(id, "cannot lock", file);
            return EError::Io;
        }
#else
        // flock belongs to the open file description: a second open in this process conflicts too. The file stays
        // after the unlock: removing it would race with a process that has just opened it
        const int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            LogOsFailure(id, "cannot lock", file);
            return EError::Io;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            out.fd = fd;
            return EError::None;
        }
        const int code = errno;
        close(fd);
        if (code != EWOULDBLOCK && code != EAGAIN && code != EINTR)
        {
            errno = code;
            LogOsFailure(id, "cannot lock", file);
            return EError::Io;
        }
#endif
        if (std::chrono::steady_clock::now() >= deadline)
            return EError::Timeout;
        std::this_thread::sleep_for(kLockRetry);
    }
}

void ReleaseLock(LockFile& lock)
{
#if defined(XR_PLATFORM_WINDOWS)
    if (lock.handle != INVALID_HANDLE_VALUE)
        CloseHandle(lock.handle);
    lock.handle = INVALID_HANDLE_VALUE;
#else
    if (lock.fd >= 0)
    {
        flock(lock.fd, LOCK_UN);
        close(lock.fd);
    }
    lock.fd = -1;
#endif
}

// Released on every way out of a scope
struct ScopedLock
{
    LockFile file;
    ~ScopedLock() { ReleaseLock(file); }
};

// The locks plugins hold (user_data_lock .. user_data_unlock)
struct PluginLock
{
    GwpUserDataLock handle = GWP_INVALID_USER_DATA_LOCK;
    const GwpPlugin* plugin = nullptr;
    xr_string id;
    xr_string name;
    LockFile file;
};

std::mutex g_locks_mutex;
xr_vector<PluginLock> g_locks;
GwpUserDataLock g_next_lock = 1;

u32 LockTimeout(u32 timeout_ms) { return std::min<u32>(timeout_ms, GWP_USER_DATA_LOCK_TIMEOUT_MAX); }

// ---------------------------------------------------------------------------------------------
// Plugin API
// ---------------------------------------------------------------------------------------------

// An addon whose id is "core" (the loader accepts it) would share the folder of the scripts of the base: it gets no
// folder. Logged once per process
void LogCoreAddonRefused()
{
    static std::atomic<bool> logged{ false };
    if (!logged.exchange(true))
        Msg("! [addon:%s] user_data: the id '%s' is the folder of the scripts of the base - an addon with this id gets "
            "no user data (rename the addon)", kCoreOwner, kCoreOwner);
}

// The id of the addon of a plugin; false - not a plugin of a known addon, or the addon "core"
bool PluginOwner(const GwpPlugin* self, xr_string& id)
{
    if (!self)
        return false;
    const pcstr owner = PluginAddonId(self);
    if (!owner || !IsValidOwner(owner))
        return false;
    if (std::strcmp(owner, kCoreOwner) == 0)
    {
        LogCoreAddonRefused();
        return false;
    }
    id = owner;
    return true;
}

// Copies text into out (zero-terminated, cut to cap - 1); the full length
uint32_t CopyOut(const xr_string& text, char* out, uint32_t cap)
{
    if (out && cap > 0)
    {
        const size_t count = std::min<size_t>(text.size(), cap - 1);
        std::memcpy(out, text.data(), count);
        out[count] = 0;
    }
    return static_cast<uint32_t>(text.size());
}

xr_string FolderText(const fs::path& folder)
{
    xr_string text = PathToUtf8(folder);
    if (text.empty() || text.back() != static_cast<char>(fs::path::preferred_separator))
        text += static_cast<char>(fs::path::preferred_separator);
    return text;
}

uint32_t GWP_CALL ApiUserDataDir(const GwpPlugin* self, char* out, uint32_t cap)
{
    xr_string id;
    fs::path folder;
    if (!PluginOwner(self, id) || AddonFolder(id, true, folder) != EError::None)
    {
        CopyOut({}, out, cap);
        return 0;
    }
    return CopyOut(FolderText(folder), out, cap);
}

GwpResult GWP_CALL ApiUserDataRead(const GwpPlugin* self, const char* name, void* out, uint32_t cap, uint32_t* size)
{
    xr_string id;
    if (size)
        *size = 0;
    if (!PluginOwner(self, id) || !name || !IsValidName(name) || !size || (!out && cap > 0))
        return GWP_ERROR_INVALID_ARGUMENT;
    fs::path folder;
    if (AddonFolder(id, false, folder) != EError::None)
        return GWP_ERROR;
    xr_string data;
    u64 file_size = 0;
    const EError error = ReadUserFile(id, folder / name, data, file_size);
    *size = static_cast<uint32_t>(std::min<u64>(file_size, 0xFFFFFFFFu));
    if (error != EError::None)
        return ResultOf(error);
    if (data.size() > cap)
        return GWP_ERROR_INVALID_ARGUMENT; // *size says how much room the file needs
    if (!data.empty())
        std::memcpy(out, data.data(), data.size());
    return GWP_OK;
}

GwpResult GWP_CALL ApiUserDataWrite(const GwpPlugin* self, const char* name, const void* data, uint32_t size)
{
    xr_string id;
    if (!PluginOwner(self, id) || !name || !IsValidName(name) || (!data && size > 0))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (size > kMaxFileSize)
        return GWP_ERROR_NOT_SUPPORTED;
    fs::path folder;
    if (AddonFolder(id, true, folder) != EError::None)
        return GWP_ERROR;
    return ResultOf(WriteUserFile(id, folder, name, data, size));
}

GwpResult GWP_CALL ApiUserDataLock(const GwpPlugin* self, const char* name, uint32_t timeout_ms,
    GwpUserDataLock* out_lock)
{
    if (out_lock)
        *out_lock = GWP_INVALID_USER_DATA_LOCK;
    xr_string id;
    if (!PluginOwner(self, id) || !name || !IsValidName(name) || !out_lock)
        return GWP_ERROR_INVALID_ARGUMENT;
    fs::path folder;
    if (AddonFolder(id, true, folder) != EError::None)
        return GWP_ERROR;
    LockFile file;
    // The wait runs without g_locks_mutex: another thread may unlock meanwhile
    const EError error = AcquireLock(id, folder, name, LockTimeout(timeout_ms), file);
    if (error != EError::None)
        return ResultOf(error);
    std::lock_guard<std::mutex> guard(g_locks_mutex);
    PluginLock& lock = g_locks.emplace_back();
    lock.handle = g_next_lock++;
    lock.plugin = self;
    lock.id = id;
    lock.name = name;
    lock.file = file;
    *out_lock = lock.handle;
    return GWP_OK;
}

GwpResult GWP_CALL ApiUserDataUnlock(const GwpPlugin* self, GwpUserDataLock handle)
{
    LockFile file;
    {
        std::lock_guard<std::mutex> guard(g_locks_mutex);
        const auto it = std::find_if(
            g_locks.begin(), g_locks.end(), [handle](const PluginLock& lock) { return lock.handle == handle; });
        if (handle == GWP_INVALID_USER_DATA_LOCK || it == g_locks.end())
            return GWP_ERROR_NOT_FOUND;
        if (it->plugin != self)
            return GWP_ERROR_ACCESS_DENIED;
        file = it->file;
        g_locks.erase(it);
    }
    ReleaseLock(file);
    return GWP_OK;
}

GwpResult GWP_CALL ApiUserDataRemove(const GwpPlugin* self, const char* name)
{
    xr_string id;
    if (!PluginOwner(self, id) || !name || !IsValidName(name))
        return GWP_ERROR_INVALID_ARGUMENT;
    fs::path folder;
    if (AddonFolder(id, false, folder) != EError::None)
        return GWP_ERROR;
    return ResultOf(RemoveUserFile(id, folder, name));
}

uint32_t GWP_CALL ApiUserDataList(const GwpPlugin* self, char* out, uint32_t cap)
{
    xr_string id;
    fs::path folder;
    xr_vector<xr_string> names;
    if (!PluginOwner(self, id) || AddonFolder(id, false, folder) != EError::None ||
        ListUserFiles(id, folder, names) != EError::None)
    {
        CopyOut({}, out, cap);
        return 0;
    }
    xr_string text;
    for (const xr_string& name : names)
    {
        if (!text.empty())
            text += '\n';
        text += name;
    }
    return CopyOut(text, out, cap);
}

// ---------------------------------------------------------------------------------------------
// Lua: _gw_internal.user_data_open(id) -> the object of addons.user_data
// ---------------------------------------------------------------------------------------------

constexpr pcstr kStoreMeta = "gw.addons.user_data"; // the metatable of the objects, in the registry of the state
constexpr pcstr kFacadeModule = "addons"; // addons.script: the facade, not the caller

struct LuaStore
{
    char id[kMaxIdLength + 1];
};

// The module of a chunk "@<path>\<module>.script", lower case; "" - not a script file
xr_string ModuleOfSource(pcstr source)
{
    if (!source || source[0] != '@')
        return {};
    std::string_view path(source + 1);
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    xr_string module(path.substr(0, dot));
    std::transform(module.begin(), module.end(), module.begin(),
        [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return module;
}

// The addon whose code calls: the first Lua function on the stack outside the facade (addons.script), its module
// (scripts\<module>.script) owned by an addon (AddonScriptModuleOwner: the layer of the addon serves it and it
// replaces no script of the base) - that addon; a module of the base, a script that replaces one, no Lua caller -
// "core". A tail call (return addons.user_data(...)) leaves no frame of the caller: its caller counts.
// "" - a module of the addon with the id "core" (no folder, LogCoreAddonRefused)
xr_string CallerOwner(lua_State* L)
{
    lua_Debug ar;
    for (int level = 1; lua_getstack(L, level, &ar); ++level)
    {
        if (!lua_getinfo(L, "S", &ar))
            continue;
        const xr_string module = ModuleOfSource(ar.source);
        if (module.empty() || module == kFacadeModule)
            continue; // a C function, a chunk of a string, the facade
        const xr_string owner = AddonScriptModuleOwner(module.c_str());
        if (owner == kCoreOwner)
            return {};
        return owner.empty() ? xr_string(kCoreOwner) : owner;
    }
    return kCoreOwner;
}

int PushFailure(lua_State* L, bool nil_first, EError error)
{
    if (nil_first)
        lua_pushnil(L);
    else
        lua_pushboolean(L, 0);
    lua_pushstring(L, ErrorName(error));
    return 2;
}

// The object of the method call (store:read(...)); a Lua error for anything else - before any C++ object is made
LuaStore* CheckStore(lua_State* L, pcstr method)
{
    LuaStore* store = static_cast<LuaStore*>(luaL_testudata(L, 1, kStoreMeta));
    if (!store)
        luaL_error(L, "user_data:%s(...): call it as a method of the object of addons.user_data (store:%s)", method,
            method);
    return store;
}

// The name argument: a valid name or nullptr
pcstr NameArgument(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TSTRING)
        return nullptr;
    size_t length = 0;
    const pcstr name = lua_tolstring(L, index, &length);
    return length == xr_strlen(name) && IsValidName(std::string_view(name, length)) ? name : nullptr;
}

// store:id() -> the id of the addon the folder belongs to
int LuaStoreId(lua_State* L)
{
    lua_pushstring(L, CheckStore(L, "id")->id);
    return 1;
}

// store:dir() -> the absolute path of the folder (UTF-8, a trailing separator) | nil, "io"
int LuaStoreDir(lua_State* L)
{
    const LuaStore* store = CheckStore(L, "dir");
    fs::path folder;
    const EError error = AddonFolder(store->id, true, folder);
    if (error != EError::None)
        return PushFailure(L, true, error);
    lua_pushstring(L, FolderText(folder).c_str());
    return 1;
}

// store:read(name) -> text | nil, "not_found" | "too_big" | "io" | "invalid_argument"
int LuaStoreRead(lua_State* L)
{
    const LuaStore* store = CheckStore(L, "read");
    const pcstr name = NameArgument(L, 2);
    if (!name)
        return PushFailure(L, true, EError::InvalidArgument);
    fs::path folder;
    EError error = AddonFolder(store->id, false, folder);
    if (error != EError::None)
        return PushFailure(L, true, error);
    xr_string data;
    u64 size = 0;
    error = ReadUserFile(store->id, folder / name, data, size);
    if (error != EError::None)
        return PushFailure(L, true, error);
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

// store:write(name, text) -> true | false, "invalid_argument" | "too_big" | "io"
int LuaStoreWrite(lua_State* L)
{
    const LuaStore* store = CheckStore(L, "write");
    const pcstr name = NameArgument(L, 2);
    if (!name || lua_type(L, 3) != LUA_TSTRING)
        return PushFailure(L, false, EError::InvalidArgument);
    size_t size = 0;
    const pcstr data = lua_tolstring(L, 3, &size);
    if (size > kMaxFileSize)
        return PushFailure(L, false, EError::TooBig);
    fs::path folder;
    EError error = AddonFolder(store->id, true, folder);
    if (error == EError::None)
        error = WriteUserFile(store->id, folder, name, data, size);
    if (error != EError::None)
        return PushFailure(L, false, error);
    lua_pushboolean(L, 1);
    return 1;
}

// The message handler of the protected call of update: the message with the Lua stack
int LuaUpdateTraceback(lua_State* L)
{
    const pcstr message = lua_tostring(L, 1);
    luaL_traceback(L, L, message && message[0] ? message : "(a Lua error without a message)", 1);
    return 1;
}

// store:update(name, fn[, timeout_ms]) -> true | false, "invalid_argument" | "timeout" | "too_big" | "io" | "callback".
// Under the lock of the name: old = the file (nil - none), new = fn(old); a string is written atomically, nil leaves
// the file as it is. fn runs under lua_pcall: its error is logged with the stack and answers "callback"
int LuaStoreUpdate(lua_State* L)
{
    const LuaStore* store = CheckStore(L, "update");
    const pcstr name = NameArgument(L, 2);
    if (!name || lua_type(L, 3) != LUA_TFUNCTION)
        return PushFailure(L, false, EError::InvalidArgument);
    u32 timeout_ms = GWP_USER_DATA_LOCK_TIMEOUT_DEFAULT;
    if (!lua_isnoneornil(L, 4))
    {
        const lua_Number asked = lua_type(L, 4) == LUA_TNUMBER ? lua_tonumber(L, 4) : -1;
        if (!(asked >= 0)) // also NaN
            return PushFailure(L, false, EError::InvalidArgument);
        timeout_ms = LockTimeout(static_cast<u32>(std::min<lua_Number>(asked, GWP_USER_DATA_LOCK_TIMEOUT_MAX)));
    }
    lua_settop(L, 3); // store, name, fn: the stack of the call below starts at 4
    fs::path folder;
    EError error = AddonFolder(store->id, true, folder);
    if (error != EError::None)
        return PushFailure(L, false, error);
    ScopedLock lock;
    error = AcquireLock(store->id, folder, name, timeout_ms, lock.file);
    if (error != EError::None)
    {
        if (error == EError::Timeout)
            Msg("~ [addon:%s] user_data: update '%s': the lock is held by another (%u ms), nothing is written",
                store->id, name, timeout_ms);
        return PushFailure(L, false, error);
    }
    xr_string old;
    u64 size = 0;
    error = ReadUserFile(store->id, folder / name, old, size);
    if (error != EError::None && error != EError::NotFound)
        return PushFailure(L, false, error);
    lua_pushcfunction(L, &LuaUpdateTraceback); // 4
    lua_pushvalue(L, 3); // 5: fn
    if (error == EError::NotFound)
        lua_pushnil(L);
    else
        lua_pushlstring(L, old.data(), old.size());
    if (lua_pcall(L, 1, 1, 4) != 0)
    {
        const pcstr message = lua_tostring(L, -1);
        Msg("! [addon:%s] user_data: update '%s': the function failed, the file stays as it was:\n%s", store->id, name,
            message ? message : "(no message)");
        lua_settop(L, 3);
        return PushFailure(L, false, EError::Callback);
    }
    // 5: the result
    if (lua_isnil(L, 5))
    {
        lua_settop(L, 3);
        lua_pushboolean(L, 1);
        return 1;
    }
    if (lua_type(L, 5) != LUA_TSTRING)
    {
        Msg("! [addon:%s] user_data: update '%s': the function returned a %s, not a string or nil: nothing is written",
            store->id, name, lua_typename(L, lua_type(L, 5)));
        lua_settop(L, 3);
        return PushFailure(L, false, EError::InvalidArgument);
    }
    size_t length = 0;
    const pcstr data = lua_tolstring(L, 5, &length);
    error = length > kMaxFileSize ? EError::TooBig : WriteUserFile(store->id, folder, name, data, length);
    lua_settop(L, 3);
    if (error != EError::None)
        return PushFailure(L, false, error);
    lua_pushboolean(L, 1);
    return 1;
}

// store:remove(name) -> true | false, "not_found" | "invalid_argument" | "io"
int LuaStoreRemove(lua_State* L)
{
    const LuaStore* store = CheckStore(L, "remove");
    const pcstr name = NameArgument(L, 2);
    if (!name)
        return PushFailure(L, false, EError::InvalidArgument);
    fs::path folder;
    EError error = AddonFolder(store->id, false, folder);
    if (error == EError::None)
        error = RemoveUserFile(store->id, folder, name);
    if (error != EError::None)
        return PushFailure(L, false, error);
    lua_pushboolean(L, 1);
    return 1;
}

// store:list() -> { name, ... } sorted (empty on a failure, logged)
int LuaStoreList(lua_State* L)
{
    const LuaStore* store = CheckStore(L, "list");
    fs::path folder;
    xr_vector<xr_string> names;
    if (AddonFolder(store->id, false, folder) == EError::None)
        ListUserFiles(store->id, folder, names);
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i)
    {
        lua_pushlstring(L, names[i].data(), names[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int LuaStoreToString(lua_State* L)
{
    lua_pushfstring(L, "user_data(%s)", CheckStore(L, "__tostring")->id);
    return 1;
}

// _gw_internal.user_data_open([id]) -> the object | nil, "access_denied" | "invalid_argument". The owner is the
// addon of the calling code (CallerOwner); id, when given, must be that one
int LuaUserDataOpen(lua_State* L)
{
    if (!lua_isnoneornil(L, 1) && lua_type(L, 1) != LUA_TSTRING)
        return PushFailure(L, true, EError::InvalidArgument);
    const xr_string owner = CallerOwner(L);
    if (owner.empty())
    {
        LogCoreAddonRefused();
        return PushFailure(L, true, EError::AccessDenied);
    }
    const pcstr asked = lua_isnoneornil(L, 1) ? nullptr : lua_tostring(L, 1);
    if (asked && owner != asked)
    {
        Msg("! [addon:%s] addons.user_data('%s'): an addon reaches only its own folder - call addons.user_data() or "
            "addons.user_data(ctx.id) from a script of the addon (not as a tail call: return addons.user_data(...))",
            owner.c_str(), asked);
        return PushFailure(L, true, EError::AccessDenied);
    }
    if (!IsValidOwner(owner))
        return PushFailure(L, true, EError::InvalidArgument);
    LuaStore* store = static_cast<LuaStore*>(lua_newuserdata(L, sizeof(LuaStore)));
    xr_strcpy(store->id, owner.c_str());
    luaL_getmetatable(L, kStoreMeta);
    lua_setmetatable(L, -2);
    return 1;
}
} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CAddonUserDataScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CAddonUserDataScript::script_register(lua_State* L)
{
    // The metatable of the objects: the methods through __index, closed for getmetatable / setmetatable
    luaL_newmetatable(L, kStoreMeta);
    const luaL_Reg methods[] = {
        { "id", &LuaStoreId },
        { "dir", &LuaStoreDir },
        { "read", &LuaStoreRead },
        { "write", &LuaStoreWrite },
        { "update", &LuaStoreUpdate },
        { "remove", &LuaStoreRemove },
        { "list", &LuaStoreList },
    };
    lua_createtable(L, 0, static_cast<int>(std::size(methods)));
    for (const luaL_Reg& method : methods)
    {
        lua_pushcfunction(L, method.func);
        lua_setfield(L, -2, method.name);
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &LuaStoreToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "user_data");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    // _gw_internal is shared with addon_event_bus.cpp, addon_storage.cpp, addon_lua_entry.cpp,
    // gw_condlist_native.cpp: the first one creates it. Internal to addons.script (the API is addons.user_data)
    lua_getglobal(L, "_gw_internal");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_gw_internal");
    }
    lua_pushcfunction(L, &LuaUserDataOpen);
    lua_setfield(L, -2, "user_data_open");
    lua_pop(L, 1);
}

void FillEngineApi(GwpEngineApi& api)
{
    api.user_data_dir = &ApiUserDataDir;
    api.user_data_read = &ApiUserDataRead;
    api.user_data_write = &ApiUserDataWrite;
    api.user_data_lock = &ApiUserDataLock;
    api.user_data_unlock = &ApiUserDataUnlock;
    api.user_data_remove = &ApiUserDataRemove;
    api.user_data_list = &ApiUserDataList;
}

void ReleasePluginLocks(const GwpPlugin* plugin)
{
    xr_vector<PluginLock> released;
    {
        std::lock_guard<std::mutex> guard(g_locks_mutex);
        for (auto it = g_locks.begin(); it != g_locks.end();)
        {
            if (it->plugin != plugin)
            {
                ++it;
                continue;
            }
            released.push_back(*it);
            it = g_locks.erase(it);
        }
    }
    for (PluginLock& lock : released)
    {
        Msg("~ [addon:%s] user_data: the lock of '%s' was still held when the plugin stopped - released",
            lock.id.c_str(), lock.name.c_str());
        ReleaseLock(lock.file);
    }
}
} // namespace gw::addons::user_data
