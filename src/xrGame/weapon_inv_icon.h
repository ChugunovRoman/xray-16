#pragma once

#include "xrCore/xrstring.h"
#include "xrCore/_matrix.h"

class CWeapon;
class CInventoryItem;
class IRenderable;

enum EWeaponInvIconPreset : u8
{
    eWpnInvIcon_Inventory = 0,
    eWpnInvIcon_Technician = 1,
    eWpnInvIconPreset_COUNT
};

namespace weapon_inv_icon
{
void LoadSettings();
void ReloadSettings();
// После reload_system_ini или вручную: сброс всех $user$ RT иконок, перечитать [weapon_inv_icon], переочередить предметы.
void HotReloadInvIconSettings();
// RT size in texels for a preset: inv_grid(_alt) * preset rt_grid_*.
void GetWeaponIconRtTexelSize(const CWeapon* w, EWeaponInvIconPreset preset, u32& out_w, u32& out_h);
// UI source rect size in texels for a preset: inv_grid(_alt) * preset ui_grid_*.
void GetWeaponIconUiTexelSize(const CWeapon* w, EWeaponInvIconPreset preset, u32& out_w, u32& out_h);
// То же по имени секции (режим CUIInventoryCellItem(shared_str) без CInventoryItem).
void GetWeaponIconUiTexelSizeForSection(pcstr weapon_section, EWeaponInvIconPreset preset, u32& out_w, u32& out_h);
// Секция есть в g_section_inv_icon_presets_ready для данного пресета (GPU shared RT уже рисовали).
bool SectionSharedInvIconPresetReady(pcstr weapon_section, EWeaponInvIconPreset preset);
bool IsEnabledForSection(pcstr weapon_section);
bool IsEnabledForItem(const CInventoryItem* item);

// Очередь GPU-иконки: только после смены аддонов (InvalidateDynamicInventoryIcons), GE_ADDON_CHANGE, HotReload.
// Спавн, reload() и открытие инвентаря не вызывают — в UI остаются inv_icon / inv_upgrade_icon до регенерации.
void ScheduleWeapon(CWeapon* w);
// Adds `preset` to the item's requested set (CInventoryItem::RequestInvIconPreset) and queues the
// item when something in that set is missing. Only requested presets are ever rendered, so the
// technician preset costs nothing until CUIInventoryUpgradeWnd asks for it.
void ScheduleItem(CInventoryItem* item, EWeaponInvIconPreset preset = eWpnInvIcon_Inventory);
// Regenerate GPU dynamic icon for this item immediately (same pass as ProcessRenderPass, requested presets only,
// ignores frame budget).
void RenderDynamicInvIconsImmediateForItem(CInventoryItem* item);
void OnWeaponDestroyed(CWeapon* w);
void OnItemDestroyed(CInventoryItem* item);

void ProcessRenderPass();
void OnWeaponIconSnapshot(IRenderable* subject, bool begin);

shared_str TextureResourceName(const CWeapon* w, EWeaponInvIconPreset preset);
shared_str TextureResourceName(const CInventoryItem* item, EWeaponInvIconPreset preset);
// Shared RT name for a section (no object id); see CInventoryItem::InvIconUsesSharedSectionRt.
shared_str TextureResourceName(pcstr item_section, EWeaponInvIconPreset preset);
void BuildMatricesForWeapon(CWeapon* w, EWeaponInvIconPreset preset, Fmatrix& out_view, Fmatrix& out_proj);

u32 InvIconRtEpoch();
void OnWeaponIconUserRtsReleased();
// One icon RT was dropped by the renderer's budget: clear the ready flag of every item that used
// that texture, so the UI falls back to the static icon and re-requests it the next time it draws.
void OnInvIconRtEvicted(pcstr texture_name);
// Console inv_icon_stats: how many icon targets exist and how much video memory they hold.
void LogInvIconStats(bool detailed);

bool WeaponUsesDynamicIcon(pcstr weapon_section);

// log_config_on_load (default true): одна строка при первой загрузке [weapon_inv_icon].
// debug_trace / debug_inv_icon_cache: подробный лог; опционально __debugbreak (MSVC).
void DbgTraceWeaponCellShaderDecision(pcstr weapon_section);

// Console: bake from [section] visual via model_Create (no spawn). Skips scope variants (parent_section != section).
// DDS: $game_textures$/ui/weapon_inv_bake/inventory/<section>.dds and .../technician/<section>.dds. Single player.
void BakeDynamicInvIconsToDds(pcstr single_section_or_null);
// Console: bake currently active actor weapon (world model with current addons) into same DDS paths.
void BakeCurrentActorWeaponInvIconToDds();
} // namespace weapon_inv_icon
