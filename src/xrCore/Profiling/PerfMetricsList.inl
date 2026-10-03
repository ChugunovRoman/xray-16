#if !defined(PERF_COUNTER) || !defined(PERF_TIMER)
#   error "PERF_COUNTER and PERF_TIMER must be defined before including PerfMetricsList.inl"
#endif

// TaskManager metrics
PERF_COUNTER(task_exec_own, "task/exec_own")
PERF_COUNTER(task_exec_worker0, "task/exec_worker0")
PERF_COUNTER(task_exec_steal, "task/exec_steal")
PERF_COUNTER(task_exec_miss, "task/exec_miss")
PERF_COUNTER(task_steal_attempt, "task/steal_attempt")
PERF_COUNTER(task_steal_success, "task/steal_success")
PERF_COUNTER(task_sleep_count, "task/sleep_count")
PERF_TIMER(task_sleep_time, "task/sleep_time")
PERF_COUNTER(task_wait_spin, "task/wait_spin")
PERF_COUNTER(task_wake_event, "task/wake_event")

// Spatial / common metrics placeholders
PERF_COUNTER(spatial_q_box, "spatial/q_box")
PERF_COUNTER(spatial_q_box_calls, "spatial/q_box/calls")
PERF_COUNTER(objects_crows, "objects/crows")
