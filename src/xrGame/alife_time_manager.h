////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_time_manager.h
//	Created 	: 05.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife time manager class
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/object_interfaces.h"
#include "alife_space.h"

class CALifeTimeManager
{
private:
    ALife::_TIME_ID m_game_time;
    float m_time_factor;
    float m_normal_time_factor;
    u32 m_start_time;
    // plans/lua_to_cpp/55-timeskip-squad-movement.md: the movement time cursor of a time skip (alife_time_skip.h). 0 -
    // no skip: the offline movement follows game_time(). Otherwise the clock has jumped already and the offline
    // movement (CALifeMonsterDetailPathManager) goes on only up to the cursor, which the catch-up moves step by step.
    // Not saved: a save finishes the catch-up first
    ALife::_TIME_ID m_skip_cursor;

private:
    ALife::_TIME_ID m_start_game_time;

public:
    CALifeTimeManager(LPCSTR section);
    virtual ~CALifeTimeManager();
    virtual void save(IWriter& memory_stream);
    virtual void load(IReader& file_stream);
    void init(LPCSTR section);
    IC void set_time_factor(float time_factor);
    IC ALife::_TIME_ID start_game_time() const;
    IC ALife::_TIME_ID game_time() const;
    IC float time_factor() const;
    IC float normal_time_factor() const;
    IC void change_game_time(u32 value);
    IC void set_game_time_factor(ALife::_TIME_ID gameTime, const float timeFactor);
    // Plan 55: the time of the offline movement - the cursor of a running time skip, else game_time()
    IC ALife::_TIME_ID movement_time() const;
    IC ALife::_TIME_ID skip_cursor() const { return m_skip_cursor; }
    IC void set_skip_cursor(ALife::_TIME_ID cursor) { m_skip_cursor = cursor; }
    // Plan 55: for a step of a time skip the clock shows `time` (earlier than now); the answer goes to shift_clock_back
    // after the step, which gives the clock its time again (the real time of the step included)
    IC ALife::_TIME_ID shift_clock_to(ALife::_TIME_ID time);
    IC void shift_clock_back(ALife::_TIME_ID shift) { m_game_time += shift; }
};

#include "alife_time_manager_inline.h"
