/*
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
#define VA_THREADX_IMPLEMENTATION
#include "ViewAlyzer.h"
#if VA_ENABLED && (VA_RTOS_SELECT == VA_RTOS_THREADX)
#include "VA_Internal.h"
#include "tx_api.h"
#include "tx_thread.h"
#include "tx_trace.h"
#include "tx_queue.h"
#include "tx_mutex.h"
#include "tx_semaphore.h"
#include "tx_event_flags.h"
#include "tx_timer.h"
#include "tx_byte_pool.h"
#include "tx_block_pool.h"
#include "VA_Adapter_ThreadX.h"
extern volatile const unsigned char va_threadx_core_config;

static void va_tx_operation(void *object, VA_QueueObjectType_t type, uint8_t event,
                            uint8_t operation, uint32_t value, uint32_t detail,
                            void *task, uint16_t exception)
{
#if VA_NEEDS_RTOS_OPERATIONS
    if ((event == VA_EVENT_QUEUE_DETAILS && !VA_HAS_QUEUE_DETAILS) ||
        (event == VA_EVENT_FLAG_DETAILS && !VA_HAS_EVENT_FLAG_DETAILS) ||
        (event == VA_EVENT_MEM_SLAB && !VA_TRACE_MEM_SLABS)) return;
    va_logRtosOperation(object, type, event, (VA_RtosOperation_t)operation, value, detail, task, exception);
#else
    (void)object; (void)type; (void)event; (void)operation;
    (void)value; (void)detail; (void)task; (void)exception;
#endif
}

#if THREADX_MAJOR_VERSION != 6 || !((THREADX_MINOR_VERSION == 4 && THREADX_PATCH_VERSION == 0) || (THREADX_MINOR_VERSION == 5 && THREADX_PATCH_VERSION == 2))
#error "ViewAlyzer supports the ST ThreadX 6.4.0 and Eclipse ThreadX 6.5.2 baselines only"
#endif
#ifdef TX_THREAD_SMP_MAX_CORES
#error "ViewAlyzer ThreadX adapter currently supports single-core ports only"
#endif

static TX_THREAD *va_tx_current(void)
{
    return (__get_IPSR() == 0 && _tx_thread_system_state == 0) ? _tx_thread_current_ptr : TX_NULL;
}

static void va_tx_native_sync(unsigned int event, void *object, uintptr_t a, uintptr_t b, uintptr_t c);
static TX_THREAD *va_tx_running;
static unsigned char va_tx_idle;
static int va_tx_idle_active;
#if VA_TRACE_SLEEP
static unsigned char va_tx_sleeping[(VA_MAX_TASKS + 7u) / 8u];
#endif
/* A GET cannot invoke an application callback, so one in-flight result per
 * task/exception is sufficient even when another thread or ISR preempts it. */
#if VA_TRACE_MUTEXES || VA_TRACE_SEMAPHORES
/* Registry slots, one system-context slot, then exception contexts. These
 * are internal indices; a sixteen-bit wire ID must never index this bitmap. */
#define VA_TX_TAKE_CONTEXTS (VA_MAX_TASKS + 257u)
static unsigned char va_tx_native_take[(VA_TX_TAKE_CONTEXTS + 7u) / 8u];

static unsigned va_tx_take_slot(void)
{
    uint32_t exception = __get_IPSR();
    if (exception) return exception <= 255 ? VA_MAX_TASKS + 1u + exception : VA_TX_TAKE_CONTEXTS;
    TX_THREAD *thread = va_tx_current();
    int slot = _va_find_task_index(thread);
    return thread ? (slot >= 0 ? (unsigned)slot : VA_TX_TAKE_CONTEXTS) : VA_MAX_TASKS;
}

static int va_tx_take_recorded(int update)
{
    unsigned slot = va_tx_take_slot(), mask = 1u << (slot & 7u);
    if (slot >= VA_TX_TAKE_CONTEXTS) return 0;
    int recorded = (va_tx_native_take[slot >> 3] & mask) != 0;
    if (update > 0) va_tx_native_take[slot >> 3] |= (unsigned char)mask;
    else if (update == 0) va_tx_native_take[slot >> 3] &= (unsigned char)~mask;
    return recorded;
}
#else
static int va_tx_take_recorded(int update) { (void)update; return 0; }
#endif

static void va_tx_sleep(TX_THREAD *thread, int entering)
{
#if VA_TRACE_SLEEP
    int index = _va_find_task_index(thread);
    if (index < 0) return;
    unsigned slot = (unsigned)index, mask = 1u << (slot & 7u);
    if (entering) {
        va_tx_sleeping[slot >> 3] |= (unsigned char)mask;
        va_logSleepEnter(thread);
    } else if (va_tx_sleeping[slot >> 3] & mask) {
        va_tx_sleeping[slot >> 3] &= (unsigned char)~mask;
        va_logSleepExit(thread);
    }
#else
    (void)thread; (void)entering;
#endif
}

static void va_tx_clear_thread_tracking(TX_THREAD *thread)
{
    va_tx_sleep(thread, 0);
#if VA_NEEDS_OBJECT_REGISTRY
    _va_clear_object_reference(thread);
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_SEMAPHORES
    int slot = _va_find_task_index(thread);
    if (slot >= 0) va_tx_native_take[(unsigned)slot >> 3] &= (unsigned char)~(1u << ((unsigned)slot & 7u));
#endif
}

static void va_tx_register_idle(void)
{
#if VA_NEEDS_TASK_REGISTRY
    VA_CS_ENTER();
    if (_va_find_task_id(&va_tx_idle) == 0) {
        g_task_pxStack = TX_NULL;
        g_task_pxEndOfStack = TX_NULL;
        g_task_ulStackDepth = 0;
        g_task_uxPriority = TX_MAX_PRIORITIES;
        g_task_uxBasePriority = TX_MAX_PRIORITIES;
        va_taskcreated(&va_tx_idle, "Idle");
    }
    VA_CS_EXIT();
#endif
}

/* VA_Init references this directly, so section GC/LTO cannot remove the
 * dependency even when a hookless kernel never calls an execution hook. */
void va_threadx_validate_kernel(void)
{
    (void)va_threadx_core_config;
    TX_TRACE_BUFFER_ENTRY *volatile *trace_configuration = &_tx_trace_buffer_current_ptr;
    (void)*trace_configuration;
}

void _tx_execution_initialize(void)
{
    va_tx_running = TX_NULL;
    va_tx_idle_active = 0;
#if VA_TRACE_SLEEP
    memset(va_tx_sleeping, 0, sizeof(va_tx_sleeping));
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_SEMAPHORES
    memset(va_tx_native_take, 0, sizeof(va_tx_native_take));
#endif
    if (VA_IsInit()) va_tx_register_idle();
}

static void va_tx_register_thread(TX_THREAD *thread)
{
#if VA_NEEDS_TASK_REGISTRY
    if (thread == TX_NULL || thread->tx_thread_id != TX_THREAD_ID || !VA_IsInit()) return;
    VA_CS_ENTER();
    if (_va_find_task_id(thread) == 0) {
        g_task_pxStack = thread->tx_thread_stack_start;
        g_task_pxEndOfStack = thread->tx_thread_stack_end;
        g_task_ulStackDepth = (uint32_t)thread->tx_thread_stack_size; /* bytes */
        g_task_uxPriority = thread->tx_thread_priority;
        g_task_uxBasePriority = thread->tx_thread_user_priority;
        va_taskcreated(thread, thread->tx_thread_name);
        /* A new registry ID can follow deletion or a recorder re-init. Never
         * inherit a different task's pending sleep or API completion bit. */
        int slot = _va_find_task_index(thread);
        if (slot >= 0) {
#if VA_TRACE_SLEEP
            va_tx_sleeping[(unsigned)slot >> 3] &= (unsigned char)~(1u << ((unsigned)slot & 7u));
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_SEMAPHORES
            va_tx_native_take[(unsigned)slot >> 3] &= (unsigned char)~(1u << ((unsigned)slot & 7u));
#endif
        }
    }
    VA_CS_EXIT();
#else
    (void)thread;
#endif
}

static void va_tx_priority_values(TX_THREAD *thread, UINT effective, UINT base, unsigned cause)
{
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
    if (!VA_IsInit() || thread == TX_NULL || thread->tx_thread_id != TX_THREAD_ID) return;
    VA_CS_ENTER();
    va_tx_register_thread(thread);
    int index = _va_find_task_index(thread);
    if (index >= 0 && (taskMap[index].uxPriority != effective || taskMap[index].uxBasePriority != base)) {
        if (!cause) cause = taskMap[index].uxBasePriority != base ? 1 : (effective < base ? 2 : 3);
        va_logTaskPriority(thread, (int32_t)effective, (int32_t)base, cause);
    }
    VA_CS_EXIT();
#else
    (void)thread; (void)effective; (void)base; (void)cause;
#endif
}

void va_threadx_priority_changed(void *handle)
{
    TX_THREAD *thread = (TX_THREAD *)handle;
    if (thread != TX_NULL && thread->tx_thread_id == TX_THREAD_ID)
        va_tx_priority_values(thread, thread->tx_thread_priority, thread->tx_thread_user_priority, 0);
}

void va_threadx_native_event(unsigned int event, void *object, uintptr_t detail, uintptr_t b, uintptr_t c)
{
    TX_THREAD *thread = (TX_THREAD *)object;
    if (!VA_IsInit()) return;
    switch (event) {
    case TX_TRACE_THREAD_PRIORITY_CHANGE: {
        /* Validated native call cannot fail and may preempt before returning.
         * Publish the committed base/effective priorities before that switch. */
        UINT requested = (UINT)detail;
        UINT effective = requested < thread->tx_thread_inherit_priority ? requested : thread->tx_thread_inherit_priority;
        va_tx_priority_values(thread, effective, requested, 1);
        break;
    }
    case TX_TRACE_THREAD_CREATE:
        va_tx_register_thread(thread);
        va_logTaskState(thread, VA_TASK_SUSPENDED);
        break;
    case TX_TRACE_THREAD_DELETE:
        va_tx_clear_thread_tracking(thread);
        va_taskdeleted(thread);
        break;
    case TX_TRACE_THREAD_SUSPEND:
        if (detail == TX_PRIORITY_CHANGE) break;
        va_tx_register_thread(thread);
        if (detail == TX_SLEEP) {
            va_logTaskState(thread, VA_TASK_SLEEPING);
            va_tx_sleep(thread, 1);
        } else if (detail == TX_COMPLETED || detail == TX_TERMINATED) {
            va_logTaskState(thread, VA_TASK_SUSPENDED);
        } else if (detail == TX_SUSPENDED) {
            va_logTaskState(thread, VA_TASK_SUSPENDED);
        } else {
            va_logTaskState(thread, VA_TASK_BLOCKED);
        }
        break;
    case TX_TRACE_THREAD_RESUME:
        if (detail == TX_PRIORITY_CHANGE) break;
        va_tx_register_thread(thread);
        /* A wake can cancel suspension before the suspend hook ran. That
         * thread is still running and has no recorded sleep to close. */
        if (thread->tx_thread_suspending) {
            if (thread->tx_thread_delayed_suspend)
                va_logTaskState(thread, VA_TASK_SUSPENDED);
            break;
        }
        if (detail == TX_READY || detail == TX_COMPLETED || detail == TX_TERMINATED) break;
        va_tx_sleep(thread, 0);
        va_logTaskState(thread, thread->tx_thread_delayed_suspend ? VA_TASK_SUSPENDED : VA_TASK_READY);
        break;
    default:
        va_tx_native_sync(event, object, detail, b, c);
        break;
    }
}

void _tx_execution_thread_enter(void)
{
    if (!VA_IsInit()) return;
#if VA_NEEDS_TASK_REGISTRY
    if (va_tx_running != TX_NULL && _va_find_task_id(va_tx_running) == 0) va_tx_running = TX_NULL;
    if (va_tx_idle_active && _va_find_task_id(&va_tx_idle) == 0) va_tx_idle_active = 0;
#endif
    TX_THREAD *thread = _tx_thread_current_ptr;
    va_tx_register_thread(thread);
    if (thread == va_tx_running) return;
    /* The execute pointer can change after the exit callback checked it. */
    if (va_tx_running != TX_NULL) {
#if VA_NEEDS_TASK_REGISTRY
        if (_va_find_task_id(va_tx_running) != 0)
#endif
            va_taskswitchedout(va_tx_running);
    }
    if (va_tx_idle_active) {
#if VA_NEEDS_TASK_REGISTRY
        if (_va_find_task_id(&va_tx_idle) != 0)
#endif
            va_taskswitchedout(&va_tx_idle);
        va_tx_idle_active = 0;
    }
    if (thread != TX_NULL) {
        va_threadx_priority_changed(thread);
        va_taskswitchedin(thread);
        va_tx_running = thread;
    }
}

void _tx_execution_thread_exit(void)
{
    if (!VA_IsInit() || _tx_thread_current_ptr == _tx_thread_execute_ptr) return;
    if (va_tx_running != TX_NULL) {
#if VA_NEEDS_TASK_REGISTRY
        if (_va_find_task_id(va_tx_running) != 0)
#endif
            va_taskswitchedout(va_tx_running);
    }
    va_tx_running = TX_NULL;
    if (_tx_thread_execute_ptr == TX_NULL && !va_tx_idle_active) {
        va_tx_register_idle();
        va_taskswitchedin(&va_tx_idle);
        va_tx_idle_active = 1;
    }
}

/* Cortex-M exception numbers fit the wire ISR ID for STM32 devices with
 * fewer than 240 external IRQs. Do not silently alias larger IRQ numbers. */
void VA_ThreadX_ISR_Enter(void)
{
    uint32_t exception = __get_IPSR();
    if (exception == 2 || exception == 3) return; /* PRIMASK cannot serialize NMI/HardFault. */
    if (exception == 15) exception = VA_ISR_ID_SYSTICK;
    else if (exception == 14) exception = VA_ISR_ID_PENDSV;
    if (exception != 0 && exception <= 255) VA_LogISRStart((uint8_t)exception);
}
void VA_ThreadX_ISR_Exit(void)
{
    uint32_t exception = __get_IPSR();
    if (exception == 2 || exception == 3) return;
    if (exception == 15) exception = VA_ISR_ID_SYSTICK;
    else if (exception == 14) exception = VA_ISR_ID_PENDSV;
    if (exception != 0 && exception <= 255) VA_LogISREnd((uint8_t)exception);
}
void _tx_execution_isr_enter(void) { VA_ThreadX_ISR_Enter(); }
void _tx_execution_isr_exit(void) { VA_ThreadX_ISR_Exit(); }

#if VA_TRACE_STACK_USAGE
uint32_t va_adapter_get_total_stack_size(void *handle)
{
#if defined(TX_DISABLE_STACK_FILLING) || (THREADX_MINOR_VERSION == 4 && \
    defined(TX_ENABLE_RANDOM_NUMBER_STACK_FILLING) && defined(TX_ENABLE_STACK_CHECKING))
    (void)handle;
    return 0;
#else
    if (handle == &va_tx_idle) return 0;
    TX_THREAD *thread = (TX_THREAD *)handle;
    return thread != TX_NULL ? (uint32_t)thread->tx_thread_stack_size : 0;
#endif
}
uint32_t va_adapter_calculate_stack_usage(void *handle)
{
    TX_THREAD *thread = (TX_THREAD *)handle;
    uint32_t size = va_adapter_get_total_stack_size(handle), free_bytes = 0;
    if (!thread || !size) return 0;
    ULONG fill;
#if defined(TX_ENABLE_RANDOM_NUMBER_STACK_FILLING) && defined(TX_ENABLE_STACK_CHECKING)
    memset(&fill, (UCHAR)thread->tx_thread_stack_fill_value, sizeof(fill));
#else
    memset(&fill, 0xef, sizeof(fill));
#endif
    const unsigned char *stack = (const unsigned char *)thread->tx_thread_stack_start;
    while (free_bytes + sizeof(ULONG) <= size) {
        ULONG word;
        memcpy(&word, stack + free_bytes, sizeof(word));
        if (word != fill) break;
        free_bytes += sizeof(ULONG);
    }
    return size - free_bytes;
}
#endif

static VA_QueueObjectType_t va_tx_type(unsigned int op)
{
    if (op >= VA_TX_QUEUE_CREATE && op <= VA_TX_QUEUE_FLUSH) return VA_OBJECT_TYPE_QUEUE;
    if (op >= VA_TX_MUTEX_CREATE && op <= VA_TX_MUTEX_PUT) return VA_OBJECT_TYPE_RECURSIVE_MUTEX;
    if (op >= VA_TX_SEMAPHORE_CREATE && op <= VA_TX_SEMAPHORE_CEILING_PUT) return VA_OBJECT_TYPE_COUNTING_SEM;
    if (op >= VA_TX_EVENT_FLAGS_CREATE && op <= VA_TX_EVENT_FLAGS_SET) return VA_OBJECT_TYPE_EVENTFLAG;
    if (op >= VA_TX_TIMER_CREATE && op <= VA_TX_TIMER_CHANGE) return VA_OBJECT_TYPE_TIMER;
    if (op >= VA_TX_BLOCK_POOL_CREATE) return VA_OBJECT_TYPE_MEM_SLAB;
    return VA_OBJECT_TYPE_HEAP;
}

static int va_tx_valid(void *object, VA_QueueObjectType_t type)
{
    if (!object) return 0;
    switch (type) {
    case VA_OBJECT_TYPE_QUEUE: return ((TX_QUEUE *)object)->tx_queue_id == TX_QUEUE_ID;
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX: return ((TX_MUTEX *)object)->tx_mutex_id == TX_MUTEX_ID;
    case VA_OBJECT_TYPE_COUNTING_SEM: return ((TX_SEMAPHORE *)object)->tx_semaphore_id == TX_SEMAPHORE_ID;
    case VA_OBJECT_TYPE_EVENTFLAG: return ((TX_EVENT_FLAGS_GROUP *)object)->tx_event_flags_group_id == TX_EVENT_FLAGS_ID;
    case VA_OBJECT_TYPE_TIMER: return ((TX_TIMER *)object)->tx_timer_id == TX_TIMER_ID;
    case VA_OBJECT_TYPE_MEM_SLAB: return ((TX_BLOCK_POOL *)object)->tx_block_pool_id == TX_BLOCK_POOL_ID;
    case VA_OBJECT_TYPE_HEAP: return ((TX_BYTE_POOL *)object)->tx_byte_pool_id == TX_BYTE_POOL_ID;
    default: return 0;
    }
}

static void va_tx_register_object(void *object, VA_QueueObjectType_t type)
{
    if (!va_tx_valid(object, type)) return;
#if VA_NEEDS_OBJECT_REGISTRY
    if (_va_find_queue_object_id(object) != 0) return;
#endif
    const char *name = "";
    switch (type) {
    case VA_OBJECT_TYPE_QUEUE: name = ((TX_QUEUE *)object)->tx_queue_name; break;
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX: name = ((TX_MUTEX *)object)->tx_mutex_name; break;
    case VA_OBJECT_TYPE_COUNTING_SEM: name = ((TX_SEMAPHORE *)object)->tx_semaphore_name; break;
    case VA_OBJECT_TYPE_EVENTFLAG: name = ((TX_EVENT_FLAGS_GROUP *)object)->tx_event_flags_group_name; break;
    case VA_OBJECT_TYPE_TIMER: name = ((TX_TIMER *)object)->tx_timer_name; break;
    case VA_OBJECT_TYPE_MEM_SLAB: name = ((TX_BLOCK_POOL *)object)->tx_block_pool_name; break;
    case VA_OBJECT_TYPE_HEAP: name = ((TX_BYTE_POOL *)object)->tx_byte_pool_name; break;
    default: break;
    }
    va_logQueueObjectCreateTyped(object, name, type);
    if (type == VA_OBJECT_TYPE_QUEUE) {
        TX_QUEUE *queue = (TX_QUEUE *)object;
        va_logRtosObjectInfo(object, type, queue->tx_queue_capacity, queue->tx_queue_message_size * sizeof(ULONG));
    } else if (type == VA_OBJECT_TYPE_MEM_SLAB) {
        TX_BLOCK_POOL *pool = (TX_BLOCK_POOL *)object;
        va_logRtosObjectInfo(object, type, pool->tx_block_pool_total, pool->tx_block_pool_block_size);
    } else if (type == VA_OBJECT_TYPE_HEAP) {
        va_logHeapCapacity(object, name, ((TX_BYTE_POOL *)object)->tx_byte_pool_size);
    }
}

static void va_tx_queue_event(TX_QUEUE *queue, TX_THREAD *task, bool send, bool front, uint32_t used)
{
    va_tx_register_thread(task);
    va_tx_register_object(queue, VA_OBJECT_TYPE_QUEUE);
    if (send) va_logQueueObjectGiveTyped(queue, VA_OBJECT_TYPE_QUEUE);
    else va_logQueueObjectTakeTyped(queue, VA_OBJECT_TYPE_QUEUE);
    va_tx_operation(queue, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS,
        send ? (front ? VA_QUEUE_FRONT : VA_QUEUE_BACK) : VA_QUEUE_RECEIVE,
        used, 1, task, task == TX_NULL ? (uint16_t)__get_IPSR() : 0);
}

/* Native trace entries run under the kernel interrupt lock after input
 * validation. These predicates select branches that cannot fail. Emit the
 * committed transfer before ThreadX can wake and preempt into its consumer.
 * API-return wrappers own failures only for these operations. */
static void va_tx_native_sync(unsigned int event, void *object, uintptr_t a, uintptr_t b, uintptr_t c)
{
    TX_THREAD *task = va_tx_current();
    (void)b;
    switch (event) {
    case TX_TRACE_TIMER_CREATE:
    case TX_TRACE_TIMER_ACTIVATE: {
        TX_TIMER *timer = (TX_TIMER *)object;
        va_tx_register_object(timer, VA_OBJECT_TYPE_TIMER);
        TX_TIMER_INTERNAL *internal = &timer->tx_timer_internal;
        /* These native hooks execute with interrupts disabled, before a
           validated activation can preempt into the timer thread. */
        if ((event == TX_TRACE_TIMER_CREATE && c != TX_AUTO_ACTIVATE) ||
            internal->tx_timer_internal_list_head != TX_NULL ||
            internal->tx_timer_internal_remaining_ticks == 0 ||
            internal->tx_timer_internal_remaining_ticks == TX_WAIT_FOREVER) break;
        va_logTimerArmTicks(timer, internal->tx_timer_internal_remaining_ticks,
            internal->tx_timer_internal_re_initialize_ticks, TX_TIMER_TICKS_PER_SECOND,
            VA_TIMER_RELOAD_AFTER_CALLBACK);
        break;
    }
    case TX_TRACE_TIMER_DEACTIVATE:
    case TX_TRACE_TIMER_DELETE:
        va_tx_register_object(object, VA_OBJECT_TYPE_TIMER);
        va_logQueueObjectTakeTyped(object, VA_OBJECT_TYPE_TIMER);
        break;
    case TX_TRACE_MUTEX_GET: {
        TX_MUTEX *mutex = (TX_MUTEX *)object;
        if (mutex->tx_mutex_ownership_count != 0) break;
        va_tx_register_thread(task);
#if VA_TRACE_MUTEXES || VA_TRACE_SEMAPHORES
        if (va_tx_take_slot() >= VA_TX_TAKE_CONTEXTS) break;
#endif
        va_tx_register_object(mutex, VA_OBJECT_TYPE_RECURSIVE_MUTEX);
        va_logQueueObjectTakeTyped(mutex, VA_OBJECT_TYPE_RECURSIVE_MUTEX);
        (void)va_tx_take_recorded(1);
        break;
    }
    case TX_TRACE_SEMAPHORE_GET: {
        TX_SEMAPHORE *sem = (TX_SEMAPHORE *)object;
        if (sem->tx_semaphore_count == 0) break;
        va_tx_register_thread(task);
#if VA_TRACE_MUTEXES || VA_TRACE_SEMAPHORES
        if (va_tx_take_slot() >= VA_TX_TAKE_CONTEXTS) break;
#endif
        va_tx_register_object(sem, VA_OBJECT_TYPE_COUNTING_SEM);
        va_logQueueObjectTakeTyped(sem, VA_OBJECT_TYPE_COUNTING_SEM);
        (void)va_tx_take_recorded(1);
        break;
    }
    case TX_TRACE_QUEUE_SEND: case TX_TRACE_QUEUE_FRONT_SEND: {
        TX_QUEUE *queue = (TX_QUEUE *)object;
        if (queue->tx_queue_available_storage == 0) break;
        bool direct = queue->tx_queue_suspended_count != 0;
        va_tx_queue_event(queue, task, true, event == TX_TRACE_QUEUE_FRONT_SEND,
            queue->tx_queue_enqueued + (direct ? 0 : 1));
        if (direct) va_tx_queue_event(queue, queue->tx_queue_suspension_list, false, false, 0);
        break;
    }
    case TX_TRACE_QUEUE_RECEIVE: {
        TX_QUEUE *queue = (TX_QUEUE *)object;
        if (queue->tx_queue_enqueued == 0) break;
        TX_THREAD *sender = queue->tx_queue_suspended_count ? queue->tx_queue_suspension_list : TX_NULL;
        bool front = sender && sender->tx_thread_suspend_option == TX_TRUE;
        if (front) va_tx_queue_event(queue, sender, true, true, queue->tx_queue_enqueued);
        va_tx_queue_event(queue, task, false, false, queue->tx_queue_enqueued - (front ? 0 : 1));
        if (sender && !front) va_tx_queue_event(queue, sender, true, false, queue->tx_queue_enqueued);
        break;
    }
    case TX_TRACE_SEMAPHORE_PUT: case TX_TRACE_SEMAPHORE_CEILING_PUT: {
        TX_SEMAPHORE *sem = (TX_SEMAPHORE *)object;
        if (event == TX_TRACE_SEMAPHORE_CEILING_PUT && sem->tx_semaphore_suspended_count == 0 && sem->tx_semaphore_count >= c) break;
        va_tx_register_object(sem, VA_OBJECT_TYPE_COUNTING_SEM);
        va_logQueueObjectGiveTyped(sem, VA_OBJECT_TYPE_COUNTING_SEM);
        break;
    }
    case TX_TRACE_MUTEX_PUT: {
        TX_MUTEX *mutex = (TX_MUTEX *)object;
        if (mutex->tx_mutex_ownership_count != 1 ||
            (mutex->tx_mutex_owner != _tx_thread_current_ptr && _tx_thread_preempt_disable == 0)) break;
        va_tx_register_object(mutex, VA_OBJECT_TYPE_RECURSIVE_MUTEX);
        va_logQueueObjectGiveTyped(mutex, VA_OBJECT_TYPE_RECURSIVE_MUTEX);
        break;
    }
    case TX_TRACE_EVENT_FLAGS_SET: {
        TX_EVENT_FLAGS_GROUP *flags = (TX_EVENT_FLAGS_GROUP *)object;
        va_tx_register_thread(task);
        va_tx_register_object(flags, VA_OBJECT_TYPE_EVENTFLAG);
        if (b == TX_OR) va_logEventFlagSet(flags, (uint32_t)a);
        va_tx_operation(flags, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
            b == TX_AND ? VA_FLAGS_CLEAR : VA_FLAGS_SET, flags->tx_event_flags_group_current,
            b == TX_AND ? ~(uint32_t)a : (uint32_t)a, task, (uint16_t)__get_IPSR());
        break;
    }
    default: break;
    }
}

#if VA_NEEDS_OBJECT_REGISTRY
VA_QueueObjectType_t va_adapter_get_queue_object_type(void *object)
{
    int index = _va_find_queue_object_index(object);
    return index >= 0 ? queueObjectMap[index].type : VA_OBJECT_TYPE_QUEUE;
}
#endif
#if VA_NEEDS_BLOCKING_HOOK
void va_adapter_check_mutex_contention(void *object, uint16_t id)
{
    TX_THREAD *waiter = va_tx_current();
    TX_THREAD *owner = ((TX_MUTEX *)object)->tx_mutex_owner;
    if (owner == TX_NULL || waiter == TX_NULL || owner == waiter) return;
    va_tx_register_thread(owner);
    va_tx_register_thread(waiter);
    uint16_t owner_id = _va_find_task_id(owner), waiter_id = _va_find_task_id(waiter);
    if (owner_id && waiter_id) _va_send_mutex_contention_packet(id, waiter_id, owner_id, _va_get_timestamp());
}
#endif

/* Release APIs accept only a memory address. Locate its pool by range,
 * never read an allocator header through the application's pointer. */
static void *va_tx_release_pool(unsigned int op, void *memory)
{
    uintptr_t address = (uintptr_t)memory;
    if (op == VA_TX_BYTE_RELEASE) {
        TX_BYTE_POOL *pool = _tx_byte_pool_created_ptr;
        for (ULONG i = 0; pool && i < _tx_byte_pool_created_count; ++i, pool = pool->tx_byte_pool_created_next) {
            uintptr_t start = (uintptr_t)pool->tx_byte_pool_start;
            if (address >= start && address - start < pool->tx_byte_pool_size) return pool;
        }
    } else {
        TX_BLOCK_POOL *pool = _tx_block_pool_created_ptr;
        for (ULONG i = 0; pool && i < _tx_block_pool_created_count; ++i, pool = pool->tx_block_pool_created_next) {
            uintptr_t start = (uintptr_t)pool->tx_block_pool_start;
            if (address >= start && address - start < pool->tx_block_pool_size) return pool;
        }
    }
    return TX_NULL;
}

void *va_threadx_api_begin(unsigned int op, void *object, uintptr_t a, uintptr_t b, uintptr_t c)
{
    if (!VA_IsInit()) return object;
    TX_THREAD *task = va_tx_current();
    va_tx_register_thread(task);
    if (op == VA_TX_BYTE_RELEASE || op == VA_TX_BLOCK_RELEASE) {
        VA_CS_ENTER();
        object = va_tx_release_pool(op, object);
        VA_CS_EXIT();
        return object;
    }
    if (op < VA_TX_QUEUE_CREATE) return object;
    /* Create arguments may point to an uninitialized control block. */
    switch (op) {
    case VA_TX_QUEUE_CREATE: case VA_TX_MUTEX_CREATE: case VA_TX_SEMAPHORE_CREATE:
    case VA_TX_EVENT_FLAGS_CREATE: case VA_TX_TIMER_CREATE:
    case VA_TX_BYTE_POOL_CREATE: case VA_TX_BLOCK_POOL_CREATE: return object;
    default: break;
    }
    VA_QueueObjectType_t type = va_tx_type(op);
    VA_CS_ENTER();
    if (!va_tx_valid(object, type)) { VA_CS_EXIT(); return object; }
    va_tx_register_object(object, type);
    if (op == VA_TX_MUTEX_GET || op == VA_TX_SEMAPHORE_GET) (void)va_tx_take_recorded(0);
    VA_WaitReason_t reason = VA_WAIT_UNKNOWN;
    switch (op) {
    case VA_TX_QUEUE_SEND: case VA_TX_QUEUE_FRONT_SEND: reason = VA_WAIT_QUEUE_SEND; break;
    case VA_TX_QUEUE_RECEIVE: reason = VA_WAIT_QUEUE_RECEIVE; break;
    case VA_TX_MUTEX_GET: reason = VA_WAIT_MUTEX; break;
    case VA_TX_SEMAPHORE_GET: reason = VA_WAIT_SEMAPHORE; break;
    case VA_TX_EVENT_FLAGS_GET: reason = VA_WAIT_EVENT_FLAGS; break;
    case VA_TX_BYTE_ALLOCATE: reason = VA_WAIT_HEAP; break;
    case VA_TX_BLOCK_ALLOCATE: reason = VA_WAIT_MEM_SLAB; break;
    default: break;
    }
    if (task && reason != VA_WAIT_UNKNOWN && a != TX_NO_WAIT)
        va_logTaskWait(task, reason, object, type, (uint32_t)a);
    if (op == VA_TX_MUTEX_GET && a != TX_NO_WAIT) va_logQueueObjectBlocking(object);
    if (op == VA_TX_QUEUE_SEND || op == VA_TX_QUEUE_FRONT_SEND)
        va_tx_operation(object, type, VA_EVENT_QUEUE_DETAILS, (VA_RtosOperation_t)VA_QUEUE_SEND_BEGIN,
            ((TX_QUEUE *)object)->tx_queue_enqueued, op == VA_TX_QUEUE_FRONT_SEND ? 1 : 0,
            task, (uint16_t)__get_IPSR());
    if (op == VA_TX_EVENT_FLAGS_GET) {
        uint8_t flags = VA_FLAGS_WAIT_BEGIN | ((c & TX_AND) ? VA_FLAGS_ALL : 0) | ((c & TX_OR_CLEAR) ? VA_FLAGS_CLEAR_ON_EXIT : 0);
        va_tx_operation(object, type, VA_EVENT_FLAG_DETAILS, flags,
            ((TX_EVENT_FLAGS_GROUP *)object)->tx_event_flags_group_current, (uint32_t)b, task, (uint16_t)__get_IPSR());
    }
    VA_CS_EXIT();
    return object;
}

VA_ThreadXWait va_threadx_wait_begin(unsigned int op, void *object, uintptr_t a, uintptr_t b, uintptr_t c)
{
    VA_ThreadXWait context = {object, 0, 0, 0, false};
    if (!VA_IsInit()) return context;
    VA_CS_ENTER();
    context.object = va_threadx_api_begin(op, object, a, b, c);
#if VA_NEEDS_OBJECT_REGISTRY
    TX_THREAD *task = va_tx_current();
    if (task) va_tx_register_thread(task);
    context.slot = _va_hold_object_reference(task, context.object, &context.ownsReference);
    if (context.slot) {
        context.id = queueObjectMap[context.slot - 1u].id;
        context.generation = queueObjectMap[context.slot - 1u].generation;
    }
#endif
    VA_CS_EXIT();
    return context;
}

void va_threadx_wait_end(unsigned int op, VA_ThreadXWait context, uintptr_t a, uintptr_t b, uintptr_t c, unsigned int status)
{
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    TX_THREAD *task = va_tx_current();
#if VA_NEEDS_OBJECT_REGISTRY && VA_NEEDS_TASK_REGISTRY
    if (context.id && !_va_has_object_reference(task, context.slot, context.id, context.generation)) {
        VA_CS_EXIT();
        return; /* cancelled/reset wait, or a duplicate completion */
    }
#endif
    bool live = status != TX_DELETED;
#if VA_NEEDS_OBJECT_REGISTRY
    if (context.id && _va_find_queue_object_id(context.object) != context.id) live = false;
#endif
    if (live) {
        va_threadx_api_end(op, context.object, a, b, c, status);
#if VA_NEEDS_OBJECT_REGISTRY
        if (context.ownsReference) _va_release_object_reference(task, context.slot);
#endif
        VA_CS_EXIT();
        return;
    }
    if (task) va_clearTaskWait(task);
#if VA_NEEDS_OBJECT_REGISTRY
    /* The control block may already be freed or hold an unrelated object.
     * The pending API pins this ID until its final result has been emitted. */
    if (context.id) {
        bool give = op == VA_TX_QUEUE_SEND || op == VA_TX_QUEUE_FRONT_SEND;
        if (status != TX_SUCCESS)
            _va_logObjectOpFailedId(context.id, va_tx_type(op), give, status);
#if VA_TRACE_MUTEXES || VA_TRACE_SEMAPHORES
        else if ((op == VA_TX_MUTEX_GET || op == VA_TX_SEMAPHORE_GET) && !va_tx_take_recorded(-1))
            _va_send_event_packet(op == VA_TX_MUTEX_GET ? VA_EVENT_MUTEX : VA_EVENT_SEMAPHORE,
                                 context.id, _va_get_timestamp_unlocked());
#endif
#if VA_TRACE_EVENT_FLAGS
        if (status == TX_SUCCESS && op == VA_TX_EVENT_FLAGS_GET)
            _va_send_data_event_packet(VA_EVENT_EVENTFLAG,
                                      context.id, (uint32_t)c, _va_get_timestamp_unlocked());
#endif
#if VA_HAS_QUEUE_DETAILS
        if (status != TX_SUCCESS && (op == VA_TX_QUEUE_SEND || op == VA_TX_QUEUE_FRONT_SEND || op == VA_TX_QUEUE_RECEIVE))
            _va_logRtosOperationId(context.id, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS,
                give ? VA_QUEUE_SEND_FAILED : VA_QUEUE_RECEIVE_FAILED, UINT32_MAX, 0, task, 0);
#endif
#if VA_HAS_EVENT_FLAG_DETAILS
        if (op == VA_TX_EVENT_FLAGS_GET)
            _va_logRtosOperationId(context.id, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
                (status == TX_SUCCESS ? VA_FLAGS_WAIT_OK : VA_FLAGS_WAIT_TIMEOUT) | ((a & TX_AND) ? VA_FLAGS_ALL : 0) |
                ((a & TX_OR_CLEAR) ? VA_FLAGS_CLEAR_ON_EXIT : 0), (uint32_t)c, (uint32_t)b, task, 0);
#endif
    }
#endif
#if VA_NEEDS_OBJECT_REGISTRY
    if (context.ownsReference) _va_release_object_reference(task, context.slot);
#endif
    VA_CS_EXIT();
}

void VA_ThreadX_TimerEnter(void *object)
{
    if (!VA_IsInit() || !va_tx_valid(object, VA_OBJECT_TYPE_TIMER)) return;
    va_tx_register_object(object, VA_OBJECT_TYPE_TIMER);
    TX_TIMER *timer = (TX_TIMER *)object;
    va_logTimerCallback(timer, true, false, (void *)timer->tx_timer_internal.tx_timer_internal_timeout_function);
}
void VA_ThreadX_TimerExit(void *object)
{
    if (!VA_IsInit() || !va_tx_valid(object, VA_OBJECT_TYPE_TIMER)) return;
    TX_TIMER *timer = (TX_TIMER *)object;
    va_logTimerCallback(timer, false, false, (void *)timer->tx_timer_internal.tx_timer_internal_timeout_function);
}

void va_threadx_api_end(unsigned int op, void *object, uintptr_t a, uintptr_t b, uintptr_t c, unsigned int status)
{
    if (!VA_IsInit()) return;
    TX_THREAD *task = va_tx_current();
    VA_CS_ENTER();
    if (task) va_clearTaskWait(task);
    if (op < VA_TX_QUEUE_CREATE) {
        if (status == TX_SUCCESS && op == VA_TX_THREAD_PRIORITY_CHANGE)
            va_threadx_priority_changed(object);
        if (status == TX_SUCCESS && (op == VA_TX_THREAD_TERMINATE || op == VA_TX_THREAD_RESET)) {
            va_tx_clear_thread_tracking((TX_THREAD *)object);
            va_clearTaskWait(object);
            va_logTaskState(object, VA_TASK_SUSPENDED);
        }
        VA_CS_EXIT();
        return;
    }
    VA_QueueObjectType_t type = va_tx_type(op);
    switch (op) {
    case VA_TX_QUEUE_DELETE: case VA_TX_MUTEX_DELETE: case VA_TX_SEMAPHORE_DELETE:
    case VA_TX_EVENT_FLAGS_DELETE: case VA_TX_TIMER_DELETE:
    case VA_TX_BYTE_POOL_DELETE: case VA_TX_BLOCK_POOL_DELETE:
        if (status == TX_SUCCESS) va_logQueueObjectDelete(object);
        VA_CS_EXIT(); return;
    default: break;
    }
#if VA_NEEDS_OBJECT_REGISTRY
    /* Delete clears the kernel ID before it resumes waiters. Their registry
     * identity remains usable until the deleting API returns and retires it. */
    if (status == TX_DELETED && _va_find_queue_object_id(object) != 0) {
        bool give = op == VA_TX_QUEUE_SEND || op == VA_TX_QUEUE_FRONT_SEND;
        va_logObjectOpFailedTyped(object, type, give, status);
        VA_CS_EXIT(); return;
    }
#endif
    if (!va_tx_valid(object, type)) { VA_CS_EXIT(); return; }
    va_tx_register_object(object, type);
    bool success = status == TX_SUCCESS;
    uint16_t exception = (uint16_t)__get_IPSR();
    switch (op) {
    case VA_TX_QUEUE_SEND: case VA_TX_QUEUE_FRONT_SEND: case VA_TX_QUEUE_RECEIVE: {
        bool give = op != VA_TX_QUEUE_RECEIVE;
        if (!success) {
            va_logObjectOpFailedTyped(object, type, give, status);
            va_tx_operation(object, type, VA_EVENT_QUEUE_DETAILS, give ? VA_QUEUE_SEND_FAILED : VA_QUEUE_RECEIVE_FAILED,
                ((TX_QUEUE *)object)->tx_queue_enqueued, 0, task, exception);
        }
        break;
    }
    case VA_TX_QUEUE_FLUSH:
        if (success) va_tx_operation(object, type, VA_EVENT_QUEUE_DETAILS, (VA_RtosOperation_t)VA_QUEUE_RESET,
            ((TX_QUEUE *)object)->tx_queue_enqueued, 0, task, exception);
        break;
    case VA_TX_MUTEX_GET: case VA_TX_MUTEX_PUT: case VA_TX_SEMAPHORE_GET:
    case VA_TX_SEMAPHORE_PUT: case VA_TX_SEMAPHORE_CEILING_PUT: {
        bool give = op != VA_TX_MUTEX_GET && op != VA_TX_SEMAPHORE_GET;
        if (success) {
            if (!give && !va_tx_take_recorded(-1) &&
                (op != VA_TX_MUTEX_GET || ((TX_MUTEX *)object)->tx_mutex_ownership_count == 1))
                va_logQueueObjectTakeTyped(object, type);
        } else va_logObjectOpFailedTyped(object, type, give, status);
        break;
    }
    case VA_TX_EVENT_FLAGS_GET:
        if (success) va_logEventFlagWaitEnd(object, (uint32_t)c);
        else va_logObjectOpFailedTyped(object, type, false, status);
        va_tx_operation(object, type, VA_EVENT_FLAG_DETAILS, (success ? VA_FLAGS_WAIT_OK : VA_FLAGS_WAIT_TIMEOUT)
            | ((a & TX_AND) ? VA_FLAGS_ALL : 0) | ((a & TX_OR_CLEAR) ? VA_FLAGS_CLEAR_ON_EXIT : 0),
            (uint32_t)c, (uint32_t)b, task, exception);
        va_tx_operation(object, type, VA_EVENT_FLAG_DETAILS, (VA_RtosOperation_t)VA_FLAGS_SNAPSHOT,
            ((TX_EVENT_FLAGS_GROUP *)object)->tx_event_flags_group_current, 0, task, exception);
        break;
    case VA_TX_EVENT_FLAGS_SET:
        if (success) {
            va_tx_operation(object, type, VA_EVENT_FLAG_DETAILS, (VA_RtosOperation_t)VA_FLAGS_SNAPSHOT,
                ((TX_EVENT_FLAGS_GROUP *)object)->tx_event_flags_group_current, 0, task, exception);
        } else va_logObjectOpFailedTyped(object, type, true, status);
        break;
    case VA_TX_BYTE_ALLOCATE: case VA_TX_BYTE_RELEASE: {
        TX_BYTE_POOL *pool = (TX_BYTE_POOL *)object;
        uint32_t used = (uint32_t)(pool->tx_byte_pool_size - pool->tx_byte_pool_available);
        if (!success && op == VA_TX_BYTE_ALLOCATE) va_logHeapAllocFailed(object, (uint32_t)b);
        else if (success && op == VA_TX_BYTE_ALLOCATE) va_logHeapAlloc(object, used);
        else if (success) va_logHeapFree(object, used);
        break;
    }
    case VA_TX_BLOCK_ALLOCATE: case VA_TX_BLOCK_RELEASE: {
        TX_BLOCK_POOL *pool = (TX_BLOCK_POOL *)object;
        if (success || op == VA_TX_BLOCK_ALLOCATE)
            va_tx_operation(object, type, VA_EVENT_MEM_SLAB,
                success ? (op == VA_TX_BLOCK_ALLOCATE ? VA_OP_ALLOC : VA_OP_FREE) : VA_OP_ALLOC_FAILED,
                pool->tx_block_pool_total - pool->tx_block_pool_available, status, task, exception);
        break;
    }
    default: break;
    }
    VA_CS_EXIT();
}
#endif
