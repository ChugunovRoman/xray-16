#pragma once

#include "weapon_inv_icon.h"

class CUICellItem;
class CInventoryItem;

CUICellItem* create_cell_item(CInventoryItem* itm, EWeaponInvIconPreset weapon_icon_preset = eWpnInvIcon_Inventory);
