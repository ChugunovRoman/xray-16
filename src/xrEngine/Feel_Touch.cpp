#include "stdafx.h"
#include <tracy/Tracy.hpp>
#include "IGame_Level.h"
#include "Feel_Touch.h"
#include "xr_object.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
using namespace Feel;

Touch::Touch() : pure_relcase(&Touch::feel_touch_relcase, &Touch::feel_touch_relcase_batch) {}
Touch::~Touch() {}
bool Touch::feel_touch_contact(IGameObject* O) { return true; }
void Touch::feel_touch_deny(IGameObject* O, u32 T)
{
    DenyTouch D;
    D.O = O;
    D.Expire = Device.dwTimeGlobal + T;
    feel_touch_disable.push_back(D);
}

void Touch::feel_touch_update(Fvector& C, float R)
{
    ZoneScopedN("Touch::feel_touch_update");
    // Check if denied objects expire in time
    u32 dwT = Device.dwTimeGlobal;
    for (u32 dit = 0; dit < feel_touch_disable.size(); dit++)
    {
        if (feel_touch_disable[dit].Expire < dwT)
        {
            feel_touch_disable.erase(feel_touch_disable.begin() + dit);
            dit--;
        }
    }

    // Find nearest objects
    q_nearest.clear();
    q_nearest.reserve(feel_touch.size());
    g_pGameLevel->ObjectSpace.GetNearest(q_nearest, C, R, NULL);
    xr_vector<IGameObject*>::iterator n_begin = q_nearest.begin();
    xr_vector<IGameObject*>::iterator n_end = q_nearest.end();

    // The membership tests below (is a near object touched already, is a touched one still near) were linear finds: a
    // quadratic walk with hundreds of objects around (a 28 ms frame of the Game thread). From kSortedFrom items on
    // they look into sorted copies; the order of the lists and of the callbacks stays as it was.
    constexpr size_t kSortedFrom = 32;
    thread_local xr_vector<IGameObject*> touched_sorted;
    thread_local xr_vector<IGameObject*> near_sorted;
    const bool sorted_touched = feel_touch.size() >= kSortedFrom && q_nearest.size() >= kSortedFrom;
    const bool sorted_near = q_nearest.size() >= kSortedFrom && feel_touch.size() >= kSortedFrom;
    if (sorted_touched)
    {
        // the objects added by the loop below come from q_nearest (no duplicates there): the snapshot is enough
        touched_sorted.assign(feel_touch.begin(), feel_touch.end());
        std::sort(touched_sorted.begin(), touched_sorted.end());
    }
    const auto is_touched = [&](IGameObject* O) {
        return sorted_touched ? std::binary_search(touched_sorted.begin(), touched_sorted.end(), O) :
                                std::find(feel_touch.begin(), feel_touch.end(), O) != feel_touch.end();
    };
    u32 touched_new = 0, touched_deleted = 0;

    if (n_end != n_begin)
    {
        // Process results (NEW)
        for (xr_vector<IGameObject*>::iterator it = n_begin; it != n_end; ++it)
        {
            IGameObject* O = *it;
            if (O->getDestroy())
                continue; // Don't touch candidates for destroy
            if (!feel_touch_contact(O))
                continue; // Actual contact

            if (!is_touched(O))
            {
                // check for deny
                bool bDeny = false;
                for (u32 dit = 0; dit < feel_touch_disable.size(); dit++)
                    if (O == feel_touch_disable[dit].O)
                    {
                        bDeny = true;
                        break;
                    }

                // _new _
                if (!bDeny)
                {
                    feel_touch.push_back(O);
                    ZoneScopedN("feel_touch_new");
                    feel_touch_new(O);
                    ++touched_new;
                }
            }
        }
    }

    if (sorted_near)
    {
        near_sorted.assign(n_begin, n_end);
        std::sort(near_sorted.begin(), near_sorted.end());
    }
    const auto is_near = [&](IGameObject* O) {
        return sorted_near ? std::binary_search(near_sorted.begin(), near_sorted.end(), O) :
                             std::find(n_begin, n_end, O) != n_end;
    };

    // Process results (DELETE)
    for (int d = 0; d < int(feel_touch.size()); d++)
    {
        IGameObject* O = feel_touch[d];
        if (O->getDestroy() || !feel_touch_contact(O) || !is_near(O)) // Don't touch candidates for destroy
        {
            // _delete_
            feel_touch.erase(feel_touch.begin() + d);
            ZoneScopedN("feel_touch_delete");
            feel_touch_delete(O);
            ++touched_deleted;
            d--;
        }
    }

    // Whose touch it is and how much it had: a spike names its owner class
    if (TracyIsConnected)
    {
        string256 text;
        xr_sprintf(text, "%s near=%u touched=%u new=%u deleted=%u", typeid(*this).name(), u32(q_nearest.size()),
            u32(feel_touch.size()), touched_new, touched_deleted);
        ZoneText(text, xr_strlen(text));
    }

    //. Engine.Sheduler.Slice ();
}

void Touch::feel_touch_relcase(IGameObject* O)
{
    xr_vector<IGameObject*>::iterator I = std::find(feel_touch.begin(), feel_touch.end(), O);
    if (I != feel_touch.end())
    {
        feel_touch.erase(I);
        feel_touch_delete(O);
    }
    xr_vector<DenyTouch>::iterator Id = feel_touch_disable.begin(), IdE = feel_touch_disable.end();
    for (; Id != IdE; ++Id)
        if ((*Id).O == O)
        {
            feel_touch_disable.erase(Id);
            break;
        }
}

void Touch::feel_touch_relcase_batch(const RelcaseBatch& batch)
{
    PERF_SCOPE(relcase_cb_touch);
    // ~3500 subscribers (a Feel::Touch of every NPC and monster, the PDA in every NPC inventory, zones...), most with
    // empty lists: feel_touch_delete is called only for a found object, so with empty lists no call changes anything
    if (feel_touch.empty() && feel_touch_disable.empty())
    {
        PERF_INC(relcase_touch_skips);
        return;
    }
    // 32-frame-spikes.md, the callback scan: one walk of both lists with the pointer hash prefilter of contains() -
    // if no batch object is in them, every feel_touch_relcase below finds nothing and calls nothing (feel_touch_delete
    // only for a found object), so skipping them is the same; otherwise all of them run in the batch order, as before
    if (batch.ptr_bits)
    {
        bool found = false;
        for (const IGameObject* object : feel_touch)
        {
            if (batch.contains(object))
            {
                found = true;
                break;
            }
        }
        for (u32 i = 0; !found && i < feel_touch_disable.size(); ++i)
            found = batch.contains(feel_touch_disable[i].O);
        if (!found)
        {
            PERF_INC(relcase_touch_scan_skips);
            return;
        }
    }
    for (u32 it = 0; it < batch.count; ++it)
        feel_touch_relcase(batch.objects[it]);
}
