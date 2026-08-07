/**
 * @file VA_Adapter_Zephyr.c
 * @brief ViewAlyzer Zephyr Adapter - RTOS-specific logic
 *
 * Implements the adapter interface for Zephyr RTOS:
 *   - Queue-object type detection (Zephyr doesn't use FreeRTOS queue hacks)
 *   - Stack-usage calculation via k_thread_stack_space_get
 *   - Mutex contention (Zephyr k_mutex owner field)
 *
 * Also overrides Zephyr's CONFIG_TRACING_USER weak callbacks to emit
 * native ViewAlyzer task-switch events through the core engine.
 *
 * Mutex, semaphore, and message-queue tracing dispatch functions are
 * defined directly here (not in stock tracing_user.c) so that users
 * never need to modify Zephyr core files.  The companion header
 * tracing_user.h (shipped alongside this file) wires the
 * sys_port_trace macros to these functions.  Users just need:
 *   zephyr_include_directories(BEFORE <path-to-VA-zephyr-dir>)
 *
 * This file is compiled ONLY when VA_RTOS_SELECT == VA_RTOS_ZEPHYR.
 *
 * Copyright 2025-2026 BKPT, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ViewAlyzer.h"

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_ZEPHYR)

#include "VA_Internal.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <string.h>

/* Zephyr routes trace points to exactly one backend; without this one every
   hook below is dead code and captures come back empty. */
#if !defined(CONFIG_TRACING_USER)
#error "ViewAlyzer: add CONFIG_TRACING_USER=y to prj.conf (Zephyr sends its trace points to another backend without it, so captures stay empty)"
#endif

/* ── Timeout conversion helper - handles K_FOREVER safely ────────── */

static inline uint32_t va_zephyr_timeout_to_ms(k_timeout_t timeout)
{
    if (K_TIMEOUT_EQ(timeout, K_FOREVER))
        return 0xFFFFFFFFu;
    if (K_TIMEOUT_EQ(timeout, K_NO_WAIT))
        return 0;
    return (uint32_t)k_ticks_to_ms_floor64(timeout.ticks);
}

#if VA_NEEDS_OBJECT_REGISTRY
static void va_zephyr_ensure_object_type(void *object,
                     VA_QueueObjectType_t expected_type,
                     const char *type_hint)
{
    if (object == NULL)
        return;

    if (_va_find_queue_object_id(object) == 0)
    {
        va_logQueueObjectCreateWithType(object, type_hint);
        return;
    }

    if (_va_get_stored_queue_object_type(object) != expected_type)
        va_updateQueueObjectType(object, type_hint);
}

/* ── Adapter interface - queue object type ───────────────────────── */

VA_QueueObjectType_t va_adapter_get_queue_object_type(void *handle)
{
    /* Zephyr cannot classify a bare handle; the typeHint at registration
       carries the classification instead. */
    (void)handle;
    return VA_OBJECT_TYPE_QUEUE;
}
#endif /* VA_NEEDS_OBJECT_REGISTRY */

/* ── Adapter interface - stack usage ─────────────────────────────── */

#if VA_TRACE_STACK_USAGE
uint32_t va_adapter_calculate_stack_usage(void *taskHandle)
{
    size_t unused = 0;
    int ret = k_thread_stack_space_get((k_tid_t)taskHandle, &unused);
    if (ret == 0)
    {
        int idx = _va_find_task_index(taskHandle);
        if (idx >= 0 && taskMap[idx].ulStackDepth > 0)
        {
            /* ulStackDepth is in bytes for Zephyr (set during registration) */
            uint32_t used = taskMap[idx].ulStackDepth - (uint32_t)unused;
            return used;
        }
        return (uint32_t)unused;
    }
    return 0;
}

uint32_t va_adapter_get_total_stack_size(void *taskHandle)
{
    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0)
    {
        return taskMap[idx].ulStackDepth;
    }
    return 0;
}

#endif /* VA_TRACE_STACK_USAGE */

/* ── Adapter interface - mutex contention ────────────────────────── */

#if VA_NEEDS_BLOCKING_HOOK
void va_adapter_check_mutex_contention(void *queueObject, uint8_t queue_va_id)
{
    struct k_mutex *m = (struct k_mutex *)queueObject;
    k_tid_t holder = m->owner;
    if (holder != NULL)
    {
        k_tid_t waiter = k_current_get();
        uint8_t waiting_id = _va_find_task_id((void *)waiter);
        uint8_t holder_id  = _va_find_task_id((void *)holder);
        if (waiting_id != 0 && holder_id != 0 && waiting_id != holder_id)
        {
            _va_send_mutex_contention_packet(queue_va_id, waiting_id, holder_id, _va_get_timestamp());
        }
    }
}
#endif /* VA_NEEDS_BLOCKING_HOOK */

/* ── Thread → ViewAlyzer ID registration helper ──────────────────── */

#if VA_NEEDS_TASK_REGISTRY
static void va_zephyr_register_thread(k_tid_t tid)
{
    const char *name = k_thread_name_get(tid);
    if (name == NULL || name[0] == '\0')
        name = "thread";

    /* The whole check-fill-register sequence must be atomic: a context
       switch in between would clobber the creation globals through the
       lazy switch-in registration. */
    VA_CS_ENTER();
    if (_va_find_task_id((void *)tid) == 0)
    {
        g_task_pxStack       = NULL;
        g_task_pxEndOfStack  = NULL;
        g_task_uxPriority    = (uint32_t)k_thread_priority_get(tid);
        g_task_uxBasePriority = g_task_uxPriority;
#if defined(CONFIG_THREAD_STACK_INFO)
        g_task_ulStackDepth  = tid->stack_info.size;
#else
        g_task_ulStackDepth  = 0;
#endif
        va_taskcreated((void *)tid, name);
    }
    VA_CS_EXIT();
}

/* ── Public helper ──────────────────────────────────────────────── */

static void va_zephyr_foreach_cb(const struct k_thread *thread, void *user_data)
{
    ARG_UNUSED(user_data);
    va_zephyr_register_thread((k_tid_t)thread);
}

void VA_Zephyr_RegisterExistingThreads(void)
{
    k_thread_foreach(va_zephyr_foreach_cb, NULL);
}

/* ── Zephyr tracing weak-function overrides ──────────────────────── */

/* Registration is deliberately NOT under VA_TRACE_TASKS: other packets
   still need thread ids/names. Switch-IN doubles as lazy registration. */
void sys_trace_thread_create_user(struct k_thread *thread)
{
    if (!VA_IsInit())
        return;
    va_zephyr_register_thread((k_tid_t)thread);
}

/* Frees the registry slot so a new thread at a recycled address cannot
   inherit the dead thread's identity. Reached via the
   sys_port_trace_k_thread_abort_enter re-point in tracing_user.h; the weak
   sys_trace_thread_abort_user below covers kernels that still route abort
   through sys_trace_thread_abort (release is idempotent). */
void sys_trace_va_thread_abort(struct k_thread *thread)
{
    if (!VA_IsInit())
        return;
    va_taskdeleted((void *)thread);
}

void sys_trace_thread_abort_user(struct k_thread *thread)
{
    sys_trace_va_thread_abort(thread);
}

void sys_trace_thread_switched_in_user(void)
{
    if (!VA_IsInit())
        return;
    k_tid_t cur = k_current_get();
    if (_va_find_task_id((void *)cur) == 0)
        va_zephyr_register_thread(cur);
#if VA_TRACE_TASKS
    va_taskswitchedin((void *)cur);
#endif
}

void sys_trace_thread_switched_out_user(void)
{
#if VA_NEEDS_SWITCH_HOOK
    if (!VA_IsInit())
        return;
    k_tid_t cur = k_current_get();
    va_taskswitchedout((void *)cur);
#endif
}
#endif /* VA_NEEDS_TASK_REGISTRY */

#if VA_TRACE_ISRS
void sys_trace_isr_enter_user(void)
{
    if (!VA_IsInit())
        return;
    uint32_t exception = __get_IPSR() & 0xFFu;
    VA_LogISRStart((uint8_t)exception);
}

void sys_trace_isr_exit_user(void)
{
    if (!VA_IsInit())
        return;
    uint32_t exception = __get_IPSR() & 0xFFu;
    VA_LogISREnd((uint8_t)exception);
}
#endif

#if VA_TRANSPORT_IS_RAMBUF && defined(VA_RAMBUF_BUSY_IDLE) && (VA_RAMBUF_BUSY_IDLE == 1) \
    && defined(CONFIG_CPU_CORTEX_M)
/* Keep the core out of WFI while the RAM buffer transport is active: some
   probe/MCU combos return garbage for debug reads during sleep. The spin
   must release the kernel irq lock each iteration or ticks and
   rescheduling stop. */
void sys_trace_idle_user(void)
{
    /* Also reached via k_cpu_atomic_idle() from ordinary threads (driver
       wait loops) that expect this path to RETURN after an interrupt; only
       the true idle thread may spin. Applications are bounded at
       K_LOWEST_APPLICATION_THREAD_PRIO, so K_IDLE_PRIO is idle-only. */
    if (k_thread_priority_get(k_current_get()) != K_IDLE_PRIO)
        return;

    while (1)
    {
#if defined(CONFIG_CPU_CORTEX_M_HAS_BASEPRI)
        __set_BASEPRI(0);
#endif
        __enable_irq();
        __asm volatile("" ::: "memory");
    }
}
#endif

/* ── Mutex tracing dispatch (called from sys_port_trace macros) ──── */

#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION
void sys_trace_k_mutex_init(struct k_mutex *mutex, int ret)
{
    if (!VA_IsInit() || ret != 0)
        return;
    va_zephyr_ensure_object_type((void *)mutex, VA_OBJECT_TYPE_MUTEX, "Mutex");
}

void sys_trace_k_mutex_lock_enter(struct k_mutex *mutex, k_timeout_t timeout)
{
    (void)mutex;
    (void)timeout;
}

/* Contention is sampled here, while the mutex is still held by someone
   else. */
void sys_trace_k_mutex_lock_blocking(struct k_mutex *mutex, k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    ARG_UNUSED(mutex);
#if VA_TRACE_MUTEX_CONTENTION
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)mutex, VA_OBJECT_TYPE_MUTEX, "Mutex");
    va_logQueueObjectBlocking((void *)mutex);
#endif
}

void sys_trace_k_mutex_lock_exit(struct k_mutex *mutex, k_timeout_t timeout, int ret)
{
    ARG_UNUSED(mutex);
    ARG_UNUSED(timeout);
    ARG_UNUSED(ret);
#if VA_TRACE_MUTEXES
    if (!VA_IsInit() || ret != 0)
        return;
    va_zephyr_ensure_object_type((void *)mutex, VA_OBJECT_TYPE_MUTEX, "Mutex");
    va_logQueueObjectTake((void *)mutex, va_zephyr_timeout_to_ms(timeout));
#endif
}

void sys_trace_k_mutex_unlock_enter(struct k_mutex *mutex)
{
    (void)mutex;
}

void sys_trace_k_mutex_unlock_exit(struct k_mutex *mutex, int ret)
{
    ARG_UNUSED(mutex);
    ARG_UNUSED(ret);
#if VA_TRACE_MUTEXES
    if (!VA_IsInit() || ret != 0)
        return;
    va_zephyr_ensure_object_type((void *)mutex, VA_OBJECT_TYPE_MUTEX, "Mutex");
    va_logQueueObjectGive((void *)mutex, 0);
#endif
}
#endif /* VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION */

/* ── Semaphore tracing dispatch (called from sys_port_trace macros) ─── */

#if VA_TRACE_SEMAPHORES
void sys_trace_k_sem_init(struct k_sem *sem, int ret)
{
    VA_QueueObjectType_t expected_type;
    const char *type_hint;

    if (!VA_IsInit() || ret != 0)
        return;
    if (sem->limit <= 1)
    {
        expected_type = VA_OBJECT_TYPE_BINARY_SEM;
        type_hint = "BinSem";
    }
    else
    {
        expected_type = VA_OBJECT_TYPE_COUNTING_SEM;
        type_hint = "CountSem";
    }

    va_zephyr_ensure_object_type((void *)sem, expected_type, type_hint);
}

void sys_trace_k_sem_give_enter(struct k_sem *sem)
{
    VA_QueueObjectType_t expected_type = (sem->limit <= 1)
        ? VA_OBJECT_TYPE_BINARY_SEM
        : VA_OBJECT_TYPE_COUNTING_SEM;
    const char *type_hint = (sem->limit <= 1) ? "BinSem" : "CountSem";

    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)sem, expected_type, type_hint);
    va_logQueueObjectGive((void *)sem, 0);
}

void sys_trace_k_sem_take_enter(struct k_sem *sem, k_timeout_t timeout)
{
    (void)sem;
    (void)timeout;
}

void sys_trace_k_sem_take_blocking(struct k_sem *sem, k_timeout_t timeout)
{
    VA_QueueObjectType_t expected_type = (sem->limit <= 1)
        ? VA_OBJECT_TYPE_BINARY_SEM
        : VA_OBJECT_TYPE_COUNTING_SEM;
    const char *type_hint = (sem->limit <= 1) ? "BinSem" : "CountSem";

    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)sem, expected_type, type_hint);
    va_logQueueObjectBlocking((void *)sem);
}

void sys_trace_k_sem_take_exit(struct k_sem *sem, k_timeout_t timeout, int ret)
{
    VA_QueueObjectType_t expected_type = (sem->limit <= 1)
        ? VA_OBJECT_TYPE_BINARY_SEM
        : VA_OBJECT_TYPE_COUNTING_SEM;
    const char *type_hint = (sem->limit <= 1) ? "BinSem" : "CountSem";

    if (!VA_IsInit() || ret != 0)
        return;
    va_zephyr_ensure_object_type((void *)sem, expected_type, type_hint);
    va_logQueueObjectTake((void *)sem, va_zephyr_timeout_to_ms(timeout));
}
#endif

/* ── Message queue tracing dispatch (called from sys_port_trace macros) ─── */

#if VA_TRACE_QUEUES
void sys_trace_k_msgq_init(struct k_msgq *msgq)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
}

void sys_trace_k_msgq_put_enter(struct k_msgq *msgq, k_timeout_t timeout)
{
    (void)msgq;
    (void)timeout;
}

void sys_trace_k_msgq_put_blocking(struct k_msgq *msgq, k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    va_logQueueObjectBlocking((void *)msgq);
}

void sys_trace_k_msgq_put_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret)
{
    if (!VA_IsInit() || ret != 0)
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    va_logQueueObjectGive((void *)msgq, va_zephyr_timeout_to_ms(timeout));
}

void sys_trace_k_msgq_get_enter(struct k_msgq *msgq, k_timeout_t timeout)
{
    (void)msgq;
    (void)timeout;
}

void sys_trace_k_msgq_get_blocking(struct k_msgq *msgq, k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    va_logQueueObjectBlocking((void *)msgq);
}

void sys_trace_k_msgq_get_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret)
{
    if (!VA_IsInit() || ret != 0)
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    va_logQueueObjectTake((void *)msgq, va_zephyr_timeout_to_ms(timeout));
}
#endif

/* ── Sleep tracing dispatch (k_sleep / k_msleep / k_usleep) ──────── */

#if VA_TRACE_SLEEP
void sys_trace_k_thread_sleep_enter(k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    va_logSleepEnter((void *)k_current_get());
}

void sys_trace_k_thread_sleep_exit(k_timeout_t timeout, int ret)
{
    ARG_UNUSED(timeout);
    ARG_UNUSED(ret);
    if (!VA_IsInit())
        return;
    va_logSleepExit((void *)k_current_get());
}

void sys_trace_k_thread_msleep_enter(int32_t ms)
{
    ARG_UNUSED(ms);
    if (!VA_IsInit())
        return;
    va_logSleepEnter((void *)k_current_get());
}

void sys_trace_k_thread_msleep_exit(int32_t ms, int ret)
{
    ARG_UNUSED(ms);
    ARG_UNUSED(ret);
    if (!VA_IsInit())
        return;
    va_logSleepExit((void *)k_current_get());
}

void sys_trace_k_thread_usleep_enter(int32_t us)
{
    ARG_UNUSED(us);
    if (!VA_IsInit())
        return;
    va_logSleepEnter((void *)k_current_get());
}

void sys_trace_k_thread_usleep_exit(int32_t us, int ret)
{
    ARG_UNUSED(us);
    ARG_UNUSED(ret);
    if (!VA_IsInit())
        return;
    va_logSleepExit((void *)k_current_get());
}
#endif

/* ── Timer tracing dispatch (k_timer init / start / stop) ────────── */

#if VA_TRACE_TIMERS
void sys_trace_k_timer_init(struct k_timer *timer)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)timer, VA_OBJECT_TYPE_TIMER, "Timer");
}

void sys_trace_k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period)
{
    ARG_UNUSED(duration);
    ARG_UNUSED(period);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)timer, VA_OBJECT_TYPE_TIMER, "Timer");
    va_logQueueObjectGive((void *)timer, va_zephyr_timeout_to_ms(duration));
}

void sys_trace_k_timer_stop(struct k_timer *timer)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)timer, VA_OBJECT_TYPE_TIMER, "Timer");
    va_logQueueObjectTake((void *)timer, 0);
}

void sys_trace_k_timer_status_sync_blocking(struct k_timer *timer, k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)timer, VA_OBJECT_TYPE_TIMER, "Timer");
    va_logQueueObjectBlocking((void *)timer);
}
#endif

/* ── Heap tracing dispatch (k_heap init / alloc / free) ──────────── */

#if VA_TRACE_RTOS_HEAPS
/* Report each heap's usable capacity once (anchors the host's 100% line). */
static void va_zephyr_report_heap_capacity_once(struct k_heap *heap)
{
#if defined(CONFIG_VIEWALYZER_HEAP_RUNTIME_STATS)
    static void *reported[VA_MAX_SYNC_OBJECTS];
    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        if (reported[i] == (void *)heap)
            return;
        if (reported[i] == NULL)
        {
            struct sys_memory_stats stats;
            if (sys_heap_runtime_stats_get(&heap->heap, &stats) != 0)
                return;                     /* retry on the next event */
            reported[i] = (void *)heap;
            va_logHeapCapacity((void *)heap, "Heap",
                               (uint32_t)(stats.allocated_bytes + stats.free_bytes));
            return;
        }
    }
#else
    ARG_UNUSED(heap);
#endif
}

void sys_trace_k_heap_init(struct k_heap *heap)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)heap, VA_OBJECT_TYPE_HEAP, "Heap");
    va_zephyr_report_heap_capacity_once(heap);
}

void sys_trace_k_heap_alloc_exit_impl(struct k_heap *heap, uint32_t alloc_bytes, void *ret)
{
    ARG_UNUSED(alloc_bytes);
    ARG_UNUSED(ret);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)heap, VA_OBJECT_TYPE_HEAP, "Heap");
    va_zephyr_report_heap_capacity_once(heap);
    if (ret == NULL)
    {
        /* Report failed allocations. */
        va_logHeapAllocFailed((void *)heap, alloc_bytes);
        return;
    }

#if defined(CONFIG_VIEWALYZER_HEAP_RUNTIME_STATS)
    struct sys_memory_stats stats;
    if (sys_heap_runtime_stats_get(&heap->heap, &stats) == 0)
        va_logHeapAlloc((void *)heap, (uint32_t)stats.allocated_bytes);
    else
        va_logHeapAlloc((void *)heap, alloc_bytes);
#else
    va_logHeapAlloc((void *)heap, alloc_bytes);
#endif
}

void sys_trace_k_heap_free(struct k_heap *heap)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)heap, VA_OBJECT_TYPE_HEAP, "Heap");
    va_zephyr_report_heap_capacity_once(heap);

#if defined(CONFIG_VIEWALYZER_HEAP_RUNTIME_STATS)
    struct sys_memory_stats stats;
    if (sys_heap_runtime_stats_get(&heap->heap, &stats) == 0)
        va_logHeapFree((void *)heap, (uint32_t)stats.allocated_bytes);
    else
        va_logHeapFree((void *)heap, 0);
#else
    va_logHeapFree((void *)heap, 0);
#endif
}

void sys_trace_k_heap_alloc_blocking(struct k_heap *heap)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)heap, VA_OBJECT_TYPE_HEAP, "Heap");
    va_logQueueObjectBlocking((void *)heap);
}
#endif

/* ── PM tracing dispatch (pm_system_suspend enter / exit) ────────── */

#if VA_TRACE_PM
void sys_trace_va_pm_system_suspend_enter(uint32_t ticks)
{
    ARG_UNUSED(ticks);
    if (!VA_IsInit())
        return;
    va_logPMSuspendEnter();
}

void sys_trace_va_pm_system_suspend_exit(uint32_t ticks, uint8_t state)
{
    ARG_UNUSED(ticks);
    if (!VA_IsInit())
        return;
    va_logPMSuspendExit(state);
}
#endif

#endif /* VA_ENABLED && VA_RTOS_ZEPHYR */
