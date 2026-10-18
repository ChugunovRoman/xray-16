////////////////////////////////////////////////////////////////////////////
//	Module 		: script_ini_file.h
//	Created 	: 21.05.2004
//  Modified 	: 21.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Script ini file class
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "script_token_list.h"

#include <luabind/back_reference_fwd.hpp>

struct lua_State;

class CScriptIniFile : public CInifile
{
protected:
    using inherited = CInifile;

public:
    CScriptIniFile(IReader* F, LPCSTR path = nullptr);
    CScriptIniFile(LPCSTR szFileName, BOOL ReadOnly = TRUE, BOOL bLoadAtStart = TRUE, BOOL SaveAtEnd = TRUE);
    CScriptIniFile(LPCSTR initial, LPCSTR szFileName, BOOL ReadOnly = TRUE, BOOL bLoadAtStart = TRUE, BOOL SaveAtEnd = TRUE);
    // Lua ini_file(path) / ini_file(initial, path): L is the calling Lua thread. As the two above with the default
    // flags; P20 (plans/optimization_mt/15-ini-cache.md): the parse is tagged with the Lua call site.
    CScriptIniFile(lua_State* L, LPCSTR szFileName);
    CScriptIniFile(lua_State* L, LPCSTR initial, LPCSTR szFileName);

    // P20: the CInifile created next on this thread is tagged as made by Lua (from_text: create_ini_file) with the Lua
    // call site, for the [ini-load] table of perf_metrics. Returns pass_through (for a mem-initializer list);
    // EndLuaSource() puts back the previous tag. Nothing is read from L while perf_metrics is off.
    static LPCSTR BeginLuaSource(lua_State* L, bool from_text, LPCSTR pass_through);
    static void EndLuaSource();

    int r_clsid(LPCSTR S, LPCSTR L);
    int r_token(LPCSTR S, LPCSTR L, const CScriptTokenList& token_list);
    LPCSTR update(LPCSTR initial, LPCSTR file_name);
    u32 line_count(LPCSTR S);
    LPCSTR r_string(LPCSTR S, LPCSTR L);
    u32 r_u32(LPCSTR S, LPCSTR L);
    int r_s32(LPCSTR S, LPCSTR L);
    float r_float(LPCSTR S, LPCSTR L);
    Fvector r_fvector3(LPCSTR S, LPCSTR L);

    //AVO: additional methods to allow writing to ini files
    void w_bool(pcstr S, pcstr L, bool V, pcstr comment /* = nullptr */);
    void w_color(pcstr S, pcstr L, u32 V, pcstr comment /* = nullptr */);
    void w_fcolor(pcstr S, pcstr L, const Fcolor& V, pcstr comment /* = nullptr */);
    void w_float(pcstr S, pcstr L, float V, pcstr comment /* = nullptr */);
    void w_fvector2(pcstr S, pcstr L, const Fvector2& V, pcstr comment /* = nullptr */);
    void w_fvector3(pcstr S, pcstr L, const Fvector3& V, pcstr comment /* = nullptr */);
    void w_fvector4(pcstr S, pcstr L, const Fvector4& V, pcstr comment /* = nullptr */);
    void w_s16(pcstr S, pcstr L, s16 V, pcstr comment /* = nullptr */);
    void w_s32(pcstr S, pcstr L, s32 V, pcstr comment /* = nullptr */);
    void w_s64(pcstr S, pcstr L, s64 V, pcstr comment /* = nullptr */);
    void w_s8(pcstr S, pcstr L, s8 V, pcstr comment /* = nullptr */);
    void w_string(pcstr S, pcstr L, pcstr V, pcstr comment /* = nullptr */);
    void w_u16(pcstr S, pcstr L, u16 V, pcstr comment /* = nullptr */);
    void w_u32(pcstr S, pcstr L, u32 V, pcstr comment /* = nullptr */);
    void w_u64(pcstr S, pcstr L, u64 V, pcstr comment /* = nullptr */);
    void w_u8(pcstr S, pcstr L, u8 V, pcstr comment /* = nullptr */);
    bool save_as(pcstr new_fname /* = nullptr */);
    void remove_line(pcstr S, pcstr L);

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

// The Lua wrapper cache (xrScriptEngine/script_wrapper_cache.hpp): system_ini() - the system ini itself (pSettings),
// any pass of it to Lua by pointer - gives one wrapper per Lua state. Any other ini (ini_file(), create_ini_file(), spawn_ini(), game_ini()...) goes the old way: a new userdata.
// Defined in script_ini_file_script.cpp; declared with the class, so no translation unit instantiates the generic
// template (all of them are in xrGame: xrServerEntities is built into it).
namespace luabind
{
template <>
bool get_back_reference<CScriptIniFile*>(lua_State* L, CScriptIniFile* const& x);
} // namespace luabind
