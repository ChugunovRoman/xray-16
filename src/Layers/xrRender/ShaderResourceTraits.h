#pragma once

#include "ResourceManager.h"

template <typename T>
struct ShaderTypeTraits;

template <>
struct ShaderTypeTraits<SPS>
{
    typedef CResourceManager::map_PS MapType;
    typedef ID3DPixelShader DXIface;

    static inline const char* GetShaderExt() { return ".ps"; }
    static inline const char* GetCompilationTarget()
    {
        return "ps_2_0";
    }

    static void GetCompilationTarget(const char*& target, const char*& entry, const char* data)
    {
        if (strstr(data, "main_ps_1_1"))
        {
            target = "ps_1_1";
            entry = "main_ps_1_1";
        }
        if (strstr(data, "main_ps_1_2"))
        {
            target = "ps_1_2";
            entry = "main_ps_1_2";
        }
        if (strstr(data, "main_ps_1_3"))
        {
            target = "ps_1_3";
            entry = "main_ps_1_3";
        }
        if (strstr(data, "main_ps_1_4"))
        {
            target = "ps_1_4";
            entry = "main_ps_1_4";
        }
        if (strstr(data, "main_ps_2_0"))
        {
            target = "ps_2_0";
            entry = "main_ps_2_0";
        }
    }

    static inline DXIface* CreateHWShader(DWORD const* buffer, size_t size)
    {
        DXIface* ps = 0;
#ifdef USE_DX11
        R_CHK(HW.pDevice->CreatePixelShader(buffer, size, 0, &ps));
#elif defined(USE_DX10)
        R_CHK(HW.pDevice->CreatePixelShader(buffer, size, &ps));
#else
        R_CHK(HW.pDevice->CreatePixelShader(buffer, &ps));
#endif
        return ps;
    }

    static inline u32 GetShaderDest() { return RC_dest_pixel; }
};

#if defined(USE_DX10) || defined(USE_DX11)
template <>
struct ShaderTypeTraits<SGS>
{
    typedef CResourceManager::map_GS MapType;
    typedef ID3DGeometryShader DXIface;

    static inline const char* GetShaderExt() { return ".gs"; }
    static inline const char* GetCompilationTarget()
    {
#ifdef USE_DX10
        if (HW.pDevice1 == 0)
            return D3D10GetGeometryShaderProfile(HW.pDevice);
        else
            return "gs_4_1";
#endif
#ifdef USE_DX11
        if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_0)
            return "gs_4_0";
        else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_1)
            return "gs_4_1";
        else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
            return "gs_5_0";
#endif
        NODEFAULT;
        return "gs_4_0";
    }

    static void GetCompilationTarget(const char*& target, const char*& entry, const char* /*data*/)
    {
        target = GetCompilationTarget();
        entry = "main";
    }

    static inline DXIface* CreateHWShader(DWORD const* buffer, size_t size)
    {
        DXIface* gs = 0;
#ifdef USE_DX11
        R_CHK(HW.pDevice->CreateGeometryShader(buffer, size, 0, &gs));
#else
        R_CHK(HW.pDevice->CreateGeometryShader(buffer, size, &gs));
#endif       
        return gs;
    }

    static inline u32 GetShaderDest() { return RC_dest_geometry; }
};
#endif

#ifdef USE_DX11
template <>
struct ShaderTypeTraits<SHS>
{
    typedef CResourceManager::map_HS MapType;
    typedef ID3D11HullShader DXIface;

    static inline const char* GetShaderExt() { return ".hs"; }
    static inline const char* GetCompilationTarget() { return "hs_5_0"; }

    static void GetCompilationTarget(const char*& target, const char*& entry, const char* /*data*/)
    {
        target = GetCompilationTarget();
        entry = "main";
    }

    static inline DXIface* CreateHWShader(DWORD const* buffer, size_t size)
    {
        DXIface* hs = 0;
        R_CHK(HW.pDevice->CreateHullShader(buffer, size, NULL, &hs));
        return hs;
    }

    static inline u32 GetShaderDest() { return RC_dest_hull; }
};

template <>
struct ShaderTypeTraits<SDS>
{
    typedef CResourceManager::map_DS MapType;
    typedef ID3D11DomainShader DXIface;

    static inline const char* GetShaderExt() { return ".ds"; }
    static inline const char* GetCompilationTarget() { return "ds_5_0"; }

    static void GetCompilationTarget(const char*& target, const char*& entry, const char* /*data*/)
    {
        target = GetCompilationTarget();
        entry = "main";
    }

    static inline DXIface* CreateHWShader(DWORD const* buffer, size_t size)
    {
        DXIface* hs = 0;
        R_CHK(HW.pDevice->CreateDomainShader(buffer, size, NULL, &hs));
        return hs;
    }

    static inline u32 GetShaderDest() { return RC_dest_domain; }
};

template <>
struct ShaderTypeTraits<SCS>
{
    typedef CResourceManager::map_CS MapType;
    typedef ID3D11ComputeShader DXIface;

    static inline const char* GetShaderExt() { return ".cs"; }
    static inline const char* GetCompilationTarget() { return "cs_5_0"; }

    static void GetCompilationTarget(const char*& target, const char*& entry, const char* /*data*/)
    {
        target = GetCompilationTarget();
        entry = "main";
    }

    static inline DXIface* CreateHWShader(DWORD const* buffer, size_t size)
    {
        DXIface* cs = 0;
        R_CHK(HW.pDevice->CreateComputeShader(buffer, size, NULL, &cs));
        return cs;
    }

    static inline u32 GetShaderDest() { return RC_dest_compute; }
};
#endif

template <>
inline CResourceManager::map_PS& CResourceManager::GetShaderMap()
{
    return m_ps;
}

#if defined(USE_DX10) || defined(USE_DX11)
template <>
inline CResourceManager::map_GS& CResourceManager::GetShaderMap()
{
    return m_gs;
}
#endif

#if defined(USE_DX11)
template <>
inline CResourceManager::map_DS& CResourceManager::GetShaderMap()
{
    return m_ds;
}

template <>
inline CResourceManager::map_HS& CResourceManager::GetShaderMap()
{
    return m_hs;
}

template <>
inline CResourceManager::map_CS& CResourceManager::GetShaderMap()
{
    return m_cs;
}
#endif

template <typename T>
inline T* CResourceManager::CreateShader(const char* name, const bool searchForEntryAndTarget /*= false*/)
{
    ShaderTypeTraits<T>::MapType& sh_map = GetShaderMap<ShaderTypeTraits<T>::MapType>();
    LPSTR N = LPSTR(name);
    auto iterator = sh_map.find(N);

    if (iterator != sh_map.end())
        return iterator->second;
    else
    {
        T* sh = new T();

        sh->dwFlags |= xr_resource_flagged::RF_REGISTERED;
        sh_map.insert(std::make_pair(sh->set_name(name), sh));
        if (0 == xr_stricmp(name, "null"))
        {
            sh->sh = NULL;
            return sh;
        }

        // Remove ( and everything after it
        string_path shName;
        const char* pchr = strchr(name, '(');
        ptrdiff_t strSize = pchr ? pchr - name : xr_strlen(name);
        strncpy(shName, name, strSize);
        shName[strSize] = 0;

        // Open file
        string_path cname;
        strconcat(sizeof(cname), cname, GEnv.Render->getShaderPath(), /*name*/ shName,
            ShaderTypeTraits<T>::GetShaderExt());
        FS.update_path(cname, "$game_shaders$", cname);

        // duplicate and zero-terminate
        IReader* file = FS.r_open(cname);
        if (!file && strstr(Core.Params, "-lack_of_shaders"))
        {
            string1024 tmp;
            xr_sprintf(tmp, "CreateShader: %s is missing. Replacing it with stub_default%s", cname, ShaderTypeTraits<T>::GetShaderExt());
            Msg(tmp);
            strconcat(sizeof(cname), cname, GEnv.Render->getShaderPath(), "stub_default", ShaderTypeTraits<T>::GetShaderExt());
            FS.update_path(cname, "$game_shaders$", cname);
            file = FS.r_open(cname);
        }
        R_ASSERT2(file, cname);

        const auto size = file->length();
        char* const data = (LPSTR)_alloca(size + 1);
        CopyMemory(data, file->pointer(), size);
        data[size] = 0;
        FS.r_close(file);

        // Select target
        LPCSTR c_target = ShaderTypeTraits<T>::GetCompilationTarget();
        LPCSTR c_entry = "main";
        
        if (searchForEntryAndTarget)
            ShaderTypeTraits<T>::GetCompilationTarget(c_target, c_entry, data);

#ifdef USE_OGL
        DWORD flags = NULL;
#elif defined(USE_DX10) || defined(USE_DX11)
        DWORD flags = D3D10_SHADER_PACK_MATRIX_ROW_MAJOR;
#else
        DWORD flags = D3DXSHADER_DEBUG | D3DXSHADER_PACKMATRIX_ROWMAJOR;
#endif

        // Compile
        HRESULT const _hr = GEnv.Render->shader_compile(name, (DWORD const*)data, size,
            c_entry, c_target, flags, (void*&)sh);

        VERIFY(SUCCEEDED(_hr));

        CHECK_OR_EXIT(!FAILED(_hr), "Your video card doesn't meet game requirements.\n\nTry to lower game settings.");

        return sh;
    }
}

template <typename T>
inline void CResourceManager::DestroyShader(const T* sh)
{
    if (0 == (sh->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;

    ShaderTypeTraits<T>::MapType& sh_map = GetShaderMap<ShaderTypeTraits<T>::MapType>();

    LPSTR N = LPSTR(*sh->cName);
    auto iterator = sh_map.find(N);

    if (iterator != sh_map.end())
    {
        sh_map.erase(iterator);
        return;
    }
    Msg("! ERROR: Failed to find compiled shader '%s'", sh->cName.c_str());
}
