#include "stdafx.h"
#include "FileCRC32.h"

#include <mutex>

namespace
{
// 50z2 (plans/optimization_mt/50z2-startup-thm-shaders-probes-preview.md): the CRC of an #include by its full path.
// The shader cache validates every shader with the CRC of its file and all its includes, and the includes (18-40 for a
// pixel shader) were opened and hashed again for every shader. getFileCrc32MemoIncludes keeps the CRC of each include
// (with its own includes) for the process; resetFileCrc32IncludeMemo drops it when the files may have changed (device
// creation and reset, level load). The CRC values are the same as getFileCrc32 gives for unchanged files.
std::mutex s_include_memo_lock;
xr_unordered_map<xr_string, u32> s_include_memo;

void fileCrc32(IReader* F, pcstr filePath, u32& outCrc, bool parseIncludes, bool memoIncludes)
{
    outCrc = crc32(F->pointer(), F->length(), outCrc);
    string4096 str;
    if (parseIncludes)
    {
        while (!F->eof())
        {
            F->r_string(str, sizeof str);
            _Trim(str);
            if (str[0] && _Trim(str)[0] == '#' && strstr(str, "#include")) // handle includes
            {
                string_path inc_name;
                R_ASSERT(filePath && filePath[0]);
                if (_GetItem(str, 1, inc_name, '"'))
                {
                    xr_strlwr(inc_name);
                    string_path fn;
                    strconcat(sizeof fn, fn, filePath, inc_name);
                    if (memoIncludes)
                    {
                        std::lock_guard<std::mutex> guard(s_include_memo_lock);
                        const auto it = s_include_memo.find(fn);
                        if (it != s_include_memo.end())
                        {
                            outCrc += it->second; // as addFileCrc32 below adds it
                            continue;
                        }
                    }
                    const xr_string inc_path = EFS_Utils::ExtractFilePath(fn);
                    IReader* I = FS.r_open(fn);
                    R_ASSERT3(I, "Can't find include file:", inc_name);
                    u32 incCrc = 0;
                    fileCrc32(I, inc_path.c_str(), incCrc, true, memoIncludes);
                    FS.r_close(I);
                    if (memoIncludes)
                    {
                        std::lock_guard<std::mutex> guard(s_include_memo_lock);
                        s_include_memo.emplace(fn, incCrc);
                    }
                    outCrc += incCrc;
                }
            }
        }
    }
}
} // namespace

void getFileCrc32(IReader* F, pcstr filePath, u32& outCrc, bool parseIncludes)
{
    fileCrc32(F, filePath, outCrc, parseIncludes, false);
}

void addFileCrc32(IReader* F, pcstr filePath, u32& outCrc, bool parseIncludes)
{
    u32 fileCrc = 0;
    getFileCrc32(F, filePath, fileCrc, parseIncludes);
    outCrc += fileCrc;
}

void getFileCrc32MemoIncludes(IReader* F, pcstr filePath, u32& outCrc)
{
    fileCrc32(F, filePath, outCrc, true, true);
}

void resetFileCrc32IncludeMemo()
{
    std::lock_guard<std::mutex> guard(s_include_memo_lock);
    s_include_memo.clear();
}
