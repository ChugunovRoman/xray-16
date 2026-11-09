#pragma once
#include "Common/object_interfaces.h"
#include "map_location_defs.h"

class CMapLocationWrapper;
class CInventoryOwner;
class CMapLocation;
class CRelationMapLocation;

class CMapManager : public CUIResetNotifier
{
    CMapLocationWrapper* m_locations_wrapper;
    vLocations* m_locations;
    xr_vector<CMapLocation*> m_deffered_destroy_queue;
    // Visible relation locations of the running Update by object id: their spots are hidden by the other locations
    // of the same object, matched in one pass over all locations (ResolveRelationSpotChecks)
    xr_vector<std::pair<u16, CRelationMapLocation*>> m_relation_spot_checks;

    void ResolveRelationSpotChecks();

public:
    CMapManager();
    ~CMapManager();
    void Update();
    /*ICF */ vLocations& Locations(); //{return *m_locations;}
    CMapLocation* AddMapLocation(const shared_str& spot_type, u16 id);
    CMapLocation* SetMapLocation(const shared_str& spot_type, u16 id);
    CMapLocation* AddRelationLocation(CInventoryOwner* pInvOwner);
    void RemoveMapLocation(const shared_str& spot_type, u16 id);
    bool HasMapLocation(const shared_str& spot_type, u16 id);
    void RemoveMapLocationByObjectID(u16 id); // call on destroy object
    // CALifeStorageManager::load (09-content-addons-vfs.md section 8, C8): the locations of the saved objects that were
    // not loaded (`ids`, any order) go right after the registries are read, as RemoveMapLocationByObjectID removes them.
    // Without it they would stay till the first Update (no owner: not actual), and a new object given such an id meanwhile
    // would find a location of the old one. Returns how many were removed
    u32 RemoveMapLocationsOfObjects(const xr_vector<u16>& ids);
    void RemoveMapLocation(CMapLocation* ml);
    CMapLocation* GetMapLocation(const shared_str& spot_type, u16 id);
    void GetMapLocations(const shared_str& spot_type, u16 id, xr_vector<CMapLocation*>& res);
    void DisableAllPointers();
    bool GetMapLocationsForObject(u16 id, xr_vector<CMapLocation*>& res);
    void OnObjectDestroyNotify(u16 id);
    /** Called by CRelationMapLocation::Update of a visible relation location (only inside Update). */
    void QueueRelationSpotCheck(CRelationMapLocation* location);
    void ResetStorage() { m_locations = nullptr; }
#ifdef DEBUG
    void Dump();
#endif
    void Destroy(CMapLocation*);

    void OnUIReset() override;

    inline static CUIXml m_uiSpotXml;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};
