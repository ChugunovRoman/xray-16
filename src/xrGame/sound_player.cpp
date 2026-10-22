////////////////////////////////////////////////////////////////////////////
//	Module 		: sound_player.cpp
//	Created 	: 27.12.2003
//  Modified 	: 27.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Sound player
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "sound_player.h"
#include "xrScriptEngine/script_engine.hpp"
#include "ai/stalker/ai_stalker_space.h"
#include "ai_space.h"
#include "xrEngine/xr_object.h"
#include "xrCore/Animation/Bone.hpp"
#include "Include/xrRender/Kinematics.h"
#include "xrEngine/profiler.h"
#include "sound_collection_storage.h"
#include "Common/object_broker.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/BackgroundIo.h"

#include <atomic>

CSoundPlayer::CSoundPlayer(IGameObject* object)
{
    VERIFY(object);
    m_object = object;
    seed(u32(CPU::QPC() & 0xffffffff));
    m_sound_prefix = "";
}

CSoundPlayer::~CSoundPlayer() { clear(); }
void CSoundPlayer::clear()
{
    m_sounds.clear();

    xr_vector<CSoundSingle>::iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
        (*I).destroy();

    m_playing_sounds.clear();

    m_sound_mask = 0;
}

void CSoundPlayer::reinit() {}
void CSoundPlayer::reload(LPCSTR section)
{
    VERIFY(m_playing_sounds.empty());
    clear();
    m_sound_prefix = "";
}

void CSoundPlayer::unload()
{
    remove_active_sounds(u32(-1));
    VERIFY(m_playing_sounds.empty());
}

u32 CSoundPlayer::add(LPCSTR prefix, u32 max_count, ESoundTypes type, u32 priority, u32 mask, u32 internal_type,
    LPCSTR bone_name, CSound_UserDataPtr data)
{
    SOUND_COLLECTIONS::iterator I = m_sounds.find(internal_type);
    if (m_sounds.end() != I)
        return (0);

    CSoundCollectionParamsFull sound_params;
    sound_params.m_priority = priority;
    sound_params.m_synchro_mask = mask;
    sound_params.m_bone_name = bone_name;
    sound_params.m_sound_prefix = prefix;
    sound_params.m_sound_player_prefix = m_sound_prefix;
    sound_params.m_max_count = max_count;
    sound_params.m_type = type;
    sound_params.m_data = data;

    typedef CSoundCollectionStorage::SOUND_COLLECTION_PAIR SOUND_COLLECTION_PAIR;
    const SOUND_COLLECTION_PAIR& pair = sound_collection_storage().object(sound_params);
    VERIFY(pair.first == (CSoundCollectionParams&)sound_params);
    VERIFY(pair.second);
    m_sounds.emplace(internal_type, std::make_pair(sound_params, pair.second));
    return (pair.second->m_sounds.size());
}

void CSoundPlayer::remove(u32 internal_type)
{
    SOUND_COLLECTIONS::iterator I = m_sounds.find(internal_type);
    VERIFY(m_sounds.end() != I);
    m_sounds.erase(I);
}

bool CSoundPlayer::check_sound_legacy(u32 internal_type) const
{
    SOUND_COLLECTIONS::const_iterator J = m_sounds.find(internal_type);
    if (m_sounds.end() == J)
    {
#ifdef DEBUG
        GEnv.ScriptEngine->script_log(LuaMessageType::Message,
            "Can't find sound with internal type %d (sound_script = %d)", internal_type,
            StalkerSpace::eStalkerSoundScript);
#endif
        return (false);
    }

    VERIFY(m_sounds.end() != J);
    const CSoundCollectionParamsFull& sound = (*J).second.first;
    if (sound.m_synchro_mask & m_sound_mask)
        return (false);

    xr_vector<CSoundSingle>::const_iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::const_iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
        if ((*I).m_synchro_mask & sound.m_synchro_mask)
            if ((*I).m_priority <= sound.m_priority)
                return (false);
    return (true);
}

void CSoundPlayer::update(float time_delta)
{
    START_PROFILE("Sound Player")
    remove_inappropriate_sounds(m_sound_mask);
    update_playing_sounds();
    STOP_PROFILE
}

void CSoundPlayer::remove_inappropriate_sounds(u32 sound_mask)
{
    m_playing_sounds.erase(
        std::remove_if(m_playing_sounds.begin(), m_playing_sounds.end(), CInappropriateSoundPredicate(sound_mask)),
        m_playing_sounds.end());
}

void CSoundPlayer::update_playing_sounds()
{
    xr_vector<CSoundSingle>::iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
    {
        if ((*I).m_sound->_feedback())
            (*I).m_sound->_feedback()->set_position(compute_sound_point(*I));
        else if (!(*I).started() && (Device.dwTimeGlobal >= (*I).m_start_time))
        {
            // 50j-slow-disk-async-sound.md: a sound whose source was still read when it was asked for
            const bool waited = (*I).m_source_wait;
            if (waited && !source_wait_done(*I))
                continue;
            (*I).play_at_pos(m_object, compute_sound_point(*I));
            if (waited) // its length is known now
            {
                (*I).m_stop_time =
                    Device.dwTimeGlobal + iFloor((*I).m_sound->get_length_sec() * 1000.0f) + (*I).m_stop_extra;
            }
        }
    }
}

// plans/optimization_mt/50j-slow-disk-async-sound.md. A collection created in game on a slow disk reads its sources in
// the background (CSoundCollection::add, deferred); a play of a sound whose source is not read yet waits for it here,
// frame by frame (the source is queued as urgent: it is read before the other ones), at most snd_deferred_max_delay_ms
// after it was due to start. Then - or if the file failed - it is dropped, as a sound that ended (m_stop_time 0).
// Meanwhile it counts as a sound that is due: it holds its synchro mask, as a sound with a start delay does.
bool CSoundPlayer::source_wait_done(CSoundSingle& sound)
{
    const SoundSourceState state = sound.m_sound->source_state(true);
    if (state == SoundSourceState::Pending && Device.dwTimeGlobal < sound.m_start_time + snd_deferred_max_delay_ms)
        return false;
    sound.m_source_wait = false;
    if (state == SoundSourceState::Ready)
    {
        PERF_INC(snd_async_play_late);
        PERF_ADD(snd_async_play_late_ms, Device.dwTimeGlobal - sound.m_start_time);
        return true;
    }
    PERF_INC(snd_async_play_dropped);
    sound.m_started = true; // no play_at_pos: removed by the next remove_inappropriate_sounds
    sound.m_stop_time = 0;
    return false;
}

bool CSoundPlayer::need_bone_data() const
{
    xr_vector<CSoundSingle>::const_iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::const_iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
    {
        if ((*I).m_sound->_feedback())
            return (true);
        else if (!(*I).started() && (Device.dwTimeGlobal >= (*I).m_start_time))
            return (true);
    }
    return (false);
}

void CSoundPlayer::play(
    u32 internal_type, u32 max_start_time, u32 min_start_time, u32 max_stop_time, u32 min_stop_time, u32 id)
{
    if (!check_sound_legacy(internal_type))
        return;

    SOUND_COLLECTIONS::iterator I = m_sounds.find(internal_type);
    VERIFY(m_sounds.end() != I);
    CSoundCollectionParamsFull& sound = (*I).second.first;
    if ((*I).second.second->m_sounds.empty())
    {
#ifdef DEBUG
        Msg("- There are no sounds in sound collection \"%s\" with internal type %d (sound_script = %d)",
            sound.m_sound_prefix.c_str(), internal_type, StalkerSpace::eStalkerSoundScript);
#endif
        return;
    }

    remove_inappropriate_sounds(sound.m_synchro_mask);

    CSoundSingle sound_single;
    (CSoundParams&)sound_single = (CSoundParams&)sound;
    sound_single.m_bone_id = smart_cast<IKinematics*>(m_object->Visual())->LL_BoneID(sound.m_bone_name);
    R_ASSERT(sound_single.m_bone_id != BI_NONE);

    sound_single.m_sound = xr_new<ref_sound>();
    // 50j-slow-disk-async-sound.md: a random pick whose source is still read in the background gives way to a ready
    // sound of the collection, if there is one
    CSoundCollection& collection = *(*I).second.second;
    const ref_sound& picked = collection.random(id);
    const ref_sound& from = id == u32(-1) ? collection.ready_alternative(picked) : picked;
    /**
    sound_single.m_sound->clone	(
        *(*I).second.second->m_sounds[
            id == u32(-1)
            ?
            (*I).second.second->random(
                (*I).second.second->m_sounds.size()
            )
            :
            id
        ],
        st_Effect,
        sg_SourceType
    );
    **/
    sound_single.m_sound->clone(from, st_Effect, sg_SourceType);

    if (sound_single.m_sound->_get() == nullptr)
    {
        Msg("CSoundPlayer::play getted object pointer is null *sound.m_sound_prefix=[%s] m_object name=[%s] visual name=[%s]", sound.m_sound_prefix.c_str(), m_object->cName().c_str(), m_object->cNameVisual().c_str());
        return;
    }

    sound_single.m_sound->_get()->g_object = m_object;
    sound_single.m_sound->_get()->g_userdata = (*I).second.first.m_data;
    VERIFY(sound_single.m_sound->_handle());

    VERIFY(max_start_time >= min_start_time);
    VERIFY(max_stop_time >= min_stop_time);
    u32 random_time = 0;

    if (max_start_time)
        random_time = (max_start_time > min_start_time) ? random(max_start_time - min_start_time) + min_start_time :
                                                          max_start_time;

    sound_single.m_start_time = Device.dwTimeGlobal + random_time;

    random_time = 0;
    if (max_stop_time)
        random_time =
            (max_stop_time > min_stop_time) ? random(max_stop_time - min_stop_time) + min_stop_time : max_stop_time;

    // 50j-slow-disk-async-sound.md: a source that could not be read - nothing plays; one still read in the background -
    // the sound waits for it (update_playing_sounds), its stop time is set when it starts
    const SoundSourceState source = sound_single.m_sound->source_state(true);
    if (source == SoundSourceState::Failed)
    {
        PERF_INC(snd_async_play_dropped);
        xr_delete(sound_single.m_sound);
        return;
    }
    if (source == SoundSourceState::Pending)
    {
        sound_single.m_source_wait = true;
        sound_single.m_stop_extra = random_time;
        sound_single.m_stop_time = u32(-1);
        m_playing_sounds.push_back(sound_single);
        return;
    }
    // 50j: the source may have been read between clone() (a deferred clone, length 0) and the state above - its length
    // is taken now, before the stop time (a no-op for a sound that is not deferred; the source is ready: no read)
    sound_single.m_sound->resolve_deferred();

    sound_single.m_stop_time =
        sound_single.m_start_time + iFloor(sound_single.m_sound->get_length_sec() * 1000.0f) + random_time;
    m_playing_sounds.push_back(sound_single);

    if (Device.dwTimeGlobal >= m_playing_sounds.back().m_start_time)
        m_playing_sounds.back().play_at_pos(m_object, compute_sound_point(m_playing_sounds.back()));
}

IC Fvector CSoundPlayer::compute_sound_point(const CSoundSingle& sound)
{
    Fmatrix l_tMatrix;
    l_tMatrix.mul_43(m_object->XFORM(),
        smart_cast<IKinematics*>(m_object->Visual())->LL_GetBoneInstance(sound.m_bone_id).mTransform);
    return (l_tMatrix.c);
}

namespace
{
// plans/optimization_mt/50j-slow-disk-async-sound.md: a collection created in game (an NPC spawn) reads its sources in
// the background on a slow disk (CSoundRender_Core decides on the disk; elsewhere create_deferred is create). On the
// loading screen they are read here, as before: the load waits anyway, and their reads do not move into the game.
bool sound_player_collection_deferred()
{
    return !load_screen_renderer.IsActive() && Device.dwPrecacheFrame == 0;
}
} // namespace

CSoundPlayer::CSoundCollection::CSoundCollection(const CSoundCollectionParams& params)
    : m_last_sound_id(u32(-1))
{
    GW_FRAME_EVENT(SoundSet); // 50n: the frame spike report (a sound set built: FS lookups + its sources)
    seed(u32(CPU::QPC() & 0xffffffff));
    m_sounds.clear();
    const bool deferred = sound_player_collection_deferred();
    for (int j = 0, N = _GetItemCount(params.m_sound_prefix.c_str()); j < N; ++j)
    {
        string_path fn, s, temp;
        pstr S = (pstr)&s;
        _GetItem(params.m_sound_prefix.c_str(), j, temp);
        strconcat(sizeof(s), S, params.m_sound_player_prefix.c_str(), temp);
        if (FS.exist(fn, "$game_sounds$", S, ".ogg"))
        {
            ref_sound* temp = add(params.m_type, S, deferred);
            if (temp)
                m_sounds.push_back(temp);
        }
        for (u32 i = 0; i < params.m_max_count; ++i)
        {
            string256 name;
            xr_sprintf(name, "%s%d", S, i);
            if (FS.exist(fn, "$game_sounds$", name, ".ogg"))
            {
                ref_sound* temp = add(params.m_type, name, deferred);
                if (temp)
                    m_sounds.push_back(temp);
            }
        }
    }
#ifdef DEBUG
    if (m_sounds.empty())
        Msg("- There are no sounds with prefix %s", params.m_sound_prefix.c_str());
#endif
    // plans/optimization_mt/50z3-startup-slow-disk-io.md: the laptop (USB HDD) showed no trace of 50j - this line tells
    // whether a set built in game took the deferred path at all (pending = its sources left to the background thread;
    // 0 of N = they were all read before, e.g. by the sets built on the loading screen)
    if (deferred && xray::io::BackgroundIoSingle())
    {
        static std::atomic_bool s_logged{false}; // main or the Game thread
        if (!s_logged.exchange(true, std::memory_order_relaxed))
        {
            u32 pending = 0;
            for (const ref_sound* snd : m_sounds)
                pending += snd->source_state() == SoundSourceState::Pending ? 1 : 0;
            Msg("* [hw] sound: the first NPC sound set built in game on a slow disk ('%s%s'): %u sounds, %u of them read "
                "in the background",
                params.m_sound_player_prefix.c_str(), params.m_sound_prefix.c_str(), static_cast<u32>(m_sounds.size()),
                pending);
        }
    }
}

CSoundPlayer::CSoundCollection::~CSoundCollection()
{
#ifdef DEBUG
    xr_vector<ref_sound*>::iterator I = m_sounds.begin();
    xr_vector<ref_sound*>::iterator E = m_sounds.end();
    for (; I != E; ++I)
    {
        VERIFY(*I);
        VERIFY(!(*I)->_feedback());
    }
#endif
    delete_data(m_sounds);
}

const ref_sound& CSoundPlayer::CSoundCollection::random(u32 id)
{
    VERIFY(!m_sounds.empty());

    if (id != u32(-1) && id < m_sounds.size())
    {
        m_last_sound_id = id;
        return (*m_sounds[id]);
    }

    if (m_sounds.size() <= 2)
    {
        m_last_sound_id = CRandom32::random(m_sounds.size());
        return (*m_sounds[m_last_sound_id]);
    }

    u32 result;
    do
    {
        result = CRandom32::random(m_sounds.size());
    } while (result == m_last_sound_id);

    m_last_sound_id = result;
    return *m_sounds[result];
}

const ref_sound& CSoundPlayer::CSoundCollection::ready_alternative(const ref_sound& picked)
{
    if (picked.source_state() != SoundSourceState::Pending)
        return picked; // always so on a fast disk and on the loading screen
    const u32 count = m_sounds.size();
    const u32 first = CRandom32::random(count);
    for (u32 i = 0; i < count; ++i)
    {
        const u32 index = (first + i) % count;
        if (m_sounds[index]->source_state() == SoundSourceState::Ready)
        {
            PERF_INC(snd_async_play_substituted);
            m_last_sound_id = index;
            return *m_sounds[index];
        }
    }
    return picked;
}
