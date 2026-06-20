//////////////////////////////////////////////////////////////////////////
// character_community.cpp:		структура представления группировки
//
//////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "character_community.h"

//////////////////////////////////////////////////////////////////////////
COMMUNITY_DATA::COMMUNITY_DATA(CHARACTER_COMMUNITY_INDEX idx, CHARACTER_COMMUNITY_ID idn, LPCSTR team_str)
{
    index = idx;
    id = idn;
    team = (u8)atoi(team_str);
}

//////////////////////////////////////////////////////////////////////////
CHARACTER_COMMUNITY::GOODWILL_TABLE CHARACTER_COMMUNITY::m_relation_table;
// таблица отношений с значениями по умолчанию которые не меняются во время игры
CHARACTER_COMMUNITY::GOODWILL_TABLE CHARACTER_COMMUNITY::m_default_relation_table;
CHARACTER_COMMUNITY::SYMPATHY_TABLE CHARACTER_COMMUNITY::m_sympathy_table;

//////////////////////////////////////////////////////////////////////////
CHARACTER_COMMUNITY::CHARACTER_COMMUNITY() { m_current_index = NO_COMMUNITY_INDEX; }
CHARACTER_COMMUNITY::~CHARACTER_COMMUNITY() {}
void CHARACTER_COMMUNITY::set(CHARACTER_COMMUNITY_ID id, bool no_assert /*= false*/) { m_current_index = IdToIndex(id, NO_COMMUNITY_INDEX, no_assert); }
CHARACTER_COMMUNITY_ID CHARACTER_COMMUNITY::id() const { return IndexToId(m_current_index); }
u8 CHARACTER_COMMUNITY::team() const { return (*m_pItemDataVector)[m_current_index].team; }
void CHARACTER_COMMUNITY::InitIdToIndex()
{
    section_name = "game_relations";
    line_name = "communities";

    // relation tables are loaded from relations/*.ltx via BuildRelationTableFromFiles()
    // called after InitInternal() populates m_pItemDataVector
    m_sympathy_table.set_table_params("communities_sympathy", 1);
}

void CHARACTER_COMMUNITY::BuildRelationTableFromFiles()
{
    size_t n = GetMaxIndex() + 1;

    GOODWILL_TABLE::ITEM_TABLE tbl;
    tbl.resize(n);
    for (size_t i = 0; i < n; i++)
        tbl[i].resize(n, 0);

    // build name->index map
    xr_map<shared_str, int> name_to_idx;
    for (const auto& item : *m_pItemDataVector)
        name_to_idx[item.id] = (int)item.index;

    // strip actor_ prefix if base exists
    auto resolve_base = [&](const char* name) -> const char* {
        if (strncmp(name, "actor_", 6) == 0) {
            const char* base = name + 6;
            if (name_to_idx.count(shared_str(base)))
                return base;
        }
        return name;
    };

    // load per-faction sparse files
    // from_rels[base_name][to_base_name] = goodwill
    xr_map<shared_str, xr_map<shared_str, CHARACTER_GOODWILL>> from_rels;

    for (const auto& item : *m_pItemDataVector) {
        const char* faction = item.id.c_str();
        if (strncmp(faction, "actor_", 6) == 0)
            continue;

        string_path rel_path, full_path;
        xr_sprintf(rel_path, "creatures\\relations\\%s.ltx", faction);

        if (!FS.exist(full_path, "$game_config$", rel_path))
            continue;

        CInifile* ini = xr_new<CInifile>(full_path, TRUE, TRUE, FALSE);
        if (ini->section_exist(faction)) {
            CInifile::Sect& sect = ini->r_section(faction);
            auto& faction_rels = from_rels[shared_str(faction)];
            for (const auto& line : sect.Data)
                faction_rels[line.first] = (CHARACTER_GOODWILL)atoi(line.second.c_str());
        }
        xr_delete(ini);
    }

    // fill n×n matrix with actor_* fallback
    for (const auto& from_item : *m_pItemDataVector) {
        int from_idx = (int)from_item.index;
        const char* from_base = resolve_base(from_item.id.c_str());

        auto from_it = from_rels.find(shared_str(from_base));
        if (from_it == from_rels.end())
            continue;

        for (const auto& to_item : *m_pItemDataVector) {
            int to_idx = (int)to_item.index;
            const char* to_base = resolve_base(to_item.id.c_str());

            auto to_it = from_it->second.find(shared_str(to_base));
            if (to_it != from_it->second.end())
                tbl[from_idx][to_idx] = to_it->second;
        }
    }

    m_relation_table.load_from_table(tbl);
    m_default_relation_table.load_from_table(tbl);

    Msg("* Faction relations loaded from creatures/relations/ (%zu factions)", n);
}

CHARACTER_GOODWILL CHARACTER_COMMUNITY::relation(CHARACTER_COMMUNITY_INDEX to) { return relation(m_current_index, to); }
CHARACTER_GOODWILL CHARACTER_COMMUNITY::relation(CHARACTER_COMMUNITY_INDEX from, CHARACTER_COMMUNITY_INDEX to)
{
    VERIFY(from >= 0 && from < (int)m_relation_table.table().size());
    VERIFY(to >= 0 && to < (int)m_relation_table.table().size());

    return m_relation_table.table()[from][to];
}

CHARACTER_GOODWILL CHARACTER_COMMUNITY::default_relation(CHARACTER_COMMUNITY_INDEX to) { return relation(m_current_index, to); }
CHARACTER_GOODWILL CHARACTER_COMMUNITY::default_relation(CHARACTER_COMMUNITY_INDEX from, CHARACTER_COMMUNITY_INDEX to)
{
    VERIFY(from >= 0 && from < (int)m_default_relation_table.table().size());
    VERIFY(to >= 0 && to < (int)m_default_relation_table.table().size());

    return m_default_relation_table.table()[from][to];
}
void CHARACTER_COMMUNITY::reset_relations_to_default()
{
    for (int i = 0; i < m_default_relation_table.table().size(); i++) {
        for (int j = 0; j < m_default_relation_table.table()[i].size(); j++) {
            m_relation_table.table()[i][j] = m_default_relation_table.table()[i][j];
        }
    }
}

void CHARACTER_COMMUNITY::set_relation(
    CHARACTER_COMMUNITY_INDEX from, CHARACTER_COMMUNITY_INDEX to, CHARACTER_GOODWILL goodwill)
{
    VERIFY(from >= 0 && from < (int)m_relation_table.table().size());
    VERIFY(to >= 0 && to < (int)m_relation_table.table().size());
    VERIFY(goodwill != NO_GOODWILL);

    m_relation_table.table()[from][to] = goodwill;
}

float CHARACTER_COMMUNITY::sympathy(CHARACTER_COMMUNITY_INDEX comm)
{
    VERIFY(comm >= 0 && comm < (int)m_sympathy_table.table().size());
    return m_sympathy_table.table()[comm][0];
}

void CHARACTER_COMMUNITY::DeleteIdToIndexData()
{
    m_relation_table.clear();
    m_sympathy_table.clear();
    inherited::DeleteIdToIndexData();
}
