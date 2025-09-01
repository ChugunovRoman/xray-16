////////////////////////////////////////////////////////////////////////////
//	Module 		: object_factory.h
//	Created 	: 27.05.2004
//  Modified 	: 27.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Object factory
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrEngine/editor_base.h"

#include "object_item_abstract.h"
#include "object_factory_spawner.h"

class CObjectFactory
#ifndef MASTER_GOLD
    : public xray::editor::ide_tool
#endif
{
public:
    using ClientObjectBaseClass = ObjectFactory::ClientObjectBaseClass;
    using ServerObjectBaseClass = ObjectFactory::ServerObjectBaseClass;

protected:
    struct CObjectItemPredicate
    {
        IC bool operator()(const CObjectItemAbstract* item1, const CObjectItemAbstract* item2) const;
        IC bool operator()(const CObjectItemAbstract* item, const CLASS_ID& clsid) const;
    };

    struct CObjectItemPredicateCLSID
    {
        CLASS_ID m_clsid;

        IC CObjectItemPredicateCLSID(const CLASS_ID& clsid);
        IC bool operator()(const CObjectItemAbstract* item) const;
    };

    struct CObjectItemPredicateScript
    {
        shared_str m_script_clsid_name;

        IC CObjectItemPredicateScript(const shared_str& script_clsid_name);
        IC bool operator()(const CObjectItemAbstract* item) const;
    };

public:
    typedef xr_vector<CObjectItemAbstract*> OBJECT_ITEM_STORAGE;
    typedef OBJECT_ITEM_STORAGE::iterator iterator;
    typedef OBJECT_ITEM_STORAGE::const_iterator const_iterator;

protected:
    mutable OBJECT_ITEM_STORAGE m_clsids;
    mutable bool m_actual;

protected:
    void register_classes();
    IC void add(CObjectItemAbstract* item);
    IC const OBJECT_ITEM_STORAGE& clsids() const;
    IC void actualize() const;

    template <typename _unknown_type>
    IC void add(const CLASS_ID& clsid, LPCSTR script_clsid);

    template <typename _client_type, typename _server_type>
    IC void add(const CLASS_ID& clsid, LPCSTR script_clsid);

    IC const CObjectItemAbstract& item(const CLASS_ID& clsid) const;
    IC const CObjectItemAbstract* item(const CLASS_ID& clsid, bool no_assert) const;

public:
    CObjectFactory();
    virtual ~CObjectFactory();
    void init();

    inline ClientObjectBaseClass* client_object(const CLASS_ID& clsid) const;
    inline ServerObjectBaseClass* server_object(const CLASS_ID& clsid, LPCSTR section) const;
    inline ServerObjectBaseClass* server_object(const CLASS_ID& clsid, LPCSTR section, bool no_assert) const;

    IC int script_clsid(const CLASS_ID& clsid) const;
    void register_script() const;
    void register_script_class(LPCSTR client_class, LPCSTR server_class, LPCSTR clsid, LPCSTR script_clsid);
    void register_script_class(LPCSTR unknown_class, LPCSTR clsid, LPCSTR script_clsid);
    void register_script_classes();

#ifndef MASTER_GOLD
public:
    void on_tool_frame() override;
    void init_spawn_data();

private:
    pcstr tool_name() const override { return "Spawner"; }

    static constexpr auto CATEGORIES_COUNT = static_cast<size_t>(xray::SpawnCategory::CategoriesCount);

    std::array<CInifile::Root, CATEGORIES_COUNT> m_spawner_sections;
#endif
};

extern CObjectFactory* g_object_factory;

IC const CObjectFactory& object_factory();

#include "object_factory_inline.h"
