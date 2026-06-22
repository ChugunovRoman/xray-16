#pragma once

#include "ui/UIMapDataSource.h"

class CSpawnSmartTerrainMapDataSource final : public IMapDataSource
{
public:
    explicit CSpawnSmartTerrainMapDataSource(pcstr spawn_name = "all");

    static void StartSharedLoading(pcstr spawn_name = "all");
    static void PublishSharedDataIfReady();
    static void ShutdownSharedLoading();

    void SetSpawnName(pcstr spawn_name);
    pcstr GetSpawnName() const { return m_spawn_name.c_str(); }

    void Reload() override;
    void EnumeratePoints(xr_vector<SMapPointDesc>& out) const override;
    u32 GetDataRevision() const override;
    bool GetFocusLevel(shared_str& outLevel) const override;
    bool UpdatePointVisual(u32 logical_id, pcstr owner_faction, pcstr icon_texture) override;
    bool SetSmartType(u32 logical_id, pcstr type);
    bool SetPointHintText(u32 logical_id, pcstr hint_text);

private:
    shared_str m_spawn_name;
};
