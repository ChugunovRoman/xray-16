#pragma once
#include "xrMessages.h"
#include "xrCommon/xr_deque.h"

extern BOOL g_bCheckTime;
extern int g_dwEventDelay;

class NET_Event
{
public:
    u16 ID;
    u32 timestamp;
    u16 type;
    u16 destination;
    xr_vector<u8> data;
    // M_SPAWN only (plans/spawn_optimization/SPEC.md, stage 0 / 8.6): header fields read once at insert, so the queue can
    // look at a spawn without creating its CSE. 0xffff when the header could not be read.
    u16 spawn_id{u16(-1)};
    u16 spawn_parent{u16(-1)};
    u32 time_receive{}; // NET_Packet::timeReceive (IPureClient::timeServer_Async when the server sent it), 0 = unknown
    // Spawn stage 2 (SPEC.md 5.A): CPU::GetTicks() (real time, ms) when the event entered the client queue, for the
    // delay limit of the spawn budget (spawn_max_delay_ms) and spawn/queue_wait_ms
    u32 time_queued{};

public:
    void import(NET_Packet& P)
    {
        data.clear();
        P.r_begin(ID); // VERIFY(M_EVENT==ID);
        switch (ID)
        {
        case M_SPAWN:
        {
            P.read_start();
            //				timestamp = P->
            time_receive = P.timeReceive;
        }
        break;
        case M_EVENT:
        {
            P.r_u32(timestamp);
            timestamp += u32(g_dwEventDelay);
            P.r_u16(type);
            P.r_u16(destination);
        }
        break;
        case M_MOVE_PLAYERS:
        case M_STATISTIC_UPDATE:
        case M_FILE_TRANSFER:
        case M_GAMEMESSAGE: {
        }
        break;
        default: { VERIFY(0);
        }
        break;
        }

        u32 size = P.r_elapsed();
        if (size)
        {
            data.resize(size);
            P.r(&*data.begin(), size);
        }
        if (ID == M_SPAWN)
            read_spawn_header();
    }

    // The section (s_name) of an M_SPAWN, "" if the data is malformed. data = the whole packet: u16 M_SPAWN, then the
    // header of CSE_Abstract::Spawn_Write (xrServer_Object_Base.cpp).
    [[nodiscard]] pcstr spawn_section() const
    {
        if (ID != M_SPAWN || data.size() < sizeof(u16) + 1)
            return "";
        const auto begin = data.begin() + sizeof(u16);
        return std::find(begin, data.end(), u8(0)) == data.end() ? "" : reinterpret_cast<pcstr>(&*begin);
    }

private:
    // Spawn_Write: u16 M_SPAWN, stringZ s_name, stringZ name_replace, u8, u8 s_RP, vec3 o_Position, vec3 o_Angle,
    // u16 RespawnTime, u16 ID, u16 ID_Parent, ...
    void read_spawn_header()
    {
        spawn_id = u16(-1);
        spawn_parent = u16(-1);
        size_t pos = sizeof(u16);
        for (int string = 0; string < 2; ++string)
        {
            const auto end = std::find(data.begin() + std::min(pos, data.size()), data.end(), u8(0));
            if (end == data.end())
                return;
            pos = size_t(end - data.begin()) + 1;
        }
        pos += 2 * sizeof(u8) + 2 * sizeof(Fvector) + sizeof(u16);
        if (pos + 2 * sizeof(u16) > data.size())
            return;
        CopyMemory(&spawn_id, &data[pos], sizeof(u16));
        CopyMemory(&spawn_parent, &data[pos + sizeof(u16)], sizeof(u16));
    }

public:
    void Export(NET_Packet& P)
    {
        u16 ID = M_EVENT;
        P.w_begin(ID);
        P.w_u32(timestamp);
        P.w_u16(type);
        P.w_u16(destination);
        if (data.size())
            P.w(&*data.begin(), (u32)data.size());
    }
    void implication(NET_Packet& P) const
    {
        if (data.size())
            CopyMemory(P.B.data, &*data.begin(), (u32)data.size());
        P.B.count = (u32)data.size();
        P.r_pos = 0;
    }
};

IC bool operator<(const NET_Event& A, const NET_Event& B) { return A.timestamp < B.timestamp; }
class NET_Queue_Event
{
public:
    //	xr_multiset<NET_Event>	queue;
    xr_deque<NET_Event> queue;

public:
    IC void insert(NET_Packet& P)
    {
        NET_Event E;
        E.import(P);
        E.time_queued = CPU::GetTicks();
        //		queue.insert	(E);
        queue.push_back(E);
        /*
        //-------------------------------------------
#ifdef DEBUG
        shared_str EventName;
        string16 tmp;

        switch (E.type)
        {
        case 1: EventName = "GE_OWNERSHIP_TAKE [1]"; break;
        case 2: EventName = "GE_OWNERSHIP_REJECT [2]"; break;
        case 5: EventName = "GE_DIE [5]"; break;
        case 7: EventName = "GE_DESTROY [7]"; break;
        default: EventName = xr_itoa(E.type, tmp, 10); break;
        }

        Msg("Event %s to %d - at %d", *EventName, E.destination, E.timestamp);
#endif
        //-------------------------------------------
        //*/
    }
    IC BOOL available(u32 T)
    {
        //		if (queue.empty()/* || (T<queue.begin()->timestamp)*/)	return FALSE;
        //		else												return TRUE;
        if (queue.empty())
            return FALSE;
        /**
        else
        {
            if (!g_bCheckTime) return TRUE;
#ifdef _DEBUG
            if (T<queue.begin()->timestamp) return FALSE;
#endif
            return TRUE;
        }
        **/
        return TRUE;
    }
    [[nodiscard]] size_t size() const { return queue.size(); }
    // The next event get() would return; the queue must not be empty.
    [[nodiscard]] const NET_Event& front() const { return queue.front(); }

    IC void get(u16& ID, u16& dest, u16& type, NET_Packet& P)
    {
        const NET_Event& E = *queue.begin();
        ID = E.ID;
        dest = E.destination;
        type = E.type;
        E.implication(P);
        //		queue.erase			(queue.begin());
        queue.pop_front();
    }
};
