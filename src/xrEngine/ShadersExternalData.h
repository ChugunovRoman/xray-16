#pragma once

// Хранилище внешних шейдерных параметров, которые читаются в Blender_Recorder_StandartBinding.cpp
class ShadersExternalData //--#SM+#--
{
public:
    /// Базовое значение зума линзы прицела (без приближения). Используется в C++ и передаётся в шейдеры через hud_params_2.y
    static constexpr float SCOPE_LENSE_ZOOM_DEFAULT = 1.0f;

    Fmatrix m_script_params; // Матрица, значения которой доступны из Lua
    Fvector4 hud_params_2;     // [scope_textures_size, scope_lense_zoom_default, NULL, NULL] - Параметры худа оружия
    Fvector4 hud_params;     // [zoom_rotate_factor, secondVP_zoom_factor, NULL, NULL]; w = scope lens zoom [0.1, 1.0], default 1.0
    Fvector4 m_blender_mode; // x\y = [0 - default, 1 - night vision, 2 - thermo vision, ... см. common.h] - Режимы рендеринга
                             // x - основной вьюпорт, y - второй вьюпорт, z = ?, w = [0 - идёт рендер обычного объекта, 1 - идёт рендер детальных объектов (трава, мусор)]
    Fvector4 shader_param_7;  // Пользовательские шейдерные параметры (NVG / другие эффекты)
    Fvector4 shader_param_8;  // Пользовательские шейдерные параметры (NVG / другие эффекты)

    ShadersExternalData()
    {
        m_script_params = Fmatrix();
        hud_params_2.set(0.f, 0.f, 0.f, 0.f);
        hud_params.set(0.f, 0.f, 0.f, 0.f);
        m_blender_mode.set(0.f, 0.f, 0.f, 0.f);
        shader_param_7.set(0.f, 0.f, 0.f, 0.f);
        shader_param_8.set(0.f, 0.f, 0.f, 0.f);
    }
};
