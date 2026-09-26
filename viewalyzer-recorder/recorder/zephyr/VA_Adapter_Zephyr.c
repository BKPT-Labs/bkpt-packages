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
 * Recorder-owned tracing dispatch functions use viewalyzer_zephyr_* names
 * so they can coexist with Zephyr's own sys_trace_* definitions. Required
 * weak user callbacks retain their upstream names. The companion header
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
void viewalyzer_zephyr_thread_abort(struct k_thread *thread)
{
    if (!VA_IsInit())
        return;
    va_taskdeleted((void *)thread);
}

void sys_trace_thread_abort_user(struct k_thread *thread)
{
    viewalyzer_zephyr_thread_abort(thread);
}

/* k_thread_create() has no name parameter, so threads named afterwards via
   k_thread_name_set() would otherwise keep the placeholder "thread" name. */
void sys_trace_thread_name_set_user(struct k_thread *thread)
{
    if (!VA_IsInit())
        return;
    const char *name = k_thread_name_get((k_tid_t)thread);
    if (name == NULL || name[0] == '\0')
        return;
    if (_va_find_task_id((void *)thread) == 0)
        va_zephyr_register_thread((k_tid_t)thread);
    else
        va_taskrenamed((void *)thread, name);
}

void sys_trace_thread_switched_in_user(void)
{
    if (!VA_IsInit())
        return;
    k_tid_t cur = k_current_get();
    if (_va_find_task_id((void *)cur) == 0)
        va_zephyr_register_thread(cur);
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
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
/* ISR ids are one byte on the wire, so exception numbers above 255 alias. */
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
void viewalyzer_zephyr_mutex_init(struct k_mutex *mutex, int ret)
{
    if (!VA_IsInit() || ret != 0)
        return;
    va_zephyr_ensure_object_type((void *)mutex, VA_OBJECT_TYPE_MUTEX, "Mutex");
}

void viewalyzer_zephyr_mutex_lock_enter(struct k_mutex *mutex, k_timeout_t timeout)
{
    (void)mutex;
    (void)timeout;
}

/* Contention is sampled here, while the mutex is still held by someone
   else. */
void viewalyzer_zephyr_mutex_lock_blocking(struct k_mutex *mutex, k_timeout_t timeout)
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

void viewalyzer_zephyr_mutex_lock_exit(struct k_mutex *mutex, k_timeout_t timeout, int ret)
{
    ARG_UNUSED(mutex);
    ARG_UNUSED(timeout);
    ARG_UNUSED(ret);
#if VA_TRACE_MUTEXES
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)mutex, VA_OBJECT_TYPE_MUTEX, "Mutex");
    if (ret == 0)
        va_logQueueObjectTake((void *)mutex, va_zephyr_timeout_to_ms(timeout));
    else
        va_logObjectOpFailedTyped((void *)mutex, VA_OBJECT_TYPE_MUTEX, false, 0);
#endif
}

void viewalyzer_zephyr_mutex_unlock_enter(struct k_mutex *mutex)
{
    (void)mutex;
}

void viewalyzer_zephyr_mutex_unlock_exit(struct k_mutex *mutex, int ret)
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
void viewalyzer_zephyr_sem_init(struct k_sem *sem, int ret)
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

void viewalyzer_zephyr_sem_give_enter(struct k_sem *sem)
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

void viewalyzer_zephyr_sem_take_enter(struct k_sem *sem, k_timeout_t timeout)
{
    (void)sem;
    (void)timeout;
}

void viewalyzer_zephyr_sem_take_blocking(struct k_sem *sem, k_timeout_t timeout)
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

void viewalyzer_zephyr_sem_take_exit(struct k_sem *sem, k_timeout_t timeout, int ret)
{
    VA_QueueObjectType_t expected_type = (sem->limit <= 1)
        ? VA_OBJECT_TYPE_BINARY_SEM
        : VA_OBJECT_TYPE_COUNTING_SEM;
    const char *type_hint = (sem->limit <= 1) ? "BinSem" : "CountSem";

    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)sem, expected_type, type_hint);
    if (ret == 0)
        va_logQueueObjectTake((void *)sem, va_zephyr_timeout_to_ms(timeout));
    else
        va_logObjectOpFailedTyped((void *)sem, expected_type, false, 0);
}
#endif

/* ── Message queue tracing dispatch (called from sys_port_trace macros) ─── */

#if VA_HAS_QUEUE_DETAILS
void viewalyzer_zephyr_msgq_detail(struct k_msgq *msgq, uint8_t op, int ret);
void viewalyzer_zephyr_unbounded(void *queue, uint8_t op, uint32_t count);
#endif

#if VA_TRACE_QUEUES
void viewalyzer_zephyr_msgq_init(struct k_msgq *msgq)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_msgq_detail(msgq, VA_QUEUE_INIT, 0);
#endif
}

void viewalyzer_zephyr_msgq_put_enter(struct k_msgq *msgq, k_timeout_t timeout)
{
    (void)msgq;
    (void)timeout;
}

void viewalyzer_zephyr_msgq_put_blocking(struct k_msgq *msgq, k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    va_logQueueObjectBlocking((void *)msgq);
}

void viewalyzer_zephyr_msgq_put_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    if (ret == 0)
        va_logQueueObjectGive((void *)msgq, va_zephyr_timeout_to_ms(timeout));
    else
        va_logObjectOpFailedTyped((void *)msgq, VA_OBJECT_TYPE_QUEUE, true, 0);
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_msgq_detail(msgq, VA_QUEUE_BACK, ret);
#endif
}

void viewalyzer_zephyr_msgq_get_enter(struct k_msgq *msgq, k_timeout_t timeout)
{
    (void)msgq;
    (void)timeout;
}

void viewalyzer_zephyr_msgq_get_blocking(struct k_msgq *msgq, k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    va_logQueueObjectBlocking((void *)msgq);
}

void viewalyzer_zephyr_msgq_get_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)msgq, VA_OBJECT_TYPE_QUEUE, "Queue");
    if (ret == 0)
        va_logQueueObjectTake((void *)msgq, va_zephyr_timeout_to_ms(timeout));
    else
        va_logObjectOpFailedTyped((void *)msgq, VA_OBJECT_TYPE_QUEUE, false, 0);
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_msgq_detail(msgq, VA_QUEUE_RECEIVE, ret);
#endif
}
#endif

/* ── FIFO / LIFO tracing dispatch (called from sys_port_trace macros) ───
   Mapped onto the queue events: put = give, successful get = take. These
   have no fixed capacity; consumption order follows the object kind. */

#if VA_TRACE_QUEUES
void viewalyzer_zephyr_fifo_init(struct k_fifo *fifo)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)fifo, VA_OBJECT_TYPE_QUEUE, "FifoQueue");
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_unbounded(fifo, VA_QUEUE_INIT, 0);
#endif
}

void viewalyzer_zephyr_fifo_put(struct k_fifo *fifo)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)fifo, VA_OBJECT_TYPE_QUEUE, "FifoQueue");
    va_logQueueObjectGive((void *)fifo, 0);
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_unbounded(fifo, VA_QUEUE_BACK, 1);
#endif
}

void viewalyzer_zephyr_fifo_alloc_put(struct k_fifo *fifo, int ret)
{
    if (ret == 0)
        viewalyzer_zephyr_fifo_put(fifo);
}

void viewalyzer_zephyr_fifo_get(struct k_fifo *fifo, void *ret)
{
#if VA_HAS_QUEUE_DETAILS
    if (VA_IsInit() && ret == NULL)
        viewalyzer_zephyr_unbounded(fifo, VA_QUEUE_RECEIVE_FAILED, 0);
#endif
    if (!VA_IsInit() || ret == NULL)
        return;
    va_zephyr_ensure_object_type((void *)fifo, VA_OBJECT_TYPE_QUEUE, "FifoQueue");
    va_logQueueObjectTake((void *)fifo, 0);
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_unbounded(fifo, VA_QUEUE_RECEIVE, 1);
#endif
}

void viewalyzer_zephyr_lifo_init(struct k_lifo *lifo)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)lifo, VA_OBJECT_TYPE_QUEUE, "LifoQueue");
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_unbounded(lifo, VA_QUEUE_INIT, 0);
#endif
}

void viewalyzer_zephyr_lifo_put(struct k_lifo *lifo)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)lifo, VA_OBJECT_TYPE_QUEUE, "LifoQueue");
    va_logQueueObjectGive((void *)lifo, 0);
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_unbounded(lifo, VA_QUEUE_LIFO, 1);
#endif
}

void viewalyzer_zephyr_lifo_alloc_put(struct k_lifo *lifo, int ret)
{
    if (ret == 0)
        viewalyzer_zephyr_lifo_put(lifo);
}

void viewalyzer_zephyr_lifo_get(struct k_lifo *lifo, void *ret)
{
#if VA_HAS_QUEUE_DETAILS
    if (VA_IsInit() && ret == NULL)
        viewalyzer_zephyr_unbounded(lifo, VA_QUEUE_RECEIVE_FAILED, 0);
#endif
    if (!VA_IsInit() || ret == NULL)
        return;
    va_zephyr_ensure_object_type((void *)lifo, VA_OBJECT_TYPE_QUEUE, "LifoQueue");
    va_logQueueObjectTake((void *)lifo, 0);
#if VA_HAS_QUEUE_DETAILS
    viewalyzer_zephyr_unbounded(lifo, VA_QUEUE_RECEIVE, 1);
#endif
}
#endif /* VA_TRACE_QUEUES */

/* ── Event tracing dispatch (k_event) ────────────────────────────── */
/* post = give-shaped set (value = posted bits, taken at the enter point
   before the kernel merges them); a satisfied wait = take-shaped event
   (value = matched bits); a timed-out or unmatched wait = failed op
   (value = the bits waited for). k_event_clear posts 0 bits and is
   skipped. */

#if VA_TRACE_EVENT_FLAGS
static void va_zephyr_ensure_eventflag(struct k_event *event)
{
    if (_va_find_queue_object_id((void *)event) == 0)
        va_logQueueObjectCreateTyped((void *)event, NULL, VA_OBJECT_TYPE_EVENTFLAG);
}

void viewalyzer_zephyr_event_init(struct k_event *event)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_eventflag(event);
}

void viewalyzer_zephyr_event_post(struct k_event *event, uint32_t events)
{
    if (!VA_IsInit() || events == 0)
        return;
    va_zephyr_ensure_eventflag(event);
    va_logEventFlagSet((void *)event, events);
}

void viewalyzer_zephyr_event_wait_exit(struct k_event *event, uint32_t events, uint32_t ret)
{
    if (!VA_IsInit() || events == 0)
        return;
    va_zephyr_ensure_eventflag(event);
    if (ret != 0)
        va_logEventFlagWaitEnd((void *)event, ret);
    else
        va_logObjectOpFailedTyped((void *)event, VA_OBJECT_TYPE_EVENTFLAG, false, events);
}
#endif /* VA_TRACE_EVENT_FLAGS */

/* ── Deferred-work tracing dispatch (k_work family) ──────────────── */
/* Arm events fire only when the operation actually queued or scheduled
   the item (ret > 0; 0 = already pending, negative = rejected). The
   kernel has no trace point at delayed-work expiry or around handler
   execution, so the host derives the expected fire time from arm + delay
   and attributes execution to the workqueue thread. */

#if VA_TRACE_WORK
void viewalyzer_zephyr_work_submit(struct k_work *work, int ret)
{
    if (!VA_IsInit() || ret <= 0)
        return;
    va_logWorkArm((void *)work->handler, 0);
}

void viewalyzer_zephyr_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay, int ret)
{
    if (!VA_IsInit() || ret <= 0)
        return;
    va_logWorkArm((void *)dwork->work.handler, va_zephyr_timeout_to_ms(delay));
}

void viewalyzer_zephyr_work_cancel(struct k_work *work)
{
    if (!VA_IsInit())
        return;
    va_logWorkCancel((void *)work->handler);
}

void viewalyzer_zephyr_work_cancel_delayable(struct k_work_delayable *dwork)
{
    if (!VA_IsInit())
        return;
    va_logWorkCancel((void *)dwork->work.handler);
}
#endif /* VA_TRACE_WORK */

/* ── Sleep tracing dispatch (k_sleep / k_msleep / k_usleep) ──────── */

#if VA_TRACE_SLEEP || VA_TRACE_TASK_STATES
/* Suspend/resume ride the sleep events: the suspend-to-resume window shows
   as one sleep period on the suspended thread. The resume point fires
   before the kernel checks whether the thread was actually suspended, so a
   spurious resume emits an unmatched exit, which hosts ignore. */
void viewalyzer_zephyr_thread_suspend(struct k_thread *thread)
{
    if (!VA_IsInit())
        return;
    va_logTaskState(thread, VA_TASK_SUSPENDED);
    va_logSleepEnter((void *)thread);
}

void viewalyzer_zephyr_thread_resume(struct k_thread *thread)
{
    if (!VA_IsInit())
        return;
    va_logSleepExit((void *)thread);
}

void viewalyzer_zephyr_thread_sleep_enter(k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    if (!VA_IsInit())
        return;
    if (!K_TIMEOUT_EQ(timeout, K_NO_WAIT))
        va_logTaskState(k_current_get(), VA_TASK_SLEEPING);
    va_logSleepEnter((void *)k_current_get());
}

void viewalyzer_zephyr_thread_sleep_exit(k_timeout_t timeout, int ret)
{
    ARG_UNUSED(timeout);
    ARG_UNUSED(ret);
    if (!VA_IsInit())
        return;
    va_logTaskState(k_current_get(), VA_TASK_RUNNING);
    va_logSleepExit((void *)k_current_get());
}

void viewalyzer_zephyr_thread_msleep_enter(int32_t ms)
{
    ARG_UNUSED(ms);
    if (!VA_IsInit())
        return;
    if (ms > 0)
        va_logTaskState(k_current_get(), VA_TASK_SLEEPING);
    va_logSleepEnter((void *)k_current_get());
}

void viewalyzer_zephyr_thread_msleep_exit(int32_t ms, int ret)
{
    ARG_UNUSED(ms);
    ARG_UNUSED(ret);
    if (!VA_IsInit())
        return;
    va_logTaskState(k_current_get(), VA_TASK_RUNNING);
    va_logSleepExit((void *)k_current_get());
}

void viewalyzer_zephyr_thread_usleep_enter(int32_t us)
{
    ARG_UNUSED(us);
    if (!VA_IsInit())
        return;
    if (us > 0)
        va_logTaskState(k_current_get(), VA_TASK_SLEEPING);
    va_logSleepEnter((void *)k_current_get());
}

void viewalyzer_zephyr_thread_usleep_exit(int32_t us, int ret)
{
    ARG_UNUSED(us);
    ARG_UNUSED(ret);
    if (!VA_IsInit())
        return;
    va_logTaskState(k_current_get(), VA_TASK_RUNNING);
    va_logSleepExit((void *)k_current_get());
}
#endif

/* ── Timer tracing dispatch (k_timer init / start / stop) ────────── */

#if VA_TRACE_TIMERS
void viewalyzer_zephyr_timer_init(struct k_timer *timer)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)timer, VA_OBJECT_TYPE_TIMER, "Timer");
}

void viewalyzer_zephyr_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)timer, VA_OBJECT_TYPE_TIMER, "Timer");
    va_logQueueObjectGive((void *)timer, va_zephyr_timeout_to_ms(duration));
    /* K_NO_WAIT period maps to 0 = one-shot. */
    va_logTimerArm((void *)timer, va_zephyr_timeout_to_ms(duration),
                   va_zephyr_timeout_to_ms(period));
}

void viewalyzer_zephyr_timer_stop(struct k_timer *timer)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)timer, VA_OBJECT_TYPE_TIMER, "Timer");
    va_logQueueObjectTake((void *)timer, 0);
}

void viewalyzer_zephyr_timer_status_sync_blocking(struct k_timer *timer, k_timeout_t timeout)
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
    /* reported[] is shared between thread and ISR contexts (K_NO_WAIT
       allocs are ISR-legal), so the scan-and-claim must be atomic. */
    static void *reported[VA_MAX_SYNC_OBJECTS];
    int slot = -1;

    VA_CS_ENTER();
    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        if (reported[i] == (void *)heap)
            break;
        if (reported[i] == NULL)
        {
            reported[i] = (void *)heap;
            slot = i;
            break;
        }
    }
    VA_CS_EXIT();

    if (slot >= 0)
    {
        struct sys_memory_stats stats;
        if (sys_heap_runtime_stats_get(&heap->heap, &stats) != 0)
        {
            VA_ATOMIC(reported[slot] = NULL);   /* retry on the next event */
            return;
        }
        va_logHeapCapacity((void *)heap, "Heap",
                           (uint32_t)(stats.allocated_bytes + stats.free_bytes));
    }
#else
    ARG_UNUSED(heap);
#endif
}

void viewalyzer_zephyr_heap_init(struct k_heap *heap)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)heap, VA_OBJECT_TYPE_HEAP, "Heap");
    va_zephyr_report_heap_capacity_once(heap);
}

void viewalyzer_zephyr_heap_alloc_exit_impl(struct k_heap *heap, uint32_t alloc_bytes, void *ret)
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

void viewalyzer_zephyr_heap_free(struct k_heap *heap)
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

void viewalyzer_zephyr_heap_alloc_blocking(struct k_heap *heap)
{
    if (!VA_IsInit())
        return;
    va_zephyr_ensure_object_type((void *)heap, VA_OBJECT_TYPE_HEAP, "Heap");
    va_logQueueObjectBlocking((void *)heap);
}
#endif

/* ── PM tracing dispatch (pm_system_suspend enter / exit) ────────── */

#if VA_TRACE_PM
void viewalyzer_zephyr_pm_system_suspend_enter(uint32_t ticks)
{
    ARG_UNUSED(ticks);
    if (!VA_IsInit())
        return;
    va_logPMSuspendEnter();
}

void viewalyzer_zephyr_pm_system_suspend_exit(uint32_t ticks, uint8_t state)
{
    ARG_UNUSED(ticks);
    if (!VA_IsInit())
        return;
    va_logPMSuspendExit(state);
}
#endif

#if VA_TRACE_TASK_STATES
void viewalyzer_zephyr_sched_state(struct k_thread *thread, VA_TaskState_t state)
{
    if (!VA_IsInit()) return;
    va_zephyr_register_thread(thread);
    va_logTaskState(thread, state);
    if (state == VA_TASK_READY) va_clearTaskWait(thread);
}

void viewalyzer_zephyr_priority(struct k_thread *thread, int prio)
{
    if (!VA_IsInit()) return;
    va_zephyr_register_thread(thread);
    /* This hook covers both explicit changes and mutex inheritance. Zephyr
       does not expose a task-wide base priority or the cause at this site. */
    va_logTaskPriority(thread, prio, INT32_MIN, 0);
}

void viewalyzer_zephyr_wait(void *object, VA_QueueObjectType_t type,
                           VA_WaitReason_t reason, uint32_t detail)
{
    if (!VA_IsInit() || k_is_in_isr()) return;
    va_zephyr_register_thread(k_current_get());
    va_logTaskWait(k_current_get(), reason, object, type, detail);
}

void viewalyzer_zephyr_wait_end(void)
{
    if (!k_is_in_isr()) va_clearTaskWait(k_current_get());
}
#else
#define viewalyzer_zephyr_wait(o, t, r, d) ((void)0)
#define viewalyzer_zephyr_wait_end() ((void)0)
#endif

#if VA_TRACE_TIMERS && VA_TRACE_TIMER_CALLBACKS && defined(VA_ZEPHYR_HAS_TIMER_CALLBACKS)
void viewalyzer_zephyr_timer_callback(struct k_timer *timer, bool enter, bool stop)
{
    va_logTimerCallback(timer, enter, stop,
        stop ? (void *)timer->stop_fn : (void *)timer->expiry_fn);
}
#endif

#if VA_NEEDS_RTOS_OPERATIONS
static void va_zephyr_operation(void *object, VA_QueueObjectType_t type, uint8_t event,
                                VA_RtosOperation_t op, uint32_t value, uint32_t detail)
{
    if (!VA_IsInit()) return;
    uint16_t exception = (uint16_t)__get_IPSR();
    if (exception == 0) va_zephyr_register_thread(k_current_get());
    va_logRtosOperation(object, type, event, op, value, detail,
                        exception == 0 ? k_current_get() : NULL, exception);
}
#endif

#if VA_HAS_QUEUE_DETAILS
void viewalyzer_zephyr_msgq_detail(struct k_msgq *msgq, uint8_t op, int ret)
{
    if (!VA_IsInit()) return;
    if (ret != 0) op = op == VA_QUEUE_RECEIVE ? VA_QUEUE_RECEIVE_FAILED :
                       op == VA_QUEUE_PEEK ? VA_QUEUE_PEEK_FAILED : VA_QUEUE_SEND_FAILED;
    va_logRtosObjectInfo(msgq, VA_OBJECT_TYPE_QUEUE, msgq->max_msgs, msgq->msg_size);
    /* Purge hook is before the assignment, under the kernel's spinlock. */
    uint32_t used = op == VA_QUEUE_RESET ? 0 : msgq->used_msgs;
    va_zephyr_operation(msgq, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS,
        (VA_RtosOperation_t)op, used, 1);
}

void viewalyzer_zephyr_unbounded(void *queue, uint8_t op, uint32_t count)
{
    if (!VA_IsInit()) return;
    /* No O(n) queue traversal or guessed kernel layout. A failed get observes
       empty; other operations cannot safely establish occupancy after wakeup. */
    va_zephyr_operation(queue, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS,
        (VA_RtosOperation_t)op,
        (op == VA_QUEUE_INIT || op == VA_QUEUE_RECEIVE_FAILED) ? 0 : UINT32_MAX, count);
}

void viewalyzer_zephyr_fifo_batch(struct k_fifo *fifo, void *head, void *tail)
{
    if (!VA_IsInit()) return;
    /* The caller still owns the intrusive list at ENTER. Bound recorder work;
       UINT32_MAX means an uncounted remainder, never a fabricated one-item put. */
    uint32_t count = 0;
    void *node = head;
    while (node != NULL && count < 256) {
        ++count;
        if (node == tail) { node = NULL; break; }
        node = *(void **)node;
    }
    viewalyzer_zephyr_unbounded(fifo, VA_QUEUE_BATCH, node == NULL ? count : UINT32_MAX);
}
#endif

#if VA_HAS_EVENT_FLAG_DETAILS
void viewalyzer_zephyr_flags(struct k_event *event, uint8_t op, uint32_t bits, uint32_t mask)
{
    va_zephyr_operation(event, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
        (VA_RtosOperation_t)op, bits, mask);
}
#endif

#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
void viewalyzer_zephyr_slab(struct k_mem_slab *slab, uint8_t operation, int ret)
{
    if (!VA_IsInit()) return;
    if (operation == VA_OP_WAIT_BEGIN) {
        viewalyzer_zephyr_wait(slab, VA_OBJECT_TYPE_MEM_SLAB, VA_WAIT_MEM_SLAB, 0);
        return;
    }
    if (operation == VA_OP_ALLOC) viewalyzer_zephyr_wait_end();
#if VA_TRACE_MEM_SLABS
    va_logRtosObjectInfo(slab, VA_OBJECT_TYPE_MEM_SLAB,
                         slab->info.num_blocks, slab->info.block_size);
    if (operation != 0)
        va_zephyr_operation(slab, VA_OBJECT_TYPE_MEM_SLAB, VA_EVENT_MEM_SLAB,
            ret != 0 ? VA_OP_ALLOC_FAILED : (VA_RtosOperation_t)operation,
            slab->info.num_used, (uint32_t)ret);
#else
    ARG_UNUSED(ret);
#endif
}
#endif

#if VA_TRACE_CONDVARS || VA_TRACE_TASK_STATES
void viewalyzer_zephyr_condvar(struct k_condvar *condvar, uint8_t operation,
                              int ret, struct k_mutex *mutex, k_timeout_t timeout)
{
    if (!VA_IsInit()) return;
    if (operation == VA_OP_WAIT_BEGIN)
        viewalyzer_zephyr_wait(condvar, VA_OBJECT_TYPE_CONDVAR, VA_WAIT_CONDVAR,
                               (uint32_t)(uintptr_t)mutex);
    if (operation == VA_OP_WAIT_END) viewalyzer_zephyr_wait_end();
#if VA_TRACE_CONDVARS
    if (operation == 0)
        va_logQueueObjectCreateTyped(condvar, NULL, VA_OBJECT_TYPE_CONDVAR);
    else
        va_zephyr_operation(condvar, VA_OBJECT_TYPE_CONDVAR, VA_EVENT_CONDVAR,
            (VA_RtosOperation_t)operation,
            operation == VA_OP_WAIT_BEGIN ? (uint32_t)(uintptr_t)mutex : (uint32_t)ret,
            operation == VA_OP_WAIT_BEGIN ? va_zephyr_timeout_to_ms(timeout) : 0);
#else
    ARG_UNUSED(ret); ARG_UNUSED(mutex); ARG_UNUSED(timeout);
#endif
}
#endif

#if VA_TRACE_POLL || VA_TRACE_TASK_STATES
void viewalyzer_zephyr_poll(struct k_poll_event *events, int count, bool enter, int ret)
{
    if (!VA_IsInit()) return;
    if (enter)
        viewalyzer_zephyr_wait(NULL, VA_OBJECT_TYPE_POLL_SIGNAL, VA_WAIT_POLL,
                               (uint32_t)(uintptr_t)events);
    else
        viewalyzer_zephyr_wait_end();
#if VA_TRACE_POLL
    va_zephyr_operation(NULL, VA_OBJECT_TYPE_POLL_SIGNAL, VA_EVENT_POLL,
        enter ? VA_OP_WAIT_BEGIN : VA_OP_WAIT_END,
        (uint32_t)(uintptr_t)events, enter ? (uint32_t)count : (uint32_t)ret);
#else
    ARG_UNUSED(count); ARG_UNUSED(ret);
#endif
}

void viewalyzer_zephyr_poll_signal(struct k_poll_signal *sig, uint8_t operation, int ret)
{
#if VA_TRACE_POLL
    if (!VA_IsInit()) return;
    if (operation == 0)
        va_logQueueObjectCreateTyped(sig, NULL, VA_OBJECT_TYPE_POLL_SIGNAL);
    else
        va_zephyr_operation(sig, VA_OBJECT_TYPE_POLL_SIGNAL, VA_EVENT_POLL,
            (VA_RtosOperation_t)operation, (uint32_t)sig->result, (uint32_t)ret);
#else
    ARG_UNUSED(sig); ARG_UNUSED(operation); ARG_UNUSED(ret);
#endif
}
#endif

#endif /* VA_ENABLED && VA_RTOS_ZEPHYR */
