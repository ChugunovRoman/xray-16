#include "StdAfx.h"

// ImGui tools of the addons: the registry and the host (plans/addons_api_support/20-imgui-addon-tools.md). See
// addon_imgui_tools.h.

#include "addon_imgui_tools.h"
#include "addon_imgui_core.h"
#include "addon_imgui_lua.h"
#include "addon_host.h"

#include "xrEngine/IGame_Level.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

class CActor;
extern CActor* g_actor;

namespace gw::addons::imgui_tools
{
int g_enabled = 1;

namespace
{
constexpr pcstr kSettingsType = "GWAddonTool";              // imgui.ini: [GWAddonTool][<owner>/<id>] Open=1
constexpr pcstr kMenuStringId = "st_imgui_menu_addons";     // the menu "Addons"
constexpr pcstr kMenuFallback = "Addons";
constexpr pcstr kFailedStringId = "st_imgui_tool_failed";   // "(error)" after the item of a failed tool
constexpr pcstr kFailedFallback = "(error)";

struct Entry
{
    ToolDef def;
    std::unique_ptr<ToolCallbacks> callbacks;
    xr_string key;          // "<owner>/<id>": imgui.ini
    xr_string title;        // translated
    xr_vector<xr_string> menu; // the levels of the submenu, translated
    xr_string tooltip;      // translated
    xr_string window_name;  // "<title>###gw.<owner>.<id>"
    xr_string item_id;      // "###gw.<owner>.<id>": the id of the menu item (the label changes with "(error)")
    xr_string reason;       // failed: the first line of the error
    u32 warned = 0;         // imgui::EWarnOnce bits
    u32 seq = 0;            // the order of the registration
    bool want_open = false; // asked to be open (the menu, set_open, the saved flag)
    bool open = false;      // on_open passed: drawn
    bool first = false;     // the next draw is the first after the open
    bool failed = false;
    bool dead = false;      // removed: freed when no loop and no callback runs (Purge)
    bool release_alive = true;
};

xr_vector<Entry*>* g_entries = nullptr;
u32 g_seq = 0;
// The host loops over the entries or a callback of a tool runs: a removed entry is only marked dead then
int g_busy = 0;
bool g_has_dead = false;
// imgui.ini: <owner>/<id> -> open. Lives as long as the process: imgui.ini is written last in CRenderDevice::DestroyImGui
// (Device_imgui.cpp), after the game persistent (the host) and the host of the addons are gone - the handler must still
// have the lines then
xr_unordered_map<xr_string, bool>* g_saved_open = nullptr;
bool g_handler_installed = false;
bool g_ini_dirty = false;         // MarkIniSettingsDirty on the main thread (OnFrame / the frame of the host)
int g_requested_state = -1;       // visible_state asked by Lua (RequestEditorState / SetOpen show)
// The labels of the menu, translated (LocalizationChanged makes them again)
xr_string* g_menu_label = nullptr;
xr_string* g_failed_label = nullptr;

struct Busy
{
    Busy() { ++g_busy; }
    ~Busy() { --g_busy; }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
};

xr_unordered_map<xr_string, bool>& SavedOpen()
{
    if (!g_saved_open)
        g_saved_open = xr_new<xr_unordered_map<xr_string, bool>>();
    return *g_saved_open;
}

xr_string Translate(const xr_string& text)
{
    if (text.empty())
        return text;
    const shared_str translated = StringTable().translate(text.c_str());
    return translated.c_str() ? xr_string(translated.c_str()) : text;
}

// A string id with an English fallback when the string is not there (translate gives the id back)
xr_string TranslateOr(pcstr id, pcstr fallback)
{
    const shared_str translated = StringTable().translate(id);
    if (!translated.c_str() || !xr_strcmp(translated.c_str(), id))
        return fallback;
    return translated.c_str();
}

void MakeLabels()
{
    if (!g_menu_label)
        g_menu_label = xr_new<xr_string>();
    if (!g_failed_label)
        g_failed_label = xr_new<xr_string>();
    // "###" keeps the id of the menu "Addons" when the language changes (an open menu stays open)
    *g_menu_label = TranslateOr(kMenuStringId, kMenuFallback) + "###gw.addon_tools";
    *g_failed_label = TranslateOr(kFailedStringId, kFailedFallback);
}

void SplitMenu(const xr_string& menu, xr_vector<xr_string>& levels)
{
    levels.clear();
    size_t start = 0;
    while (start <= menu.size())
    {
        size_t end = menu.find('/', start);
        if (end == xr_string::npos)
            end = menu.size();
        if (end > start)
            levels.emplace_back(menu.substr(start, end - start));
        start = end + 1;
    }
}

void TranslateEntry(Entry& entry)
{
    entry.title = Translate(entry.def.title.empty() ? entry.def.id : entry.def.title);
    if (entry.title.empty())
        entry.title = entry.def.id;
    SplitMenu(entry.def.menu.empty() ? entry.def.owner : entry.def.menu, entry.menu);
    for (xr_string& level : entry.menu)
        level = Translate(level);
    entry.tooltip = Translate(entry.def.tooltip);
    entry.item_id = "###gw." + entry.def.owner + "." + entry.def.id;
    entry.window_name = entry.title + entry.item_id;
}

bool ValidId(const xr_string& id)
{
    if (id.size() < 2 || id.size() > kMaxIdLength)
        return false;
    for (const char c : id)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    }
    return true;
}

// The owner goes into imgui.ini ("[GWAddonTool][<owner>/<id>]") and into the id of the window: any addon id (UTF-8 too),
// without the characters of that syntax
bool ValidOwner(const xr_string& owner)
{
    if (owner.empty() || owner.size() > kMaxIdLength)
        return false;
    for (const char c : owner)
    {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '[' || c == ']' || c == '#')
            return false;
    }
    return true;
}

u32 MenuDepth(const xr_string& menu)
{
    xr_vector<xr_string> levels;
    SplitMenu(menu, levels);
    return static_cast<u32>(levels.size());
}

bool ValidSize(const float (&size)[2]) { return std::isfinite(size[0]) && std::isfinite(size[1]) && size[0] >= 0.f && size[1] >= 0.f; }

Entry* Find(pcstr id)
{
    if (!g_entries || !id)
        return nullptr;
    for (Entry* entry : *g_entries)
    {
        if (!entry->dead && entry->def.id == id)
            return entry;
    }
    return nullptr;
}

bool Visible(const Entry& entry) { return !entry.dead && (!entry.def.dev_only || DevToolsAllowed()); }

void SaveOpen(Entry& entry, bool open)
{
    if (entry.def.no_save_open)
        return;
    auto& saved = SavedOpen();
    const auto it = saved.find(entry.key);
    const bool was = it != saved.end() && it->second;
    if (was == open)
        return;
    if (open)
        saved[entry.key] = true;
    else
        saved.erase(it);
    g_ini_dirty = true;
}

// The entry is freed: no loop and no callback runs (else it is only dead, Purge frees it)
void Free(Entry* entry)
{
    if (entry->callbacks)
        entry->callbacks->Release(entry->release_alive);
    xr_delete(entry);
}

void Purge()
{
    if (g_busy || !g_has_dead || !g_entries)
        return;
    g_has_dead = false;
    for (auto it = g_entries->begin(); it != g_entries->end();)
    {
        Entry* entry = *it;
        if (!entry->dead)
        {
            ++it;
            continue;
        }
        it = g_entries->erase(it);
        Free(entry);
    }
}

void MarkDead(Entry& entry, bool alive)
{
    entry.dead = true;
    entry.want_open = false;
    entry.open = false;
    entry.release_alive = alive;
    g_has_dead = true;
}

// The window closed (the cross, the menu, set_open(false), unregister, an error of draw): on_close
void Close(Entry& entry)
{
    entry.open = false;
    entry.first = false;
    Busy busy;
    entry.callbacks->OnClose(entry.def);
}

// on_open of the tools asked to open (the frame of the host, before the windows)
void OpenAsked()
{
    Busy busy;
    for (size_t i = 0; i < g_entries->size(); ++i)
    {
        Entry& entry = *(*g_entries)[i];
        if (!entry.want_open || entry.open || !Visible(entry))
            continue;
        xr_string error;
        const bool ok = entry.callbacks->OnOpen(entry.def, error);
        // A failed on_open forgets the open flag also when its tool is gone (a plugin that crashed in on_open is
        // stopped, its tools removed): the window would open by itself after plugin_reload / the next start and crash
        // the plugin again (review 2026-11-18)
        if (!ok)
            SaveOpen(entry, false);
        if (entry.dead)
            continue; // on_open removed its tool
        if (!ok)
        {
            entry.want_open = false;
            entry.failed = true;
            entry.reason = error.substr(0, error.find('\n'));
            SaveOpen(entry, false);
            continue;
        }
        if (!entry.want_open)
            continue; // on_open closed it again
        entry.open = true;
        entry.first = true;
    }
}

bool InGame() { return g_pGameLevel != nullptr && g_actor != nullptr; }

void DrawTool(Entry& entry, bool light, ImGuiWindowFlags light_flags)
{
    ImGuiWindowFlags flags = imgui::TranslateWindowFlags(entry.def.window_flags);
    if (light)
        flags |= light_flags;
    else
    {
        if (entry.def.size[0] > 0.f || entry.def.size[1] > 0.f)
            ImGui::SetNextWindowSize(ImVec2(entry.def.size[0], entry.def.size[1]), ImGuiCond_FirstUseEver);
        if (entry.def.pos[0] >= 0.f && entry.def.pos[1] >= 0.f)
            ImGui::SetNextWindowPos(ImVec2(entry.def.pos[0], entry.def.pos[1]), ImGuiCond_FirstUseEver);
    }
    if (entry.def.min_size[0] > 0.f || entry.def.min_size[1] > 0.f)
        ImGui::SetNextWindowSizeConstraints(ImVec2(entry.def.min_size[0], entry.def.min_size[1]), ImVec2(FLT_MAX, FLT_MAX));

    bool keep_open = true;
    const bool shown = ImGui::Begin(entry.window_name.c_str(), light ? nullptr : &keep_open, flags);
    bool ok = true;
    xr_string error;
    if (shown)
    {
        ImGuiErrorRecoveryState state;
        imgui::StoreState(state);
        imgui::DrawFrame frame;
        frame.owner = entry.def.owner.c_str();
        frame.id = entry.def.id.c_str();
        frame.warned = &entry.warned;
        {
            imgui::DrawScope scope(frame);
            FrameInfo info;
            info.light = light;
            info.first = entry.first;
            info.in_game = InGame();
            info.dt = Device.fTimeDeltaReal;
            entry.first = false;
            ok = entry.callbacks->Draw(entry.def, info, error);
            // What draw left open: the shadow stack knows the pairs, closes them by their End* (in the reverse order)
            imgui::EScope innermost = imgui::EScope::Count;
            const u32 closed = imgui::CloseAll(frame, &innermost);
            if (closed && ok)
            {
                imgui::WarnOnce(imgui::kWarnUnclosed, "%s without %s (%u pair(s) left open), closed by the engine "
                    "(this warning is shown once)", imgui::ScopeBeginName(innermost), imgui::ScopeEndName(innermost),
                    closed);
            }
            // The safety net of ImGui: after an error the stacks may be anything
            if (imgui::RecoverState(state) && ok)
            {
                imgui::WarnOnce(imgui::kWarnUnclosed, "the stack of ImGui was left unbalanced, fixed by the engine "
                    "(this warning is shown once)");
            }
        }
    }
    ImGui::End();
    // A failed draw forgets the open flag also when its tool is gone: a plugin that crashed in draw is stopped and its
    // tools removed - with the flag kept the window would open by itself after plugin_reload / the next start and crash
    // the plugin again (review 2026-11-18). An error of Lua closes the window below and forgets it the same way
    if (!ok)
        SaveOpen(entry, false);
    if (entry.dead)
        return; // draw removed its tool (or its plugin crashed)
    if (!ok)
    {
        // The callbacks logged the error: the window closes, the item of the menu shows "(error)"
        entry.want_open = false;
        entry.failed = true;
        entry.reason = error.substr(0, error.find('\n'));
        if (entry.open)
            Close(entry);
        return;
    }
    if (!keep_open && entry.open)
    {
        entry.want_open = false;
        SaveOpen(entry, false);
        Close(entry);
    }
}

// The menu "Addons": the visible tools by the levels of their menu, then order, then title
void ShowMenu(xr_vector<Entry*>& tools)
{
    std::sort(tools.begin(), tools.end(), [](const Entry* a, const Entry* b) {
        const size_t n = std::min(a->menu.size(), b->menu.size());
        for (size_t i = 0; i < n; ++i)
        {
            const int c = xr_strcmp(a->menu[i].c_str(), b->menu[i].c_str());
            if (c)
                return c < 0;
        }
        if (a->menu.size() != b->menu.size())
            return a->menu.size() < b->menu.size(); // the items of a menu above its submenus
        if (a->def.order != b->def.order)
            return a->def.order < b->def.order;
        const int c = xr_strcmp(a->title.c_str(), b->title.c_str());
        return c ? c < 0 : a->seq < b->seq;
    });

    // The submenus open now: their names (copies: a callback of a click may rename a tool) and whether BeginMenu
    // returned true (a child is tried only in an open parent)
    xr_vector<xr_string> path;
    xr_vector<bool> opened;
    xr_string label;
    for (Entry* entry : tools)
    {
        size_t common = 0;
        while (common < path.size() && common < entry->menu.size() && path[common] == entry->menu[common])
            ++common;
        while (path.size() > common)
        {
            if (opened.back())
                ImGui::EndMenu();
            path.pop_back();
            opened.pop_back();
        }
        while (path.size() < entry->menu.size())
        {
            const bool parent = opened.empty() || opened.back();
            const xr_string name = entry->menu[path.size()];
            opened.push_back(parent && ImGui::BeginMenu(name.c_str()));
            path.push_back(name);
        }
        if (!opened.empty() && !opened.back())
            continue;
        label = entry->title;
        if (entry->failed)
        {
            label += ' ';
            label += *g_failed_label;
        }
        label += entry->item_id;
        if (ImGui::MenuItem(label.c_str(), nullptr, entry->want_open))
            SetOpen(entry->def.id.c_str(), !entry->want_open, false);
        const xr_string& hint = entry->failed && !entry->reason.empty() ? entry->reason : entry->tooltip;
        if (!hint.empty())
            ImGui::SetItemTooltip("%s", hint.c_str());
    }
    while (!path.empty())
    {
        if (opened.back())
            ImGui::EndMenu();
        path.pop_back();
        opened.pop_back();
    }
}

// imgui.ini: the lines of [GWAddonTool] when ImGui read the file before the handler was there
void ReadSavedOpenFromDisk()
{
    const pcstr file = ImGui::GetIO().IniFilename;
    if (!file)
        return;
    size_t size = 0;
    char* data = static_cast<char*>(ImFileLoadToMemory(file, "rb", &size, 1));
    if (!data)
        return;
    const xr_string prefix = xr_string("[") + kSettingsType + "][";
    bool* current = nullptr;
    char* line = data;
    char* const end = data + size;
    while (line < end)
    {
        char* line_end = line;
        while (line_end < end && *line_end != '\n' && *line_end != '\r')
            ++line_end;
        *line_end = 0;
        if (line[0] == '[')
        {
            current = nullptr;
            const size_t length = line_end - line;
            if (length > prefix.size() + 1 && !strncmp(line, prefix.c_str(), prefix.size()) && line_end[-1] == ']')
            {
                const xr_string name(line + prefix.size(), length - prefix.size() - 1);
                current = &SavedOpen()[name];
            }
        }
        else if (current)
        {
            int value = 0;
            if (sscanf(line, "Open=%d", &value) == 1)
                *current = value != 0;
        }
        line = line_end + 1;
    }
    IM_FREE(data);
}

void ApplySavedOpen()
{
    if (!g_entries)
        return;
    for (Entry* entry : *g_entries)
    {
        if (entry->dead || entry->def.no_save_open)
            continue;
        const auto it = SavedOpen().find(entry->key);
        if (it != SavedOpen().end() && it->second)
            entry->want_open = true;
    }
}

void InstallSettingsHandler()
{
    if (g_handler_installed || !ImGui::GetCurrentContext())
        return;
    g_handler_installed = true;
    if (ImGui::FindSettingsHandler(kSettingsType))
        return;
    ImGuiSettingsHandler handler;
    handler.TypeName = kSettingsType;
    handler.TypeHash = ImHashStr(kSettingsType);
    handler.ClearAllFn = [](ImGuiContext*, ImGuiSettingsHandler*) { SavedOpen().clear(); };
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
        bool& open = SavedOpen()[name]; // unordered_map: the reference stays valid when other lines are added
        open = false;
        return &open;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
        int value = 0;
        if (entry && sscanf(line, "Open=%d", &value) == 1)
            *static_cast<bool*>(entry) = value != 0;
    };
    handler.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler*) { ApplySavedOpen(); };
    // Every line ever read or set, the tools that are not registered now too (an addon switched off keeps its flags)
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* buffer) {
        if (!g_saved_open)
            return;
        for (const auto& [key, open] : *g_saved_open)
        {
            if (open)
                buffer->appendf("[%s][%s]\nOpen=1\n\n", self->TypeName, key.c_str());
        }
    };
    ImGui::AddSettingsHandler(&handler);
    if (GImGui->SettingsLoaded)
        ReadSavedOpenFromDisk(); // ImGui read imgui.ini already (the first NewFrame came before the persistent)
}

pcstr StateName(xray::editor::ide::visible_state state)
{
    using state_t = xray::editor::ide::visible_state;
    switch (state)
    {
    case state_t::hidden: return "hidden";
    case state_t::full: return "full";
    case state_t::light: return "light";
    }
    return "hidden";
}
} // namespace

pcstr RegisterResultName(ERegister result)
{
    switch (result)
    {
    case ERegister::Ok: return "ok";
    case ERegister::BadDef: return "bad def";
    case ERegister::BadId: return "bad id";
    case ERegister::BadOwner: return "owner";
    case ERegister::Exists: return "exists";
    case ERegister::Limit: return "limit";
    }
    return "bad def";
}

bool IsValidId(pcstr id) { return id && ValidId(id); }
bool IsValidOwner(pcstr owner) { return owner && ValidOwner(owner); }

ERegister Register(const ToolDef& def, std::unique_ptr<ToolCallbacks> callbacks)
{
    if (!callbacks)
        return ERegister::BadDef;
    if (!ValidId(def.id))
        return ERegister::BadId;
    if (!ValidOwner(def.owner))
        return ERegister::BadOwner;
    if (def.title.size() > kMaxTitleLength || def.menu.size() > kMaxMenuLength || def.tooltip.size() > kMaxTooltipLength ||
        MenuDepth(def.menu) > kMaxMenuDepth || !ValidSize(def.size) || !ValidSize(def.min_size) ||
        !std::isfinite(def.pos[0]) || !std::isfinite(def.pos[1]) || (def.window_flags & ~imgui::kWindowAll))
    {
        return ERegister::BadDef;
    }
    if (Find(def.id.c_str()))
        return ERegister::Exists;
    u32 total = 0, of_owner = 0;
    if (g_entries)
    {
        for (const Entry* entry : *g_entries)
        {
            if (entry->dead)
                continue;
            ++total;
            if (entry->def.owner == def.owner)
                ++of_owner;
        }
    }
    if (total >= kMaxTools || of_owner >= kMaxToolsPerOwner)
        return ERegister::Limit;

    if (!g_entries)
        g_entries = xr_new<xr_vector<Entry*>>();
    if (!g_menu_label)
        MakeLabels();
    Entry* entry = xr_new<Entry>();
    entry->def = def;
    entry->callbacks = std::move(callbacks);
    entry->key = def.owner + "/" + def.id;
    entry->seq = ++g_seq;
    TranslateEntry(*entry);
    if (!def.no_save_open)
    {
        const auto it = SavedOpen().find(entry->key);
        entry->want_open = it != SavedOpen().end() && it->second;
    }
    // Pushed at the end: a loop of the host over the indices sees it closed (open is false) and goes on
    g_entries->push_back(entry);
    return ERegister::Ok;
}

EUnregister Unregister(pcstr id, pcstr owner, bool quiet)
{
    Entry* entry = Find(id);
    if (!entry)
        return EUnregister::NotFound;
    if (!owner || entry->def.owner != owner)
        return EUnregister::Owner;
    if (!quiet)
    {
        entry->want_open = false;
        SaveOpen(*entry, false);
        if (entry->open)
            Close(*entry);
        if (entry->dead)
            return EUnregister::Ok; // on_close removed it already
    }
    MarkDead(*entry, true);
    Purge();
    return EUnregister::Ok;
}

bool SetOpen(pcstr id, bool open, bool show)
{
    Entry* entry = Find(id);
    if (!entry || !Visible(*entry))
        return false;
    if (open)
    {
        entry->want_open = true;
        entry->failed = false; // opening it again clears the mark of the error
        entry->reason.clear();
        SaveOpen(*entry, true);
        if (show)
            RequestEditorState("full");
    }
    else
    {
        entry->want_open = false;
        SaveOpen(*entry, false);
        if (entry->open)
            Close(*entry);
    }
    Purge();
    return true;
}

bool IsOpen(pcstr id)
{
    const Entry* entry = Find(id);
    return entry && entry->want_open;
}

bool SetTitle(pcstr id, pcstr title)
{
    Entry* entry = Find(id);
    if (!entry || !title || xr_strlen(title) > kMaxTitleLength)
        return false;
    entry->def.title = title;
    TranslateEntry(*entry);
    return true;
}

xr_vector<ToolInfo> List()
{
    xr_vector<ToolInfo> result;
    if (!g_entries)
        return result;
    for (const Entry* entry : *g_entries)
    {
        if (entry->dead)
            continue;
        ToolInfo& info = result.emplace_back();
        info.id = entry->def.id;
        info.owner = entry->def.owner;
        info.title = entry->title;
        for (const xr_string& level : entry->menu)
        {
            if (!info.menu.empty())
                info.menu += '/';
            info.menu += level;
        }
        info.kind = entry->callbacks ? entry->callbacks->Kind() : EToolKind::Lua;
        info.open = entry->want_open;
        info.state = !Visible(*entry) ? "hidden" : entry->failed ? "failed" : "ok";
        info.reason = entry->reason;
    }
    return result;
}

pcstr EditorStateName() { return StateName(Device.editor().GetState()); }

bool RequestEditorState(pcstr state)
{
    using state_t = xray::editor::ide::visible_state;
    if (!state)
        return false;
    if (!xr_strcmp(state, "hidden"))
        g_requested_state = static_cast<int>(state_t::hidden);
    else if (!xr_strcmp(state, "full"))
        g_requested_state = static_cast<int>(state_t::full);
    else if (!xr_strcmp(state, "light"))
        g_requested_state = static_cast<int>(state_t::light);
    else
        return false;
    return true;
}

bool Available() { return g_enabled != 0; }

bool DevToolsAllowed()
{
    static const bool allowed = UiDebuggerEnabled || strstr(Core.Params, "-dbg") != nullptr || IsDebugLog();
    return allowed;
}

void RemoveOwner(pcstr owner)
{
    if (!owner)
        return;
    imgui_lua::DropDeferred(owner);
    if (!g_entries)
        return;
    u32 removed = 0;
    for (Entry* entry : *g_entries)
    {
        if (entry->dead || entry->def.owner != owner || !entry->callbacks || entry->callbacks->Kind() != EToolKind::Lua)
            continue;
        MarkDead(*entry, true);
        ++removed;
    }
    Purge();
    if (removed)
        Msg("* [addon:%s] addon_reload: %u ImGui tool(s) removed", owner, removed);
}

void RemoveByKey(EToolKind kind, const void* key, bool alive)
{
    if (!g_entries)
        return;
    for (Entry* entry : *g_entries)
    {
        if (entry->dead || !entry->callbacks || entry->callbacks->Kind() != kind)
            continue;
        if (key && entry->callbacks->Key() != key)
            continue;
        MarkDead(*entry, alive);
    }
    Purge();
}

void OnFrame()
{
    // The deferred functions first: they may register, remove, open (that all lands in this frame of the host)
    imgui_lua::RunDeferred();
    if (g_requested_state >= 0)
    {
        const auto state = static_cast<xray::editor::ide::visible_state>(g_requested_state);
        g_requested_state = -1;
        Device.editor().SetState(state);
    }
    if (g_ini_dirty && ImGui::GetCurrentContext())
    {
        g_ini_dirty = false;
        ImGui::MarkIniSettingsDirty();
    }
    Purge();
}

void LocalizationChanged()
{
    MakeLabels();
    if (!g_entries)
        return;
    for (Entry* entry : *g_entries)
    {
        if (!entry->dead)
            TranslateEntry(*entry);
    }
}

void Shutdown()
{
    if (g_entries)
    {
        for (Entry* entry : *g_entries)
        {
            entry->release_alive = false; // the Lua state and the plugins may be gone
            Free(entry);
        }
        xr_delete(g_entries);
    }
    g_has_dead = false;
    xr_delete(g_menu_label);
    xr_delete(g_failed_label);
    // g_saved_open stays: imgui.ini is written in DestroyContext, after this (see its comment)
}

// ---------------------------------------------------------------------------------------------
// AddonToolsHost
// ---------------------------------------------------------------------------------------------

AddonToolsHost::AddonToolsHost()
{
    ImGui::SetCurrentContext(Device.GetImGuiContext());
    InstallSettingsHandler();
}

// The settings handler stays registered: ImGui writes imgui.ini in DestroyContext, after the persistent is gone;
// its functions and g_saved_open live as long as the process (xrGame is not unloaded before)
AddonToolsHost::~AddonToolsHost() = default;

void AddonToolsHost::on_tool_frame()
{
    if (!g_enabled || !g_entries || g_entries->empty())
        return;
    ZoneScopedN("addons/imgui_tools");
    if (g_ini_dirty)
    {
        g_ini_dirty = false;
        ImGui::MarkIniSettingsDirty();
    }
    OpenAsked();
    const bool light = Device.editor().GetState() == xray::editor::ide::visible_state::light;
    const ImGuiWindowFlags light_flags = light ? get_default_window_flags() : ImGuiWindowFlags_None;
    {
        Busy busy;
        // Over the indices: draw may register (pushed at the end, closed) or remove (marked dead) tools
        for (size_t i = 0; i < g_entries->size(); ++i)
        {
            Entry& entry = *(*g_entries)[i];
            if (!entry.open || !Visible(entry) || (light && entry.def.hide_in_light))
                continue;
            DrawTool(entry, light, light_flags);
        }
    }
    Purge();
}

void AddonToolsHost::on_main_menu_bar()
{
    if (!g_enabled || !g_entries)
        return;
    thread_local xr_vector<Entry*> tools;
    tools.clear();
    for (Entry* entry : *g_entries)
    {
        if (Visible(*entry))
            tools.push_back(entry);
    }
    if (tools.empty())
        return;
    if (!g_menu_label)
        MakeLabels();
    {
        Busy busy; // a click may close a tool: on_close may remove tools, the list must stay valid
        if (ImGui::BeginMenu(g_menu_label->c_str()))
        {
            ShowMenu(tools);
            ImGui::EndMenu();
        }
    }
    tools.clear();
    Purge();
}

bool AddonToolsHost::has_light_windows() const
{
    if (!g_enabled || !g_entries)
        return false;
    for (const Entry* entry : *g_entries)
    {
        if (entry->want_open && Visible(*entry) && !entry->def.hide_in_light)
            return true;
    }
    return false;
}
} // namespace gw::addons::imgui_tools
