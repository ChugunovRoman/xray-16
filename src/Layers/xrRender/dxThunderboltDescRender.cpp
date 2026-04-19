#include "stdafx.h"
#include "dxThunderboltDescRender.h"

namespace xray::render::RENDER_NAMESPACE
{
void dxThunderboltDescRender::Copy(IThunderboltDescRender& _in) { *this = *((dxThunderboltDescRender*)&_in); }
void dxThunderboltDescRender::CreateModel(LPCSTR m_name)
{
    l_model = nullptr;

    // Missing mesh is common with bad mod configs; don't crash — try engine default used in stock thunderbolts.ltx.
    constexpr pcstr kFallbackLightningMesh = "dm\\dm_lightning-01.dm";

    IReader* F = (m_name && m_name[0]) ? FS.r_open("$game_meshes$", m_name) : nullptr;
    if (!F && m_name && m_name[0] && xr_stricmp(m_name, kFallbackLightningMesh) != 0)
    {
        Msg("! [thunderbolt] lightning_model not found: [%s], trying [%s]", m_name, kFallbackLightningMesh);
        F = FS.r_open("$game_meshes$", kFallbackLightningMesh);
    }

    if (!F)
    {
        Msg("! [thunderbolt] Could not load lightning mesh (requested [%s]). Sky gradients still work; fix gamedata.",
            m_name ? m_name : "");
        return;
    }

    l_model = RImplementation.model_CreateDM(F);
    FS.r_close(F);
}

void dxThunderboltDescRender::DestroyModel()
{
    if (l_model)
    {
        RImplementation.model_Delete(l_model);
        l_model = nullptr;
    }
}
} // namespace xray::render::RENDER_NAMESPACE
