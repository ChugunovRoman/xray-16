////////////////////////////////////////////////////////////////////////////
//	Module 		: game_level_cross_table_inline.h
//	Created 	: 20.02.2003
//  Modified 	: 13.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Cross table between game and level graphs inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CGameLevelCrossTable::CGameLevelCrossTable(LPCSTR fName)
{
    m_tpCrossTableVFS = FS.r_open(fName);
    R_ASSERT2(m_tpCrossTableVFS, "Can't open cross table!");

    IReader* chunk = m_tpCrossTableVFS->open_chunk(CROSS_TABLE_CHUNK_VERSION);
    R_ASSERT2(chunk, "Cross table is corrupted!");
    chunk->r(&m_tCrossTableHeader, sizeof(m_tCrossTableHeader));
    chunk->close();

    ASSERT_XRAI_VERSION_MATCH(m_tCrossTableHeader.version(), "Cross table");

    m_chunk = m_tpCrossTableVFS->open_chunk(CROSS_TABLE_CHUNK_DATA);
    R_ASSERT2(m_chunk, "Cross table is corrupted!");
    m_tpaCrossTable = (CCell*)m_chunk->pointer();
    m_cells_size = m_chunk->length();
}

IC CGameLevelCrossTable::CGameLevelCrossTable(const void* buffer, const u32& buffer_size)
{
    memcpy(&m_tCrossTableHeader, buffer, sizeof(m_tCrossTableHeader));
    buffer = (const u8*)buffer + sizeof(m_tCrossTableHeader);

    ASSERT_XRAI_VERSION_MATCH(m_tCrossTableHeader.version(), "Cross table");

    m_tpaCrossTable = (CCell*)buffer;
    m_chunk = nullptr;
    m_tpCrossTableVFS = nullptr;
    // 35: buffer_size is the size of the record of the level in the game graph (CGameGraph::set_current_level)
    m_cells_size = buffer_size > sizeof(m_tCrossTableHeader) ? buffer_size - sizeof(m_tCrossTableHeader) : 0;
}

// 35 (plans/optimization_mt/33-level-path.md, journal): the cells of the mapped file are read
// on demand, and the page of a cell nobody read yet is a read from the disk on the first lookup (update_ai_locations of
// an NPC that walked into a new area). After an idle second the drive answers it in ~9 ms (its power state), so the
// copy is made once at the level load: the same bytes, then read from the process memory.
IC bool CGameLevelCrossTable::copy_to_memory()
{
    if (m_cells_copy)
        return true;
    const size_t count = header().level_vertex_count();
    const size_t size = count * sizeof(CCell);
    if (!count || size > m_cells_size)
        return false;
    m_cells_copy = xr_alloc<CCell>(count);
    memcpy(m_cells_copy, m_tpaCrossTable, size);
    m_tpaCrossTable = m_cells_copy;
    return true;
}

IC bool CGameLevelCrossTable::in_memory() const { return m_cells_copy != nullptr; }

IC CGameLevelCrossTable::~CGameLevelCrossTable()
{
    xr_free(m_cells_copy);
    if (m_chunk)
        m_chunk->close();
    FS.r_close(m_tpCrossTableVFS);
}

IC const CGameLevelCrossTable::CCell& CGameLevelCrossTable::vertex(u32 level_vertex_id) const
{
    VERIFY(level_vertex_id < header().level_vertex_count());
    return (m_tpaCrossTable[level_vertex_id]);
}

IC u32 CGameLevelCrossTable::CHeader::version() const { return (dwVersion); }
IC u32 CGameLevelCrossTable::CHeader::level_vertex_count() const { return (dwNodeCount); }
IC u32 CGameLevelCrossTable::CHeader::game_vertex_count() const { return (dwGraphPointCount); }
IC const xrGUID& CGameLevelCrossTable::CHeader::level_guid() const { return (m_level_guid); }
IC const xrGUID& CGameLevelCrossTable::CHeader::game_guid() const { return (m_game_guid); }
IC GameGraph::_GRAPH_ID CGameLevelCrossTable::CCell::game_vertex_id() const { return (tGraphIndex); }
IC float CGameLevelCrossTable::CCell::distance() const { return (fDistance); }
IC const CGameLevelCrossTable::CHeader& CGameLevelCrossTable::header() const { return (m_tCrossTableHeader); }
