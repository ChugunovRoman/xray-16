#include "pch_script.h"
#include "ai_space.h"
#include "object_factory.h"
#include "ai/monsters/ai_monster_squad_manager.h"

#include "entity_alive.h"
#include "ui/UIInventoryUtilities.h"
#include "ui/UIXmlInit.h"
#include "xrUICore/XML//UITextureMaster.h"

#include "InfoPortion.h"
#include "PhraseDialog.h"
#include "GameTask.h"
#include "encyclopedia_article.h"

#include "character_info.h"
#include "specific_character.h"
#include "character_community.h"
#include "monster_community.h"
#include "character_rank.h"
#include "character_reputation.h"

#include "xrEngine/profiler.h"

#include "sound_collection_storage.h"
#include "relation_registry.h"
#include "script_properties_list_helper.h"

extern CScriptPropertiesListHelper* g_property_list_helper;

typedef xr_vector<std::pair<shared_str, int>> STORY_PAIRS;
extern STORY_PAIRS story_ids;
extern STORY_PAIRS spawn_story_ids;

extern void release_smart_cast_stats();
extern void InitHudSoundSettings();
extern void gw_reset_specific_character_index(); // xrServer_Objects_ALife_Monsters.cpp

#include "xrEngine/IGame_Persistent.h"

namespace
{
// The data init_game_globals reads (the XML and the ini loaders): freed at the end of the game and before it is read
// again (reload_game_globals)
void clean_game_data()
{
    if (!GEnv.isDedicatedServer)
    {
        CInfoPortion::DeleteSharedData();
        CInfoPortion::DeleteIdToIndexData();

        CEncyclopediaArticle::DeleteSharedData();
        CEncyclopediaArticle::DeleteIdToIndexData();

        CPhraseDialog::DeleteSharedData();
        CPhraseDialog::DeleteIdToIndexData();
    }
    CCharacterInfo::DeleteSharedData();
    CCharacterInfo::DeleteIdToIndexData();

    // 53-create-squad-engine.md: the index of the specific characters points to their shared data
    gw_reset_specific_character_index();
    CSpecificCharacter::DeleteSharedData();
    CSpecificCharacter::DeleteIdToIndexData();

    CHARACTER_COMMUNITY::DeleteIdToIndexData();
    CHARACTER_RANK::DeleteIdToIndexData();
    CHARACTER_REPUTATION::DeleteIdToIndexData();
    MONSTER_COMMUNITY::DeleteIdToIndexData();

    // Очищение таблицы идентификаторов рангов и отношений сталкеров
    InventoryUtilities::ClearCharacterInfoStrings();
}
} // namespace

void init_game_globals()
{
    ZoneScoped;

    InitHudSoundSettings();
    if (!GEnv.isDedicatedServer)
    {
        CInfoPortion::InitInternal(ShadowOfChernobylMode || ClearSkyMode, true);
        CEncyclopediaArticle::InitInternal(ShadowOfChernobylMode, true);
        CPhraseDialog::InitInternal();
    };
    CCharacterInfo::InitInternal();
    CSpecificCharacter::InitInternal();
    CHARACTER_COMMUNITY::InitInternal();
    CHARACTER_COMMUNITY::BuildRelationTableFromFiles();
    CHARACTER_RANK::InitInternal();
    CHARACTER_REPUTATION::InitInternal();
    MONSTER_COMMUNITY::InitInternal();
}

void clean_game_globals()
{
    xr_delete(g_property_list_helper);

    // destroy ai space
    xr_delete(g_ai_space);
    // destroy object factory
    xr_delete(g_object_factory);
    // destroy monster squad global var
    xr_delete(g_monster_squad);

    story_ids.clear();
    spawn_story_ids.clear();

    clean_game_data();

    // static shader for blood
    CEntityAlive::UnloadBloodyWallmarks();
    CEntityAlive::UnloadFireParticles();

    xr_delete(g_sound_collection_storage);

#ifdef DEBUG
    xr_delete(g_profiler);
    release_smart_cast_stats();
#endif

    RELATION_REGISTRY::clear_relation_registry();
}

// plans/addons_api_support/00-roadmap.md 5a, decision 35: the content set of the addons changed (gw::slots
// ApplyContentSet: the main menu, or the end of CGamePersistent::OnAppStart before a level of the command line - no
// level, no ALife, init_game_globals of the start is done). The info portions, the encyclopedia, the dialogs, the
// profiles and the specific characters, the communities with creatures\relations, the ranks, the reputations and the
// monster communities are read again from the layers mounted now (system.ltx is read again before it). Their users keep
// ids (shared_str), not the data: the actor, the dialogs, the ALife registries are not there. A live object of these
// classes (a game object, a temporary on the stack) keeps a pointer into the shared data - then nothing is reloaded
bool reload_game_globals()
{
    ZoneScoped;

    const int users = (GEnv.isDedicatedServer ? 0 :
        CInfoPortion::SharedDataUsers() + CEncyclopediaArticle::SharedDataUsers() + CPhraseDialog::SharedDataUsers()) +
        CCharacterInfo::SharedDataUsers() + CSpecificCharacter::SharedDataUsers();
    if (users)
    {
        Msg("! [addons] the dialogs, info portions, profiles and communities are not read again: %d live object(s) "
            "hold their data; those of the previous content stay until a restart", users);
        return false;
    }

    CTimer timer;
    timer.Start();
    clean_game_data();
    init_game_globals();
    Msg("* [addons] the dialogs, info portions, encyclopedia, profiles, communities, ranks and reputations are read "
        "again: %d dialog(s), %d info portion(s), %d profile(s), %d specific character(s), %d communities, %.2f sec",
        GEnv.isDedicatedServer ? 0 : CPhraseDialog::GetMaxIndex() + 1,
        GEnv.isDedicatedServer ? 0 : CInfoPortion::GetMaxIndex() + 1, CCharacterInfo::GetMaxIndex() + 1,
        CSpecificCharacter::GetMaxIndex() + 1, int(CHARACTER_COMMUNITY::GetMaxIndex()) + 1, timer.GetElapsed_sec());
    return true;
}
