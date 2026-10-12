////////////////////////////////////////////////////////////////////////////
//	Module 		: script_ini_file.cpp
//	Created 	: 21.05.2004
//  Modified 	: 21.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Script ini file class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "script_ini_file.h"
#include "xrScriptEngine/script_engine.hpp"
#include "ai_space.h"
#include "object_factory.h"
#include "xrCore/xrDebug.h"
#include "xrCore/Profiling/IniLoadStats.hpp"

// P20 (plans/optimization_mt/15-ini-cache.md): the Lua call site of ini_file / create_ini_file for the [ini-load] table
// of perf_metrics (xrCore/Profiling/IniLoadStats.hpp).
namespace
{
#if XR_PERF_METRICS
thread_local string512 tl_lua_ini_caller{};
thread_local xray::ini_stats::Source tl_lua_ini_prev{};

// "xr_logic.script" of "...gamedata/scripts/xr_logic.script" (either separator)
pcstr LuaFileName(pcstr source)
{
    pcstr name = source;
    for (pcstr c = source; *c; ++c)
    {
        if (*c == '\\' || *c == '/')
            name = c + 1;
    }
    return name;
}

// Up to three Lua frames from the innermost, "file:line < file:line < file:line" (C frames - luabind - are skipped):
// the line that called ini_file and who called that function. ini_file_ex and utils.ini_cached are frames of their own.
void DescribeLuaCaller(lua_State* L, string512& out)
{
    out[0] = 0;
    if (!L)
        return;
    lua_Debug ar{};
    u32 frames = 0;
    for (int level = 0; level < 16 && frames < 3 && lua_getstack(L, level, &ar); ++level)
    {
        if (!lua_getinfo(L, "Sl", &ar) || ar.currentline < 0 || !ar.what || 0 == xr_strcmp(ar.what, "C"))
            continue;
        string128 frame;
        xr_sprintf(frame, "%s%s:%d", frames ? " < " : "", LuaFileName(ar.short_src), ar.currentline);
        xr_strcat(out, frame);
        ++frames;
    }
}
#endif
} // namespace

LPCSTR CScriptIniFile::BeginLuaSource(lua_State* L, bool from_text, LPCSTR pass_through)
{
#if XR_PERF_METRICS
    tl_lua_ini_prev = xray::ini_stats::TlSource();
    tl_lua_ini_caller[0] = 0;
    if (xray::ini_stats::Active())
        DescribeLuaCaller(L, tl_lua_ini_caller);
    using xray::ini_stats::Layer;
    xray::ini_stats::TlSource() = xray::ini_stats::Source{from_text ? Layer::LuaString : Layer::LuaFile, tl_lua_ini_caller};
#else
    (void)L;
    (void)from_text;
#endif
    return pass_through;
}

void CScriptIniFile::EndLuaSource()
{
#if XR_PERF_METRICS
    xray::ini_stats::TlSource() = tl_lua_ini_prev;
#endif
}

CScriptIniFile::CScriptIniFile(IReader* F, LPCSTR path) : inherited(F, path) {}
CScriptIniFile::CScriptIniFile(LPCSTR szFileName, BOOL ReadOnly, BOOL bLoadAtStart, BOOL SaveAtEnd)
    : inherited(update("$game_config$", szFileName), ReadOnly, bLoadAtStart, SaveAtEnd) {}
CScriptIniFile::CScriptIniFile(LPCSTR initial, LPCSTR szFileName, BOOL ReadOnly, BOOL bLoadAtStart, BOOL SaveAtEnd)
    : inherited(update(initial, szFileName), ReadOnly, bLoadAtStart, SaveAtEnd) {}
CScriptIniFile::CScriptIniFile(lua_State* L, LPCSTR szFileName)
    : inherited(BeginLuaSource(L, false, update("$game_config$", szFileName)), TRUE, TRUE, TRUE)
{
    EndLuaSource();
}
CScriptIniFile::CScriptIniFile(lua_State* L, LPCSTR initial, LPCSTR szFileName)
    : inherited(BeginLuaSource(L, false, update(initial, szFileName)), TRUE, TRUE, TRUE)
{
    EndLuaSource();
}

LPCSTR CScriptIniFile::update(LPCSTR initial, LPCSTR file_name)
{
    string_path S1;
    FS.update_path(S1, initial, file_name);
    return shared_str(S1).c_str(); // XXX: dangling pointer
}

int CScriptIniFile::r_clsid(LPCSTR S, LPCSTR L)
{
    return object_factory().script_clsid(inherited::r_clsid(S, L));
}

int CScriptIniFile::r_token(LPCSTR S, LPCSTR L, const CScriptTokenList& token_list)
{
    return inherited::r_token(S, L, &token_list.tokens().front());
}

u32 CScriptIniFile::line_count(LPCSTR S)
{
    R_ASSERT3_CURE(inherited::section_exist(S), "Cannot find section", S,
    {
        return 0;
    });
    return inherited::line_count(S);
}

LPCSTR CScriptIniFile::r_string(LPCSTR S, LPCSTR L)
{
    THROW3(inherited::section_exist(S), "Cannot find section", S);
    THROW3(inherited::line_exist(S, L), "Cannot find line", L);
    return inherited::r_string(S, L);
}

u32 CScriptIniFile::r_u32(LPCSTR S, LPCSTR L)
{
    THROW3(inherited::section_exist(S), "Cannot find section", S);
    THROW3(inherited::line_exist(S, L), "Cannot find line", L);
    return inherited::r_u32(S, L);
}

int CScriptIniFile::r_s32(LPCSTR S, LPCSTR L)
{
    THROW3(inherited::section_exist(S), "Cannot find section", S);
    THROW3(inherited::line_exist(S, L), "Cannot find line", L);
    return inherited::r_s32(S, L);
}

float CScriptIniFile::r_float(LPCSTR S, LPCSTR L)
{
    THROW3(inherited::section_exist(S), "Cannot find section", S);
    THROW3(inherited::line_exist(S, L), "Cannot find line", L);
    return inherited::r_float(S, L);
}

Fvector CScriptIniFile::r_fvector3(LPCSTR S, LPCSTR L)
{
    THROW3(inherited::section_exist(S), "Cannot find section", S);
    THROW3(inherited::line_exist(S, L), "Cannot find line", L);
    return inherited::r_fvector3(S, L);
}

//AVO: additional methods to allow writing to ini files
void CScriptIniFile::w_bool(pcstr S, pcstr L, bool V, pcstr comment)
{
    inherited::w_bool(S, L, V, comment);
}

void CScriptIniFile::w_color(pcstr S, pcstr L, u32 V, pcstr comment)
{
    inherited::w_color(S, L, V, comment);
}

void CScriptIniFile::w_fcolor(pcstr S, pcstr L, const Fcolor& V, pcstr comment)
{
    inherited::w_fcolor(S, L, V, comment);
}

void CScriptIniFile::w_float(pcstr S, pcstr L, float V, pcstr comment)
{
    inherited::w_float(S, L, V, comment);
}

void CScriptIniFile::w_fvector2(pcstr S, pcstr L, const Fvector2& V, pcstr comment)
{
    inherited::w_fvector2(S, L, V, comment);
}

void CScriptIniFile::w_fvector3(pcstr S, pcstr L, const Fvector3& V, pcstr comment)
{
    inherited::w_fvector3(S, L, V, comment);
}

void CScriptIniFile::w_fvector4(pcstr S, pcstr L, const Fvector4& V, pcstr comment)
{
    inherited::w_fvector4(S, L, V, comment);
}

void CScriptIniFile::w_s16(pcstr S, pcstr L, s16 V, pcstr comment)
{
    inherited::w_s16(S, L, V, comment);
}

void CScriptIniFile::w_s32(pcstr S, pcstr L, s32 V, pcstr comment)
{
    inherited::w_s32(S, L, V, comment);
}

void CScriptIniFile::w_s64(pcstr S, pcstr L, s64 V, pcstr comment)
{
    inherited::w_s64(S, L, V, comment);
}

void CScriptIniFile::w_s8(pcstr S, pcstr L, s8 V, pcstr comment)
{
    inherited::w_s8(S, L, V, comment);
}

void CScriptIniFile::w_string(pcstr S, pcstr L, pcstr V, pcstr comment)
{
    inherited::w_string(S, L, V, comment);
}

void CScriptIniFile::w_u16(pcstr S, pcstr L, u16 V, pcstr comment)
{
    inherited::w_u16(S, L, V, comment);
}

void CScriptIniFile::w_u32(pcstr S, pcstr L, u32 V, pcstr comment)
{
    inherited::w_u32(S, L, V, comment);
}

void CScriptIniFile::w_u64(pcstr S, pcstr L, u64 V, pcstr comment)
{
    inherited::w_u64(S, L, V, comment);
}

void CScriptIniFile::w_u8(pcstr S, pcstr L, u8 V, pcstr comment)
{
    inherited::w_u8(S, L, V, comment);
}

bool CScriptIniFile::save_as(pcstr new_fname)
{
    THROW2(new_fname, "File name is null");
    return(inherited::save_as(new_fname));
}

void CScriptIniFile::remove_line(pcstr S, pcstr L)
{
    inherited::remove_line(S, L);
}
