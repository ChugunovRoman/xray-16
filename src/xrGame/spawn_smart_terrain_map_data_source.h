#pragma once

#include "ui/UIMapDataSource.h"

class CSpawnSmartTerrainMapDataSource final : public IMapDataSource
{
public:
    explicit CSpawnSmartTerrainMapDataSource(pcstr spawn_name = "all");

    static void StartSharedLoading(pcstr spawn_name = "all");
    // plans/optimization_mt/50z3-startup-slow-disk-io.md: the start-up. On a slow disk the loading (the whole all.spawn,
    // ~100 MB) waits for the main menu to settle (UpdateSharedLoading) or for a map that wants it (RequestData,
    // get_spawn_smart_level_name); elsewhere it starts at once, as StartSharedLoading
    static void StartSharedLoadingWhenIdle(pcstr spawn_name = "all");
    static void UpdateSharedLoading(); // main, every frame: the deferred loading starts once the menu has settled
    static void PublishSharedDataIfReady();
    static void ShutdownSharedLoading();
    static pcstr GetSharedSmartLevelName(pcstr smart_name, pcstr spawn_name = "all");

    void SetSpawnName(pcstr spawn_name);
    pcstr GetSpawnName() const { return m_spawn_name.c_str(); }

    void Reload() override;
    void EnumeratePoints(xr_vector<SMapPointDesc>& out) const override;
    u32 GetDataRevision() const override;
    bool GetFocusLevel(shared_str& outLevel) const override;
    bool UpdatePointVisual(u32 logical_id, pcstr owner_faction, pcstr icon_texture) override;
    void RequestData() override; // 50z3: a shown map - a deferred loading starts now
    bool SetSmartType(u32 logical_id, pcstr type);
    bool SetPointHintText(u32 logical_id, pcstr hint_text);

private:
    shared_str m_spawn_name;
};
