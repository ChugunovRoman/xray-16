#pragma once

#include "xrCore/Threading/TaskManager.hpp"

/** Per-frame task batch mirroring ixray `xr_task_group::run` / `wait` using OpenXRay `TaskManager`. */
class ENGINE_API xr_frame_task_group final
{
    static constexpr size_t kMaxTasks = 4;
    const Task* m_tasks[kMaxTasks]{};
    size_t m_count{};

public:
    template <typename Invokable>
    void run(Invokable&& func)
    {
        VERIFY3(m_count < kMaxTasks, "xr_frame_task_group: run() overflow (missing wait()?)", m_count);
        VERIFY(TaskScheduler);
        m_tasks[m_count++] = &TaskScheduler->AddTask(std::forward<Invokable>(func));
    }

    void wait()
    {
        if (!TaskScheduler)
            return;
        for (size_t i = 0; i < m_count; ++i)
            TaskScheduler->Wait(*m_tasks[i]);
        m_count = 0;
    }
};
