#include "StdAfx.h"
#include "weapon_inv_icon.h"
#include "Weapon.h"
#include "Level.h"
#include "Actor.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "ai_space.h"
#include "game_type.h"
#include "xrEngine/IRenderable.h"
#include "xrEngine/Render.h"
#include "xrCore/Animation/Bone.hpp"
#include "xrCore/LocatorAPI.h"
#include "ui/UIInventoryUtilities.h"
#include "UIGameCustom.h"
#include "ui/UIActorMenu.h"

#include <algorithm>

#include "xrCommon/xr_map.h"

// Section-keyed icon RT cache (shared by all items with the same m_section_id while InvIconUsesSharedSectionRt).
static xr_map<shared_str, u8> g_section_inv_icon_presets_ready;
static u32 g_inv_icon_rt_epoch{1};

#if defined(_MSC_VER)
#include <intrin.h>
#define WPN_INV_ICON_DBG_BREAK() __debugbreak()
#else
#define WPN_INV_ICON_DBG_BREAK() ((void)0)
#endif

#define WPN_INV_ICON_CACHE_LOG(...) \
    do \
    { \
        if (g_debug_cache_trace) \
            Msg(__VA_ARGS__); \
    } while (0)

namespace
{
bool g_loaded = false;
bool g_global_enabled = false;
u32 g_icon_size = 256;
bool g_debug_trace = false;
bool g_debug_cache_trace = false;
bool g_debug_break_schedule = false;
bool g_debug_break_process = false;
u32 g_icons_per_frame = 2;
// While actor inventory/trade menu is open, cap GPU icon passes per frame (reduces FPS hitches).
u32 g_icons_per_frame_inventory_open = 1;
// Global framing (after correct RT aspect, raw preset cam_dist/fov often leaves the model tiny).
float g_cam_dist_mul{1.f};
float g_fov_deg_mul{1.f};
// Center camera on root bone (model space); if false, keep legacy origin framing.
bool g_icon_pivot_root_bone{true};

struct SPresetAngles
{
    float yaw_deg{};
    float pitch_deg{};
    float roll_deg{};
    float cam_dist{1.2f};
    float fov_deg{35.f};
    // Multiplier on (texel_h / texel_w), same as engine fASPECT; default 1.
    float proj_aspect{1.f};
    // Pan framing in preset-local axes (same basis as yaw/pitch/roll): shift look_at before build_camera.
    // +frame_shift_ax -> weapon moves LEFT in the icon; +frame_shift_ay -> moves DOWN; negative ay -> UP.
    float frame_shift_ax{};
    float frame_shift_ay{};
    // Per-preset texel scale (pixels per inv_grid cell) for RT and UI source rect.
    float rt_grid_w{ICON_GRID_WIDTH};
    float rt_grid_h{ICON_GRID_HEIGHT};
    float ui_grid_w{ICON_GRID_WIDTH};
    float ui_grid_h{ICON_GRID_HEIGHT};
};

SPresetAngles g_presets[eWpnInvIconPreset_COUNT];

xr_vector<CInventoryItem*> g_pending;

static constexpr u8 AllInvIconPresetsMask()
{
    return (u8)((1u << eWpnInvIconPreset_COUNT) - 1);
}

static void MarkSectionInvIconPresetReady(const shared_str& sect, EWeaponInvIconPreset p)
{
    g_section_inv_icon_presets_ready[sect] |= (u8)(1u << (u32)p);
}

static bool SectionInvIconPresetReady(const shared_str& sect, EWeaponInvIconPreset p)
{
    const auto it = g_section_inv_icon_presets_ready.find(sect);
    if (it == g_section_inv_icon_presets_ready.end())
        return false;
    return (it->second & (u8)(1u << (u32)p)) != 0;
}

static bool TryApplySectionInvIconCache(CInventoryItem* item)
{
    if (!item)
        return false;
    if (!item->InvIconUsesSharedSectionRt())
    {
        WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] TRYAPPLY_SKIP id=%u sect=[%s] shared_section_rt=0 (per-instance "
                               "tex; attach/detach or full Invalidate)",
            item->object_id(), item->m_section_id.c_str());
        return false;
    }
    const shared_str& sect = item->m_section_id;
    if (!sect.size())
        return false;
    const auto it = g_section_inv_icon_presets_ready.find(sect);
    if (it == g_section_inv_icon_presets_ready.end())
    {
        WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] TRYAPPLY_MISS id=%u sect=[%s] no_section_entry (never rendered "
                               "for this section yet)",
            item->object_id(), sect.c_str());
        return false;
    }
    if ((it->second & AllInvIconPresetsMask()) != AllInvIconPresetsMask())
    {
        WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] TRYAPPLY_MISS id=%u sect=[%s] partial_mask=0x%02x need=0x%02x",
            item->object_id(), sect.c_str(), it->second, AllInvIconPresetsMask());
        return false;
    }
    for (u32 pi = 0; pi < eWpnInvIconPreset_COUNT; ++pi)
        item->SetDynamicInvIconPresetReady((EWeaponInvIconPreset)pi, true);
    item->SetNeedDynamicInvIconUpgrade(false);
    item->ClearInvIconQueueRetries();
    WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] TRYAPPLY_HIT id=%u sect=[%s] -> reuse shared RT [%s] / [%s] (no queue)",
        item->object_id(), sect.c_str(), weapon_inv_icon::TextureResourceName(sect.c_str(), eWpnInvIcon_Inventory).c_str(),
        weapon_inv_icon::TextureResourceName(sect.c_str(), eWpnInvIcon_Technician).c_str());
    return true;
}

bool IsInPending(CInventoryItem* item)
{
    return std::find(g_pending.begin(), g_pending.end(), item) != g_pending.end();
}

pcstr PresetSuffix(EWeaponInvIconPreset preset)
{
    switch (preset)
    {
    case eWpnInvIcon_Inventory: return "inventory";
    case eWpnInvIcon_Technician: return "technician";
    default: return "inventory";
    }
}

float ReadPresetFloatOverride(pcstr weapon_section, EWeaponInvIconPreset preset, pcstr base_key, float fallback)
{
    if (!weapon_section || !weapon_section[0] || !base_key || !base_key[0])
        return fallback;

    string128 key;
    xr_sprintf(key, sizeof(key), "%s_%s", base_key, PresetSuffix(preset));
    if (pSettings->line_exist(weapon_section, key))
        return pSettings->r_float(weapon_section, key);

    return fallback;
}

static bool TryGetWeaponGridSizeFromSection(pcstr section, u32& out_gw, u32& out_gh)
{
    if (!section || !pSettings->section_exist(section))
        return false;
    if (pSettings->line_exist(section, "inv_grid_alt_width") && pSettings->line_exist(section, "inv_grid_alt_height"))
    {
        out_gw = pSettings->r_u32(section, "inv_grid_alt_width");
        out_gh = pSettings->r_u32(section, "inv_grid_alt_height");
    }
    else if (pSettings->line_exist(section, "inv_grid_width") && pSettings->line_exist(section, "inv_grid_height"))
    {
        out_gw = pSettings->r_u32(section, "inv_grid_width");
        out_gh = pSettings->r_u32(section, "inv_grid_height");
    }
    else
        return false;
    return out_gw && out_gh;
}

static SPresetAngles ResolveWeaponPreset(pcstr weapon_section, EWeaponInvIconPreset preset)
{
    SPresetAngles out = g_presets[preset];
    if (!weapon_section || !weapon_section[0])
        return out;

    out.yaw_deg = ReadPresetFloatOverride(weapon_section, preset, "yaw_deg", out.yaw_deg);
    out.pitch_deg = ReadPresetFloatOverride(weapon_section, preset, "pitch_deg", out.pitch_deg);
    out.roll_deg = ReadPresetFloatOverride(weapon_section, preset, "roll_deg", out.roll_deg);
    out.cam_dist = ReadPresetFloatOverride(weapon_section, preset, "cam_dist", out.cam_dist);
    out.fov_deg = ReadPresetFloatOverride(weapon_section, preset, "fov_deg", out.fov_deg);
    out.frame_shift_ax = ReadPresetFloatOverride(weapon_section, preset, "frame_shift_ax", out.frame_shift_ax);
    out.frame_shift_ay = ReadPresetFloatOverride(weapon_section, preset, "frame_shift_ay", out.frame_shift_ay);
    out.proj_aspect = ReadPresetFloatOverride(weapon_section, preset, "proj_aspect", out.proj_aspect);
    out.rt_grid_w = ReadPresetFloatOverride(weapon_section, preset, "rt_grid_width", out.rt_grid_w);
    out.rt_grid_h = ReadPresetFloatOverride(weapon_section, preset, "rt_grid_height", out.rt_grid_h);
    out.ui_grid_w = ReadPresetFloatOverride(weapon_section, preset, "ui_grid_width", out.ui_grid_w);
    out.ui_grid_h = ReadPresetFloatOverride(weapon_section, preset, "ui_grid_height", out.ui_grid_h);

    clamp(out.proj_aspect, 0.2f, 5.f);
    clamp(out.frame_shift_ax, -2.f, 2.f);
    clamp(out.frame_shift_ay, -2.f, 2.f);
    clamp(out.cam_dist, 0.05f, 50.f);
    clamp(out.fov_deg, 4.f, 89.f);
    clamp(out.rt_grid_w, 1.f, 1024.f);
    clamp(out.rt_grid_h, 1.f, 1024.f);
    clamp(out.ui_grid_w, 1.f, 1024.f);
    clamp(out.ui_grid_h, 1.f, 1024.f);
    return out;
}

static SPresetAngles ResolveWeaponPreset(const CWeapon* w, EWeaponInvIconPreset preset)
{
    return ResolveWeaponPreset(w ? w->cNameSect().c_str() : nullptr, preset);
}

void EnsureLoaded()
{
    if (g_loaded)
        return;
    g_loaded = true;
    if (!pSettings->section_exist("weapon_inv_icon"))
    {
        g_global_enabled = false;
        return;
    }
    g_global_enabled = pSettings->read_if_exists<bool>("weapon_inv_icon", "enabled", false);
    g_icon_size = pSettings->read_if_exists<u32>("weapon_inv_icon", "icon_size", 256);
    clamp(g_icon_size, 64u, 1024u);
    g_cam_dist_mul = pSettings->read_if_exists<float>("weapon_inv_icon", "cam_dist_mul", 1.f);
    g_fov_deg_mul = pSettings->read_if_exists<float>("weapon_inv_icon", "fov_deg_mul", 1.f);
    g_icon_pivot_root_bone = pSettings->read_if_exists<bool>("weapon_inv_icon", "pivot_use_root_bone", true);
    g_icons_per_frame = pSettings->read_if_exists<u32>("weapon_inv_icon", "icons_per_frame", 2);
    g_icons_per_frame_inventory_open =
        pSettings->read_if_exists<u32>("weapon_inv_icon", "icons_per_frame_inventory_open", 1);
    clamp(g_cam_dist_mul, 0.08f, 3.f);
    clamp(g_fov_deg_mul, 0.15f, 3.f);
    clamp(g_icons_per_frame, 1u, 64u);
    clamp(g_icons_per_frame_inventory_open, 1u, 64u);

    pcstr inv_name = pSettings->read_if_exists<pcstr>("weapon_inv_icon", "preset_section_inventory", "weapon_inv_icon_inventory");
    pcstr tech_name = pSettings->read_if_exists<pcstr>("weapon_inv_icon", "preset_section_technician", "weapon_inv_icon_technician");

    auto load_preset = [](pcstr sect, SPresetAngles& o)
    {
        if (!pSettings->section_exist(sect))
            return;
        o.yaw_deg = pSettings->read_if_exists<float>(sect, "yaw_deg", 0.f);
        o.pitch_deg = pSettings->read_if_exists<float>(sect, "pitch_deg", -15.f);
        o.roll_deg = pSettings->read_if_exists<float>(sect, "roll_deg", 0.f);
        o.cam_dist = pSettings->read_if_exists<float>(sect, "cam_dist", 1.2f);
        o.fov_deg = pSettings->read_if_exists<float>(sect, "fov_deg", 35.f);
        o.proj_aspect = pSettings->read_if_exists<float>(sect, "proj_aspect", 1.f);
        clamp(o.proj_aspect, 0.2f, 5.f);
        o.frame_shift_ax = pSettings->read_if_exists<float>(sect, "frame_shift_ax", 0.f);
        o.frame_shift_ay = pSettings->read_if_exists<float>(sect, "frame_shift_ay", 0.f);
        clamp(o.frame_shift_ax, -2.f, 2.f);
        clamp(o.frame_shift_ay, -2.f, 2.f);
        o.rt_grid_w = pSettings->read_if_exists<float>(sect, "rt_grid_width", ICON_GRID_WIDTH);
        o.rt_grid_h = pSettings->read_if_exists<float>(sect, "rt_grid_height", ICON_GRID_HEIGHT);
        o.ui_grid_w = pSettings->read_if_exists<float>(sect, "ui_grid_width", o.rt_grid_w);
        o.ui_grid_h = pSettings->read_if_exists<float>(sect, "ui_grid_height", o.rt_grid_h);
        clamp(o.rt_grid_w, 1.f, 1024.f);
        clamp(o.rt_grid_h, 1.f, 1024.f);
        clamp(o.ui_grid_w, 1.f, 1024.f);
        clamp(o.ui_grid_h, 1.f, 1024.f);
    };

    load_preset(inv_name, g_presets[eWpnInvIcon_Inventory]);
    load_preset(tech_name, g_presets[eWpnInvIcon_Technician]);

    g_debug_trace = pSettings->read_if_exists<bool>("weapon_inv_icon", "debug_trace", false);
    g_debug_cache_trace = pSettings->read_if_exists<bool>("weapon_inv_icon", "debug_inv_icon_cache", false);
    g_debug_break_schedule = pSettings->read_if_exists<bool>("weapon_inv_icon", "debug_break_on_schedule", false);
    g_debug_break_process = pSettings->read_if_exists<bool>("weapon_inv_icon", "debug_break_on_process", false);

    if (g_debug_trace)
    {
        Msg("~ [weapon_inv_icon] LoadSettings: global enabled=%d icon_size=%u cam_dist_mul=%.3f fov_deg_mul=%.3f "
            "icons_per_frame=%u debug_trace=ON debug_cache=%d break_sched=%d break_proc=%d",
            g_global_enabled ? 1 : 0, g_icon_size, g_cam_dist_mul, g_fov_deg_mul, g_icons_per_frame, g_debug_cache_trace ? 1 : 0,
            g_debug_break_schedule ? 1 : 0, g_debug_break_process ? 1 : 0);
        if (!g_global_enabled)
            Msg("! [weapon_inv_icon] global disabled -> ScheduleWeapon/ProcessRenderPass no-op. Set [weapon_inv_icon] enabled=true");
    }

    if (pSettings->read_if_exists<bool>("weapon_inv_icon", "log_config_on_load", true))
    {
        static bool s_weapon_inv_icon_config_notice_done{};
        if (!s_weapon_inv_icon_config_notice_done)
        {
            s_weapon_inv_icon_config_notice_done = true;
            Msg("~ [weapon_inv_icon] config OK: enabled=%d icons_per_frame=%u | verbose: debug_trace=1 or "
                "debug_inv_icon_cache=1 in misc\\weapon_inv_icon.ltx | bake console: inv_icon_bake_dds",
                g_global_enabled ? 1 : 0, g_icons_per_frame);
        }
    }
}

static void GetWeaponIconRtTexelSizeForSection(pcstr section, EWeaponInvIconPreset preset, u32& out_w, u32& out_h)
{
    EnsureLoaded();
    u32 gw, gh;
    if (!TryGetWeaponGridSizeFromSection(section, gw, gh))
    {
        out_w = out_h = g_icon_size;
        clamp(out_w, 64u, 4096u);
        clamp(out_h, 64u, 4096u);
        return;
    }
    const SPresetAngles P = ResolveWeaponPreset(section, preset);
    out_w = u32(iFloor(float(gw) * P.rt_grid_w + 0.5f));
    out_h = u32(iFloor(float(gh) * P.rt_grid_h + 0.5f));
    clamp(out_w, 64u, 4096u);
    clamp(out_h, 64u, 4096u);
}

static void GetWeaponIconUiTexelSizeForSection_Impl(pcstr section, EWeaponInvIconPreset preset, u32& out_w, u32& out_h)
{
    EnsureLoaded();
    u32 gw, gh;
    if (!TryGetWeaponGridSizeFromSection(section, gw, gh))
    {
        out_w = out_h = g_icon_size;
        clamp(out_w, 1u, 4096u);
        clamp(out_h, 1u, 4096u);
        return;
    }
    const SPresetAngles P = ResolveWeaponPreset(section, preset);
    out_w = u32(iFloor(float(gw) * P.ui_grid_w + 0.5f));
    out_h = u32(iFloor(float(gh) * P.ui_grid_h + 0.5f));
    clamp(out_w, 1u, 4096u);
    clamp(out_h, 1u, 4096u);
}
} // namespace

namespace weapon_inv_icon
{
void LoadSettings() { EnsureLoaded(); }

void ReloadSettings()
{
    g_loaded = false;
    EnsureLoaded();
    g_section_inv_icon_presets_ready.clear();
    ++g_inv_icon_rt_epoch;
}

static void GetWeaponGridSize(const CWeapon* w, u32& out_gw, u32& out_gh)
{
    EnsureLoaded();
    VERIFY(w);
    VERIFY(TryGetWeaponGridSizeFromSection(w->cNameSect().c_str(), out_gw, out_gh));
}

void GetWeaponIconRtTexelSize(const CWeapon* w, EWeaponInvIconPreset preset, u32& out_w, u32& out_h)
{
    VERIFY(w);
    GetWeaponIconRtTexelSizeForSection(w->cNameSect().c_str(), preset, out_w, out_h);
}

void GetWeaponIconUiTexelSize(const CWeapon* w, EWeaponInvIconPreset preset, u32& out_w, u32& out_h)
{
    u32 gw, gh;
    GetWeaponGridSize(w, gw, gh);
    const SPresetAngles P = ResolveWeaponPreset(w->cNameSect().c_str(), preset);
    out_w = u32(iFloor(float(gw) * P.ui_grid_w + 0.5f));
    out_h = u32(iFloor(float(gh) * P.ui_grid_h + 0.5f));
    clamp(out_w, 1u, 4096u);
    clamp(out_h, 1u, 4096u);
}

void GetWeaponIconUiTexelSizeForSection(pcstr section, EWeaponInvIconPreset preset, u32& out_w, u32& out_h)
{
    GetWeaponIconUiTexelSizeForSection_Impl(section, preset, out_w, out_h);
}

bool SectionSharedInvIconPresetReady(pcstr section, EWeaponInvIconPreset preset)
{
    EnsureLoaded();
    if (!section || !section[0])
        return false;
    return SectionInvIconPresetReady(shared_str(section), preset);
}

bool IsEnabledForSection(pcstr weapon_section)
{
    EnsureLoaded();
    if (!g_global_enabled || !weapon_section)
        return false;
    return pSettings->read_if_exists<bool>(weapon_section, "use_dynamic_inv_icon", false);
}

bool IsEnabledForItem(const CInventoryItem* item)
{
    return item ? IsEnabledForSection(item->m_section_id.c_str()) : false;
}

bool WeaponUsesDynamicIcon(pcstr weapon_section) { return IsEnabledForSection(weapon_section); }

void ScheduleWeapon(CWeapon* w)
{
    ScheduleItem(w);
}

void ScheduleItem(CInventoryItem* item)
{
    if (!item || GEnv.isDedicatedServer)
        return;
    EnsureLoaded();

    pcstr sect = item->m_section_id.c_str();

    if (!IsEnabledForSection(sect))
        return;

    if (TryApplySectionInvIconCache(item))
        return;

    bool all_presets_ready = true;
    for (u32 pi = 0; pi < eWpnInvIconPreset_COUNT; ++pi)
    {
        if (!item->DynamicInvIconPresetReady((EWeaponInvIconPreset)pi))
        {
            all_presets_ready = false;
            break;
        }
    }
    if (all_presets_ready && !item->NeedDynamicInvIconUpgrade())
        return;

    if (!IsInPending(item))
    {
        item->EnsureInvIconQueueRetries();
        g_pending.push_back(item);
        WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] QUEUE_ENQUEUE id=%u sect=[%s] shared_rt=%d (will render or "
                               "section-map hit in pass)",
            item->object_id(), sect ? sect : "?", item->InvIconUsesSharedSectionRt() ? 1 : 0);
    }

    if (g_debug_break_schedule)
        WPN_INV_ICON_DBG_BREAK();
}

void OnWeaponDestroyed(CWeapon* w)
{
    OnItemDestroyed(w);
}

void OnItemDestroyed(CInventoryItem* item)
{
    if (item && GEnv.Render && !GEnv.isDedicatedServer && !item->InvIconUsesSharedSectionRt())
    {
        for (u32 pi = 0; pi < eWpnInvIconPreset_COUNT; ++pi)
        {
            const shared_str tex = TextureResourceName(item, (EWeaponInvIconPreset)pi);
            GEnv.Render->WeaponIcon_ReleaseUserIconRt(tex.c_str());
        }
    }
    auto it = std::find(g_pending.begin(), g_pending.end(), item);
    if (it != g_pending.end())
        g_pending.erase(it);
}

shared_str TextureResourceName(const CWeapon* w, EWeaponInvIconPreset preset)
{
    return TextureResourceName(static_cast<const CInventoryItem*>(w), preset);
}

shared_str TextureResourceName(pcstr item_section, EWeaponInvIconPreset preset)
{
    string512 buf;
    const pcstr sect = (item_section && item_section[0]) ? item_section : "unknown";
    xr_sprintf(buf, sizeof(buf), "$user$itm_inv_sect_%s_%u", sect, (u32)preset);
    return shared_str(buf);
}

shared_str TextureResourceName(const CInventoryItem* item, EWeaponInvIconPreset preset)
{
    if (!item)
        return TextureResourceName(static_cast<pcstr>(nullptr), preset);
    if (item->InvIconUsesSharedSectionRt())
        return TextureResourceName(item->m_section_id.c_str(), preset);
    string512 buf;
    const pcstr sect = item->m_section_id.size() ? item->m_section_id.c_str() : "unknown";
    const u32 id = (u32)item->object_id();
    xr_sprintf(buf, sizeof(buf), "$user$itm_inv_%s_%u_%u", sect, id, (u32)preset);
    return shared_str(buf);
}

// Pivot in object-local space (matches world during icon render with identity XFORM).
static Fvector WeaponIconModelPivot(CWeapon* w)
{
    Fvector pivot{};
    if (!w || !g_icon_pivot_root_bone)
        return pivot;

    IRenderVisual* vis = w->Visual();
    if (!vis)
        return pivot;

    if (IKinematics* K = smart_cast<IKinematics*>(vis))
    {
        const Fmatrix saved_xform = w->XFORM();
        w->XFORM().identity();
        K->CalculateBones_Invalidate();
        K->CalculateBones(TRUE);
        const u16 root = K->LL_GetBoneRoot();
        pivot = K->LL_GetTransform(root).c;
        w->XFORM() = saved_xform;
        return pivot;
    }

    Fbox bb = vis->getVisData().box;
    if (bb.is_valid())
    {
        Fvector c, ext;
        bb.get_CD(c, ext);
        pivot = c;
    }
    return pivot;
}

// Bake / visual-only path: same pivot rules as WeaponIconModelPivot with identity world transform.
static Fvector VisualIconModelPivot(IRenderVisual* vis)
{
    Fvector pivot{};
    if (!vis || !g_icon_pivot_root_bone)
        return pivot;

    if (IKinematics* K = smart_cast<IKinematics*>(vis))
    {
        K->CalculateBones_Invalidate();
        K->CalculateBones(TRUE);
        const u16 root = K->LL_GetBoneRoot();
        pivot = K->LL_GetTransform(root).c;
        return pivot;
    }

    Fbox bb = vis->getVisData().box;
    if (bb.is_valid())
    {
        Fvector c, ext;
        bb.get_CD(c, ext);
        pivot = c;
    }
    return pivot;
}

static void BuildIconViewProj(pcstr section, const Fvector& pivot, EWeaponInvIconPreset preset, Fmatrix& out_view,
    Fmatrix& out_proj)
{
    EnsureLoaded();
    const SPresetAngles P = ResolveWeaponPreset(section, preset);
    const float cam_d = P.cam_dist * g_cam_dist_mul;

    Fvector eye, look_at;
    eye.set(0.f, 0.15f * cam_d, cam_d);
    look_at.set(0.f, 0.05f, 0.f);

    Fmatrix rot;
    rot.identity();
    rot.setHPB(deg2rad(P.yaw_deg), deg2rad(P.pitch_deg), deg2rad(P.roll_deg));
    rot.transform_tiny(eye);
    rot.transform_tiny(look_at);
    eye.add(pivot);
    look_at.add(pivot);

    if (!fis_zero(P.frame_shift_ax) || !fis_zero(P.frame_shift_ay))
    {
        Fvector ax, ay;
        rot.transform_dir(ax, Fvector().set(1.f, 0.f, 0.f));
        rot.transform_dir(ay, Fvector().set(0.f, 1.f, 0.f));
        Fvector t;
        t.mul(ax, P.frame_shift_ax);
        look_at.add(t);
        t.mul(ay, P.frame_shift_ay);
        look_at.add(t);
    }

    out_view.build_camera(eye, look_at, Fvector().set(0.f, 1.f, 0.f));

    u32 pw, ph;
    GetWeaponIconRtTexelSizeForSection(section, preset, pw, ph);
    // Same convention as Device.fASPECT / CameraManager: height/width (see Device.fHeight_2 / fWidth_2).
    const float base_aspect = pw ? float(ph) / float(pw) : 1.f;
    const float aspect = base_aspect * P.proj_aspect;

    float fov = P.fov_deg * g_fov_deg_mul;
    clamp(fov, 4.f, 89.f);
    out_proj.build_projection(deg2rad(fov), aspect, 0.01f, 50.f);
}

void BuildMatricesForWeapon(CWeapon* w, EWeaponInvIconPreset preset, Fmatrix& out_view, Fmatrix& out_proj)
{
    if (!w)
    {
        out_view.identity();
        out_proj.identity();
        return;
    }
    BuildIconViewProj(w->cNameSect().c_str(), WeaponIconModelPivot(w), preset, out_view, out_proj);
}

void OnWeaponIconSnapshot(IRenderable* subject, bool begin)
{
    CWeapon* w = smart_cast<CWeapon*>(subject);
    if (!w)
        return;

    thread_local Fmatrix s_saved_xform{};
    if (begin)
    {
        s_saved_xform = w->XFORM();
        w->XFORM().identity();
        w->SetWeaponIconSnapshot(true);
    }
    else
    {
        w->SetWeaponIconSnapshot(false);
        w->XFORM() = s_saved_xform;
    }
}

static void ProcessDynamicInvIconForSingleItem(CInventoryItem* item)
{
    if (!item || item->IsInvalid())
    {
        if (g_debug_trace)
            Msg("~ [weapon_inv_icon]   skip destroyed/null item ptr");
        return;
    }

    CWeapon* w = item->cast_weapon();
    if (!w)
    {
        if (item->ConsumeInvIconQueueRetryForRequeue() && !IsInPending(item))
            g_pending.push_back(item);
        return;
    }

    for (u32 pi = 0; pi < eWpnInvIconPreset_COUNT; ++pi)
    {
        const auto preset = (EWeaponInvIconPreset)pi;
        if (item->DynamicInvIconPresetReady(preset))
            continue;
        if (item->InvIconUsesSharedSectionRt() && SectionInvIconPresetReady(item->m_section_id, preset))
        {
            item->SetDynamicInvIconPresetReady(preset, true);
            WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] SECTION_MAP_HIT id=%u sect=[%s] preset=%u tex=[%s] "
                                   "(no GPU render, marked item ready)",
                item->object_id(), item->m_section_id.c_str(), pi, TextureResourceName(item, preset).c_str());
            continue;
        }

        Fmatrix view, proj;
        BuildMatricesForWeapon(w, preset, view, proj);
        u32 tw, th;
        GetWeaponIconRtTexelSize(w, preset, tw, th);
        shared_str tex = TextureResourceName(item, preset);
        WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] RENDER_NEW id=%u sect=[%s] preset=%u shared_rt=%d tex=[%s]",
            item->object_id(), item->m_section_id.c_str(), pi, item->InvIconUsesSharedSectionRt() ? 1 : 0, tex.c_str());
        const bool ok = GEnv.Render->WeaponIcon_RenderToTexture(tex.c_str(), tw, th, view, proj, w);
        if (ok)
        {
            item->SetDynamicInvIconPresetReady(preset, true);
            if (item->InvIconUsesSharedSectionRt())
            {
                MarkSectionInvIconPresetReady(item->m_section_id, preset);
                WPN_INV_ICON_CACHE_LOG("~ [weapon_inv_icon][cache] SECTION_MAP_STORE sect=[%s] preset=%u (shared RT filled)",
                    item->m_section_id.c_str(), pi);
            }
            if (g_debug_trace)
                Msg("~ [weapon_inv_icon] generated icon (runtime GPU): id=%u sect=[%s] preset=[%s] %ux%u tex=[%s]",
                    item->object_id(), item->m_section_id.c_str(), PresetSuffix(preset), tw, th, tex.c_str());
        }
        else if (g_debug_trace)
            Msg("~ [weapon_inv_icon]   WeaponIcon_RenderToTexture id=%u preset=%u tex=[%s] ok=0", w->ID(), pi,
                tex.c_str());
    }

    bool fully_ready = true;
    for (u32 pi = 0; pi < eWpnInvIconPreset_COUNT; ++pi)
    {
        if (!item->DynamicInvIconPresetReady((EWeaponInvIconPreset)pi))
        {
            fully_ready = false;
            break;
        }
    }
    if (fully_ready)
    {
        item->SetNeedDynamicInvIconUpgrade(false);
        item->ClearInvIconQueueRetries();
    }
    else
    {
        item->SetNeedDynamicInvIconUpgrade(true);
        if (item->ConsumeInvIconQueueRetryForRequeue() && !IsInPending(item))
            g_pending.push_back(item);
    }
}

void ProcessRenderPass()
{
    if (GEnv.isDedicatedServer || !GEnv.Render)
        return;
    EnsureLoaded();
    if (!g_global_enabled || g_pending.empty())
        return;

    u32 frame_budget = g_icons_per_frame;
    if (CurrentGameUI())
    {
        CUIActorMenu& am = CurrentGameUI()->GetActorMenu();
        if (am.IsShown())
            frame_budget = _min<u32>(frame_budget, g_icons_per_frame_inventory_open);
    }

    if (g_debug_trace)
        Msg("~ [weapon_inv_icon] ProcessRenderPass: pending=%d frame_budget=%u (inventory_menu=%d)", (int)g_pending.size(),
            frame_budget, (CurrentGameUI() && CurrentGameUI()->GetActorMenu().IsShown()) ? 1 : 0);

    if (g_debug_break_process)
        WPN_INV_ICON_DBG_BREAK();

    const u32 budget = _min<u32>(frame_budget, (u32)g_pending.size());
    for (u32 i = 0; i < budget; ++i)
    {
        CInventoryItem* item = g_pending.front();
        g_pending.erase(g_pending.begin());
        ProcessDynamicInvIconForSingleItem(item);
    }
}

void RenderDynamicInvIconsImmediateForItem(CInventoryItem* item)
{
    if (!item || GEnv.isDedicatedServer || !GEnv.Render)
        return;
    EnsureLoaded();
    if (!g_global_enabled || !IsEnabledForItem(item))
        return;

    g_pending.erase(std::remove(g_pending.begin(), g_pending.end(), item), g_pending.end());
    ProcessDynamicInvIconForSingleItem(item);
}

u32 InvIconRtEpoch() { return g_inv_icon_rt_epoch; }

void OnWeaponIconUserRtsReleased()
{
    g_section_inv_icon_presets_ready.clear();
    ++g_inv_icon_rt_epoch;
}

void HotReloadInvIconSettings()
{
    if (GEnv.Render && !GEnv.isDedicatedServer)
        GEnv.Render->WeaponIcon_ReleaseAllUserIconRts();

    g_pending.clear();

    ReloadSettings();

    u32 requeued = 0;
    if (!GEnv.isDedicatedServer && g_pGameLevel)
    {
        CObjectList& objs = Level().Objects;
        const u32 n = objs.o_count();
        for (u32 i = 0; i < n; ++i)
        {
            IGameObject* o = objs.o_get_by_iterator(i);
            if (!o)
                continue;
            CInventoryItem* itm = smart_cast<CInventoryItem*>(o);
            if (!itm || !IsEnabledForItem(itm))
                continue;
            itm->QueueDynamicInvIconRefresh();
            ++requeued;
        }
    }

    Msg("~ [weapon_inv_icon] HotReload: GPU icon RTs dropped, ini state reloaded, epoch bumped, queue cleared; "
        "re-queued %u dynamic-icon item(s). Open/refresh inventory to see updates.",
        requeued);
}

void DbgTraceWeaponCellShaderDecision(pcstr weapon_section)
{
    EnsureLoaded();
    if (!g_debug_trace)
        return;
    const bool sec = weapon_section && pSettings->read_if_exists<bool>(weapon_section, "use_dynamic_inv_icon", false);
    Msg("~ [weapon_inv_icon] UI CUIWeaponCellItem sect=[%s] global=%d use_dynamic=%d -> bind $user RT shader=%d",
        weapon_section ? weapon_section : "?", g_global_enabled ? 1 : 0, sec ? 1 : 0,
        (g_global_enabled && sec) ? 1 : 0);
}

static bool SectionIsBakeableWeapon(pcstr sec)
{
    if (!sec || !pSettings->section_exist(sec) || !pSettings->line_exist(sec, "class"))
        return false;
    shared_str cl = pSettings->r_string(sec, "class");
    return strstr(cl.c_str(), "WP_") != nullptr;
}

// Minimal IRenderable for icon bake: loads OGF from [section] visual, no game entity / spawn.
class CWpnIconBakeVisual final : public RenderableBase
{
public:
    explicit CWpnIconBakeVisual(pcstr visual_path)
    {
        renderable.xform.identity();
        renderable.visual =
            (visual_path && visual_path[0] && GEnv.Render) ? GEnv.Render->model_Create(visual_path) : nullptr;
        if (IKinematics* K = renderable.visual ? smart_cast<IKinematics*>(renderable.visual) : nullptr)
        {
            // Bake-only: hide addon attachment bones so icons match “clean” world mesh; in-game weapons still
            // drive visibility from CWeapon attach state.
            const u16 id_s = K->LL_BoneID("wpn_silencer");
            if (id_s != BI_NONE)
                K->LL_SetBoneVisible(id_s, FALSE, TRUE);
            const u16 id_l = K->LL_BoneID("wpn_launcher");
            if (id_l != BI_NONE)
                K->LL_SetBoneVisible(id_l, FALSE, TRUE);
            K->CalculateBones_Invalidate();
            K->CalculateBones(TRUE);
        }
    }

    void renderable_Render(u32 context_id, IRenderable* root) override
    {
        if (!renderable.visual || !GEnv.Render)
            return;
        GEnv.Render->add_Visual(context_id, root, renderable.visual, renderable.xform);
        renderable.visual->getVisData().hom_frame = Device.dwFrame;
    }
};

void BakeCurrentActorWeaponInvIconToDds()
{
    if (GEnv.isDedicatedServer || !GEnv.Render)
    {
        Msg("! [weapon_inv_icon] bake_current: need client with renderer");
        return;
    }
    if (!g_pGameLevel)
    {
        Msg("! [weapon_inv_icon] bake_current: not in game level");
        return;
    }
    if (!IsGameTypeSingle())
    {
        Msg("! [weapon_inv_icon] bake_current: single player only");
        return;
    }

    CActor* actor = Actor();
    if (!actor)
    {
        Msg("! [weapon_inv_icon] bake_current: actor not found");
        return;
    }

    PIItem active_item = actor->inventory().ActiveItem();
    CWeapon* wpn = active_item ? active_item->cast_weapon() : nullptr;
    if (!wpn)
    {
        Msg("! [weapon_inv_icon] bake_current: active item is not a weapon");
        return;
    }

    EnsureLoaded();
    if (!g_global_enabled)
        Msg("~ [weapon_inv_icon] bake_current: warning [weapon_inv_icon] enabled=false (render may still work)");

    const shared_str section = wpn->cNameSect();
    if (!section.size())
    {
        Msg("! [weapon_inv_icon] bake_current: weapon section is empty");
        return;
    }
    Msg("~ [weapon_inv_icon] bake_current: started section=[%s] (active weapon in hands)", section.c_str());

    bool ok_both = true;
    u32 done = 0;
    for (u32 pi = 0; pi < eWpnInvIconPreset_COUNT; ++pi)
    {
        const auto preset = (EWeaponInvIconPreset)pi;
        Fmatrix view, proj;
        BuildMatricesForWeapon(wpn, preset, view, proj);
        u32 tw{}, th{};
        GetWeaponIconRtTexelSize(wpn, preset, tw, th);
        const shared_str tex = TextureResourceName(section.c_str(), preset);

        const bool rendered = GEnv.Render->WeaponIcon_RenderToTexture(tex.c_str(), tw, th, view, proj, wpn);
        if (!rendered)
        {
            Msg("! [weapon_inv_icon] bake_current: render failed [%s] preset=%u", section.c_str(), pi);
            ok_both = false;
            continue;
        }

        string_path rel{};
        if (preset == eWpnInvIcon_Inventory)
            xr_sprintf(rel, "ui\\weapon_inv_bake\\inventory\\%s.dds", section.c_str());
        else
            xr_sprintf(rel, "ui\\weapon_inv_bake\\technician\\%s.dds", section.c_str());

        const bool saved = GEnv.Render->WeaponIcon_SavePersistedUserRtToDdsDxt5(tex.c_str(), "$game_textures$", rel);
        if (!saved)
        {
            Msg("! [weapon_inv_icon] bake_current: save DDS failed [%s] preset=%u rel [%s]", section.c_str(), pi, rel);
            ok_both = false;
        }
        else
        {
            ++done;
            Msg("~ [weapon_inv_icon] bake_current: icon generated (DDS) sect=[%s] preset=[%s] %ux%u rel=[$game_textures$\\%s]",
                section.c_str(), PresetSuffix(preset), tw, th, rel);
        }
        GEnv.Render->WeaponIcon_ReleaseUserIconRt(tex.c_str());
    }

    Msg("~ [weapon_inv_icon] bake_current: section=[%s] done=%u/%u result=%s",
        section.c_str(), done, (u32)eWpnInvIconPreset_COUNT, ok_both ? "OK" : "FAILED");
}

void BakeDynamicInvIconsToDds(pcstr single_section_or_null)
{
    if (GEnv.isDedicatedServer || !GEnv.Render)
    {
        Msg("! [weapon_inv_icon] bake: need client with renderer");
        return;
    }
    if (!g_pGameLevel)
    {
        Msg("! [weapon_inv_icon] bake: not in game level");
        return;
    }
    if (!IsGameTypeSingle())
    {
        Msg("! [weapon_inv_icon] bake: single player only");
        return;
    }
    EnsureLoaded();
    if (!g_global_enabled)
        Msg("~ [weapon_inv_icon] bake: warning [weapon_inv_icon] enabled=false (render may still work)");

    Msg("~ [weapon_inv_icon] bake: started (visual via model_Create; DDS under ui\\weapon_inv_bake\\inventory|technician\\)");

    string_path tex_root{};
    if (FS.update_path(tex_root, "$game_textures$", "", false))
        Msg("~ [weapon_inv_icon] bake: DDS output root (resolved) = [%s]", tex_root);
    else
        Msg("! [weapon_inv_icon] bake: cannot resolve $game_textures$ — writes will fail");

    u32 done = 0, failed = 0;
    u32 skip_no_dyn = 0, skip_no_visual = 0, skip_no_grid = 0, skip_class = 0, skip_scope_variant = 0;

    auto bake_one = [&](pcstr sec_name)
    {
        if (!pSettings->read_if_exists<bool>(sec_name, "use_dynamic_inv_icon", false))
        {
            ++skip_no_dyn;
            return;
        }
        if (pSettings->line_exist(sec_name, "parent_section"))
        {
            const shared_str par = pSettings->r_string(sec_name, "parent_section");
            if (par.size() && xr_strcmp(par.c_str(), sec_name) != 0)
            {
                ++skip_scope_variant;
                return;
            }
        }
        if (!pSettings->line_exist(sec_name, "visual"))
        {
            Msg("! [weapon_inv_icon] bake: skip [%s] (no visual)", sec_name);
            ++skip_no_visual;
            return;
        }
        u32 gw_test{}, gh_test{};
        if (!TryGetWeaponGridSizeFromSection(sec_name, gw_test, gh_test))
        {
            Msg("! [weapon_inv_icon] bake: skip [%s] (no inv_grid)", sec_name);
            ++skip_no_grid;
            return;
        }
        if (!SectionIsBakeableWeapon(sec_name))
        {
            Msg("! [weapon_inv_icon] bake: skip [%s] (class not WP_*)", sec_name);
            ++skip_class;
            return;
        }

        const shared_str visual = pSettings->r_string(sec_name, "visual");
        CWpnIconBakeVisual bake_vis(visual.c_str());
        if (!bake_vis.GetRenderData().visual)
        {
            Msg("! [weapon_inv_icon] bake: model_Create failed [%s] visual=[%s]", sec_name, visual.c_str());
            ++failed;
            return;
        }

        bool ok_both = true;
        for (u32 pi = 0; pi < eWpnInvIconPreset_COUNT; ++pi)
        {
            const auto preset = (EWeaponInvIconPreset)pi;
            Fmatrix view, proj;
            BuildIconViewProj(sec_name, VisualIconModelPivot(bake_vis.GetRenderData().visual), preset, view, proj);
            u32 tw, th;
            GetWeaponIconRtTexelSizeForSection(sec_name, preset, tw, th);
            const shared_str tex = TextureResourceName(sec_name, preset);
            const bool rendered =
                GEnv.Render->WeaponIcon_RenderToTexture(tex.c_str(), tw, th, view, proj, &bake_vis);
            if (!rendered)
            {
                Msg("! [weapon_inv_icon] bake: render failed [%s] preset=%u", sec_name, pi);
                ok_both = false;
                continue;
            }

            string_path rel{};
            if (preset == eWpnInvIcon_Inventory)
                xr_sprintf(rel, "ui\\weapon_inv_bake\\inventory\\%s.dds", sec_name);
            else
                xr_sprintf(rel, "ui\\weapon_inv_bake\\technician\\%s.dds", sec_name);

            const bool saved =
                GEnv.Render->WeaponIcon_SavePersistedUserRtToDdsDxt5(tex.c_str(), "$game_textures$", rel);
            if (!saved)
            {
                string_path abs_fail{};
                xr_strcpy(abs_fail, rel);
                if (FS.update_path(abs_fail, "$game_textures$", abs_fail, false))
                    Msg("! [weapon_inv_icon] bake: save DDS failed [%s] preset=%u path [%s]", sec_name, pi, abs_fail);
                else
                    Msg("! [weapon_inv_icon] bake: save DDS failed [%s] preset=%u rel [%s] (DX11 DXT5, valid RT?)", sec_name,
                        pi, rel);
                ok_both = false;
            }
            else
            {
                Msg("~ [weapon_inv_icon] bake: icon generated (DDS) sect=[%s] preset=[%s] %ux%u rel=[$game_textures$\\%s]",
                    sec_name, PresetSuffix(preset), tw, th, rel);
            }
            GEnv.Render->WeaponIcon_ReleaseUserIconRt(tex.c_str());
        }

        if (ok_both)
        {
            ++done;
            Msg("~ [weapon_inv_icon] bake: OK [%s]", sec_name);
        }
        else
            ++failed;
    };

    if (single_section_or_null && single_section_or_null[0])
    {
        if (!pSettings->section_exist(single_section_or_null))
        {
            Msg("! [weapon_inv_icon] bake: unknown section [%s]", single_section_or_null);
            return;
        }
        bake_one(single_section_or_null);
    }
    else
    {
        for (CInifile::Sect* sect : pSettings->sections())
        {
            if (!sect || !sect->Name.size())
                continue;
            bake_one(sect->Name.c_str());
        }
    }

    const u32 skipped =
        skip_no_dyn + skip_no_visual + skip_no_grid + skip_class + skip_scope_variant;
    Msg("~ [weapon_inv_icon] bake: done=%u failed=%u | skipped: no_use_dynamic_inv_icon=%u no_visual=%u "
        "no_inv_grid=%u bad_class=%u scope_variant_parent=%u | total_skipped=%u",
        done, failed, skip_no_dyn, skip_no_visual, skip_no_grid, skip_class, skip_scope_variant, skipped);
    if (!done && skipped && !failed)
        Msg("~ [weapon_inv_icon] bake: hint — bake needs use_dynamic_inv_icon=true + visual + inv_grid + class WP_* "
            "+ parent_section absent or == section; DDS: ui\\weapon_inv_bake\\inventory|technician\\<section>.dds; "
            "try: inv_icon_bake_dds wpn_fal_aus");
}
} // namespace weapon_inv_icon
