// file:		UITextureMaster.h
// description:	holds info about shared textures. able to initialize external
//				through IUITextureControl interface
// created:		11.05.2005
// author:		Serge Vynnychenko
// mail:		narrator@gsc-game.kiev.ua
//
// copyright 2005 GSC Game World

#pragma once

class CUIXml;
class CUIStaticItem;
#include "xrUICore/ui_defs.h"

struct TEX_INFO
{
    shared_str file;
    Frect rect;
    pcstr get_file_name() const { return file.c_str(); }
    Frect get_rect() { return rect; }
};

struct sh_pair
{
    shared_str texture_name;
    shared_str shader_name;
    bool operator<(const sh_pair& other) const
    {
        if (texture_name < other.texture_name)
            return true;
        else
            return shader_name < other.shader_name;
    }
};

class XRUICORE_API CUITextureMaster
{
public:
    static void ParseShTexInfo(pcstr xml_file);
    // owner - the addon of the layer of the file ("base"; plans/addons_api_support/09-content-addons-vfs.md 5.2):
    // an id two addons give is a conflict in the log (the later file wins as before)
    static void ParseShTexInfo(pcstr path, pcstr xml_file, pcstr owner = nullptr);
    static void ParseShTexInfo(CUIXml& xml, bool override, pcstr owner = nullptr);
    static void FreeTexInfo();
    static void FreeCachedShaders();

    static bool InitTexture(
        const shared_str& texture_name, CUIStaticItem* tc, const shared_str& shader_name = "hud" DELIMITER "default");
    static bool InitTexture(
        const shared_str& texture_name, const shared_str& shader_name, ui_shader& out_shader, Frect& out_rect);
    static float GetTextureHeight(const shared_str& texture_name);
    static float GetTextureWidth(const shared_str& texture_name);
    static bool GetTextureHeight(const shared_str& texture_name, float& outValue);
    static bool GetTextureWidth(const shared_str& texture_name, float& outValue);
    static Frect GetTextureRect(const shared_str& texture_name);
    static pcstr GetTextureFileName(pcstr texture_name);
    static void GetTextureShader(const shared_str& texture_name, ui_shader& sh);
    static TEX_INFO FindItem(const shared_str& texture_name, pcstr default_texture = nullptr);
    static bool FindItem(const shared_str& texture_name, TEX_INFO& outValue);
    static bool FindItem(const shared_str& texture_name, pcstr default_texture, TEX_INFO& outValue);
    static bool ItemExist(const shared_str& texture_name);

protected:
    IC static bool IsSh(const shared_str& texture_name);

    static xr_map<xr_string, TEX_INFO> m_textures;

    static xr_map<sh_pair, ui_shader> m_shaders;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};
