#pragma once

#include "xrCore/xrCore.h"

namespace Checks
{
    static const char* ScriptsLogicFiles = "scripts_logic_files"; // Проверка на отсутствие файлов логики
    static const char* SmartCovers = "smart_covers"; // Проверка на использование несуществующих смарт-каверов в файлах логик
    static const char* PatrolPaths = "patrol_paths"; // Проверка на использование несуществующих патрул пазов в файлах логик
    static const char* InvalidVertexes = "invalid_vertexes"; // Проверка всех вертексов у каждого патрул паза game_vertex_id() == (u16)-1 || level_vertex_id() == (u32)-1
};

namespace Dicts
{
    static const char* SmartTerrains = "smart_terrains"; // Список всех смарт-террейнов в игре
    static const char* SmartCovers = "smart_covers"; // Список всех смарт-каверов в игре
    static const char* PatrolPaths = "patrol_paths"; // Список всех патрул пазов в игре
    static const char* InventoryBox = "inventory_boxes"; // Список всех ящиков с инвентарем в игре
    static const char* SpaceRestrictor = "space_restrictors"; // Список всех рестриктов в игре
    static const char* AnomalZone = "anomal_zones"; // Список всех аномальных зон в игре
    static const char* Campfires = "campfires"; // Список всех костров в игре
    static const char* AllSpawns = "all_spawns"; // Список всех объектов из all.spawn
};

class Checker
{
public:
    Checker();
    ~Checker();

    xr_map<shared_str, bool> logic_files;

    void AddToCheckLog(shared_str type, shared_str msg);
    void AddToDictLog(shared_str type, shared_str msg);

    void CloseAllDescriptors();

private:
    xr_map<shared_str, IWriter*> discriptors;
    xr_map<shared_str, xr_map<shared_str, bool>> checks;
    xr_map<shared_str, xr_map<shared_str, bool>> dicts;

};
