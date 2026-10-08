#pragma once
#include "xrCommon/xr_vector.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include <mutex>
#include <thread>

// messages
constexpr int REG_PRIORITY_LOW = 0x11111111;
constexpr int REG_PRIORITY_NORMAL = 0x22222222;
constexpr int REG_PRIORITY_HIGH = 0x33333333;
constexpr int REG_PRIORITY_CAPTURE = 0x7fffffff;
constexpr int REG_PRIORITY_INVALID = std::numeric_limits<int>::lowest();

#define DECLARE_MESSAGE(name)\
struct pure##name\
{\
    virtual void On##name() = 0;\
    static ICF void __fastcall OnPure(pure##name* self) { self->On##name(); }\
}

DECLARE_MESSAGE(Frame); // XXX: rename to FrameStart
DECLARE_MESSAGE(FrameEnd);
DECLARE_MESSAGE(Render);
DECLARE_MESSAGE(AppActivate);
DECLARE_MESSAGE(AppDeactivate);
DECLARE_MESSAGE(AppStart);
DECLARE_MESSAGE(AppEnd);
DECLARE_MESSAGE(DeviceReset);
DECLARE_MESSAGE(UIReset);

// RAII for MessageRegistry::add_msg_mtx (GAME_RENDER_OVERLAP_PLAN P1.4): try_lock()-first so lock
// contention is measured (msgreg_lock_contend / msgreg_lock_wait) now that the frame overlap is real.
struct MessageRegistryScopeLock
{
    std::mutex& m;

    explicit MessageRegistryScopeLock(std::mutex& mtx) noexcept : m(mtx)
    {
#if XR_PERF_METRICS
        if (!m.try_lock())
        {
            PERF_INC(msgreg_lock_contend);
            const bool metrics_active = xray::perf::g_perf_metrics_active;
            const u64 t0 = metrics_active ? xray::perf::Ticks() : 0;
            m.lock();
            if (metrics_active)
                xray::perf::AddTimeSlow(xray::perf::Metric::msgreg_lock_wait, xray::perf::Ticks() - t0);
        }
#else
        m.lock();
#endif
    }
    ~MessageRegistryScopeLock() { m.unlock(); }

    MessageRegistryScopeLock(const MessageRegistryScopeLock&) = delete;
    MessageRegistryScopeLock& operator=(const MessageRegistryScopeLock&) = delete;
};

template<class T>
class MessageRegistry
{
    struct MessageObject
    {
        T* Object;
        int Prio;
    };

    std::mutex add_msg_mtx;
    xr_vector<MessageObject> messages;
    // GAME_RENDER_OVERLAP_PLAN P1.4: Process() iterates `messages` without holding add_msg_mtx (it
    // can't - OnPure() callbacks reentrantly call Add()/Remove() on the SAME thread, e.g.
    // CUISequencer::Start/CMainMenu::Activate from Lua effectors, and a plain std::mutex would
    // deadlock on that). A call from a DIFFERENT thread while Process() is running must therefore
    // not touch `messages` directly - it goes here instead, under add_msg_mtx, and Process() merges
    // it in right after the loop. The reentrant same-thread path is unaffected, same as before.
    // One queue in call order (not separate add/remove lists): a foreign Remove(X); Add(X) must end
    // with X registered. Caveat: a deferred Remove() returns before the running loop has finished, so
    // a foreign thread must not destroy the object right after Remove() while Process() may still be
    // iterating - today no registry is touched from a foreign thread during Process().
    struct PendingOp
    {
        T* Object;
        int Prio; // REG_PRIORITY_INVALID = remove
    };
    xr_vector<PendingOp> m_pending;
    std::thread::id m_processing_thread{};
    bool changed;
    // Depth instead of a bool: a nested Process() of the same registry on the same thread must not end
    // the outer one's "in process" state.
    int m_process_depth = 0;
    // GW_OVERLAP_HAZARD (METRICS_PLAN_2026-10-02.md §8.7): this template is instantiated for many
    // unrelated message queues (seqFrame, seqAppActivate, ...), but the hazard probe must only warn
    // for Device.seqRender. Left nullptr (disabled) for every instance except the one SetHazardSite()
    // is called on - see CRenderDevice's constructor in device.h.
    const char* hazard_site = nullptr;

    // add_msg_mtx must be held.
    bool IsForeignThreadDuringProcess() const
    {
        return m_process_depth > 0 && std::this_thread::get_id() != m_processing_thread;
    }

    // add_msg_mtx must be held; only when no Process() loop is running.
    void ApplyPendingLocked()
    {
        if (m_pending.empty())
            return;
        for (const PendingOp& op : m_pending)
        {
            if (op.Prio != REG_PRIORITY_INVALID)
                messages.push_back({ op.Object, op.Prio });
            else
            {
                for (auto& message : messages)
                    if (message.Object == op.Object)
                        message.Prio = REG_PRIORITY_INVALID;
            }
        }
        m_pending.clear();
        changed = true;
    }

public:
    MessageRegistry() : changed(false) {}

    void Clear()
    {
        MessageRegistryScopeLock lock(add_msg_mtx);
        VERIFY(m_process_depth == 0);
        messages.clear();
        m_pending.clear();
        changed = false;
    }

    // Opt-in GW_OVERLAP_HAZARD label for this particular instance; nullptr (default) keeps the probe
    // disabled for Add()/Remove() on this MessageRegistry.
    void SetHazardSite(const char* site) { hazard_site = site; }

    constexpr void Add(T* object, const int priority = REG_PRIORITY_NORMAL)
    {
        Add({ object, priority });
    }

    void Add(MessageObject&& newMessage)
    {
        if (hazard_site)
            GW_OVERLAP_HAZARD(hazard_site);
        VERIFY(newMessage.Object);
        VERIFY(newMessage.Prio != REG_PRIORITY_INVALID);
        MessageRegistryScopeLock lock(add_msg_mtx);

        if (IsForeignThreadDuringProcess())
        {
            m_pending.push_back({ newMessage.Object, newMessage.Prio });
            return;
        }

#ifdef DEBUG
        // Verify that we don't already have the same object with valid priority (under the lock: a
        // reentrant Add() from the processing thread may reallocate `messages`).
        for (size_t i = 0; i < messages.size(); ++i)
        {
            auto& message = messages[i];
            VERIFY(!(message.Prio != REG_PRIORITY_INVALID && message.Object == newMessage.Object));
        }
#endif
        messages.emplace_back(newMessage);

        if (m_process_depth > 0)
            changed = true;
        else
            Resort();
    }

    void Remove(T* object)
    {
        if (hazard_site)
            GW_OVERLAP_HAZARD(hazard_site);
        MessageRegistryScopeLock lock(add_msg_mtx);

        if (IsForeignThreadDuringProcess())
        {
            m_pending.push_back({ object, REG_PRIORITY_INVALID });
            return;
        }

        for (size_t i = 0; i < messages.size(); ++i)
        {
            auto& message = messages[i];
            if (message.Object == object)
                message.Prio = REG_PRIORITY_INVALID;
        }

        if (m_process_depth > 0)
            changed = true;
        else
            Resort();
    }

    void Process()
    {
        {
            MessageRegistryScopeLock lock(add_msg_mtx);
            VERIFY(m_process_depth == 0 || std::this_thread::get_id() == m_processing_thread);
            if (m_process_depth == 0)
            {
                // Anything still queued (e.g. queued after the previous Process() merged) lands now,
                // also when `messages` itself is empty.
                ApplyPendingLocked();
                if (changed)
                    Resort();
            }
            if (messages.empty())
                return;
            if (m_process_depth++ == 0)
                m_processing_thread = std::this_thread::get_id();
        }

        if (messages[0].Prio == REG_PRIORITY_CAPTURE)
            messages[0].Object->OnPure(messages[0].Object);
        else
        {
            for (size_t i = 0; i < messages.size(); ++i)
            {
                const auto& message = messages[i];
                if (message.Prio != REG_PRIORITY_INVALID)
                    message.Object->OnPure(message.Object);
            }
        }

        // Merge whatever a foreign thread queued while the loop above was reading `messages` unlocked,
        // resort and leave the "in process" state under one lock hold - no window in which a foreign
        // call would still be queued but never merged.
        {
            MessageRegistryScopeLock lock(add_msg_mtx);
            if (--m_process_depth == 0)
            {
                ApplyPendingLocked();
                if (changed)
                    Resort();
                m_processing_thread = {};
            }
        }
    }

    void Resort()
    {
        if (!messages.empty()) {
            std::sort(std::begin(messages), std::end(messages),
                [](const auto& a, const auto& b) { return a.Prio > b.Prio; });
        }

        while (!messages.empty() && messages.back().Prio == REG_PRIORITY_INVALID)
            messages.pop_back();

        if (messages.empty())
            messages.shrink_to_fit();

        changed = false;
    }
};
