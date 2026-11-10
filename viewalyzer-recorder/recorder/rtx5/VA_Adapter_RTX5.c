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
#include "ViewAlyzerRTX5Hook.h"
#include "VA_Internal.h"
#if VA_ENABLED && (VA_RTOS_SELECT == VA_RTOS_RTX5)

#if VA_NEEDS_SWITCH_HOOK
static osRtxThread_t *va_rtx5_running;
static bool va_rtx5_running_destroyed;
#endif
#if VA_NEEDS_TASK_REGISTRY
typedef struct {
    bool freed, destroyed;
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
    bool dirty_priority;
#endif
#if VA_RTX5_WAITS
    void *object;
    uint32_t detail;
    uint8_t wait_kind, options;
#endif
#if VA_TRACE_TASK_STATES
    bool blocked;
#endif
#if VA_TRACE_SLEEP
    bool sleeping;
#endif
} VA_Rtx5Task;
static VA_Rtx5Task va_rtx5_tasks[VA_MAX_TASKS];
#endif
#if VA_TRACE_ISRS
static bool va_rtx5_isrs_named;
#endif
typedef struct { void *object; osRtxThread_t *thread; uint8_t kind; } VA_Rtx5Transfer;
#if VA_RTX5_WAITS
static VA_Rtx5Transfer va_rtx5_transfers[4];
#endif
#if VA_RTX5_OBJECT_SHADOWS
typedef union { osRtxThread_t *owner; uint32_t used; } VA_Rtx5Object;
static VA_Rtx5Object va_rtx5_objects[VA_MAX_SYNC_OBJECTS];
#endif
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
static unsigned va_rtx5_dirty_priorities;
#endif
static inline void va_rtx5_finish_pending(void *thread, bool success, uint32_t result);
static inline void va_rtx5_unblocked(void *thread, uint32_t result);
static inline VA_Rtx5Transfer *va_rtx5_transfer(void);
static inline void va_rtx5_sample_priorities(void);
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_NOTIFICATIONS
#define VA_RTX5_FLAG_REQUESTS 8
static struct VA_Rtx5FlagRequest_s {
    void *object;
    osRtxThread_t *thread;
    uint32_t mask;
    uint16_t exception;
    uint8_t operation;
    bool emitted, thread_flags;
} va_rtx5_flag_requests[VA_RTX5_FLAG_REQUESTS];
typedef struct VA_Rtx5FlagRequest_s VA_Rtx5FlagRequest;
#endif
static inline void va_rtx5_flags_forget_thread(void *thread);
extern const unsigned char va_rtx5_port_config;

void va_rtx5_validate_kernel(void)
{
    osStatus_t (*volatile initialize)(void) = osKernelInitialize;
    const void *volatile port = &va_rtx5_port_config;
    (void)initialize;
    (void)port;
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_NOTIFICATIONS
    memset(va_rtx5_flag_requests, 0, sizeof(va_rtx5_flag_requests));
#endif
#if VA_RTX5_WAITS
    memset(va_rtx5_transfers, 0, sizeof(va_rtx5_transfers));
#endif
#if VA_RTX5_OBJECT_SHADOWS
    memset(va_rtx5_objects, 0, sizeof(va_rtx5_objects));
#endif
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
    va_rtx5_dirty_priorities = 0;
#endif
#if VA_NEEDS_SWITCH_HOOK
    va_rtx5_running = NULL;
    va_rtx5_running_destroyed = false;
#endif
#if VA_NEEDS_TASK_REGISTRY
    memset(va_rtx5_tasks, 0, sizeof(va_rtx5_tasks));
#endif
#if VA_TRACE_ISRS
    va_rtx5_isrs_named = false;
#endif
}

static inline void va_rtx5_register_thread(osRtxThread_t *thread)
{
#if VA_NEEDS_TASK_REGISTRY
    if (!VA_IsInit() || !thread || thread->id != osRtxIdThread ||
        thread->state == osRtxThreadInactive || thread->state == osRtxThreadTerminated) return;
    VA_CS_ENTER();
    if (!_va_find_task_id(thread)) {
        g_task_pxStack = thread->stack_mem;
        g_task_pxEndOfStack = (uint8_t *)thread->stack_mem + thread->stack_size;
        g_task_ulStackDepth = thread->stack_size;
        g_task_uxPriority = (uint32_t)thread->priority;
        g_task_uxBasePriority = (uint32_t)thread->priority_base;
        va_taskcreated(thread, thread->name);
        int i = _va_find_task_index(thread);
        if (i >= 0) memset(&va_rtx5_tasks[i], 0, sizeof(va_rtx5_tasks[i]));
    }
    VA_CS_EXIT();
#else
    (void)thread;
#endif
}

static inline void va_rtx5_priority(osRtxThread_t *thread, unsigned cause)
{
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
    if (!thread || thread->id != osRtxIdThread ||
        thread->state == osRtxThreadInactive || thread->state == osRtxThreadTerminated) return;
    va_rtx5_register_thread(thread);
    VA_CS_ENTER();
    int i = _va_find_task_index(thread);
    if (i >= 0 && (taskMap[i].uxPriority != (uint32_t)thread->priority ||
                   taskMap[i].uxBasePriority != (uint32_t)thread->priority_base)) {
        if (!cause) cause = thread->priority > thread->priority_base ? 2 : 3;
        va_logTaskPriority(thread, thread->priority, thread->priority_base, cause);
        /* Priority packets are gated by TASK_STATES, but the registry shadow
           is also used by task-only builds. */
        taskMap[i].uxPriority = (uint32_t)thread->priority;
        taskMap[i].uxBasePriority = (uint32_t)thread->priority_base;
    }
    VA_CS_EXIT();
#else
    (void)thread; (void)cause;
#endif
}

static inline void va_rtx5_cancel_wait(void *thread, uint32_t result)
{
    va_rtx5_finish_pending(thread, false, result);
#if VA_TRACE_TASK_STATES
    int state_index = _va_find_task_index(thread);
    if (state_index >= 0) va_rtx5_tasks[state_index].blocked = false;
#endif
#if VA_TRACE_SLEEP
    int i = _va_find_task_index(thread);
    if (i >= 0 && va_rtx5_tasks[i].sleeping) {
        va_logSleepExit(thread);
        va_rtx5_tasks[i].sleeping = false;
    }
#endif
    va_clearTaskWait(thread);
}

#if VA_NEEDS_TASK_REGISTRY
void EvrRtxThreadCreated(osThreadId_t thread_id, uint32_t thread_addr, const char *name)
{
    (void)thread_addr; (void)name;
    va_rtx5_register_thread(thread_id);
    va_logTaskState(thread_id, VA_TASK_READY);
}
void EvrRtxThreadDestroyed(osThreadId_t thread_id)
{
    /* Detached TCBs are already freed. Only the registry key is safe then.
       Joinable TCBs remain valid until a later join/detach BeforeFree call. */
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    va_rtx5_flags_forget_thread(thread_id);
#if VA_RTX5_OBJECT_SHADOWS
    /* A non-robust mutex can outlive its owner. Never preserve an address
       that could later identify a different thread after TCB reuse. */
    for (unsigned object = 0; object < VA_MAX_SYNC_OBJECTS; ++object) {
        if (queueObjectMap[object].active &&
            (queueObjectMap[object].type == VA_OBJECT_TYPE_MUTEX ||
             queueObjectMap[object].type == VA_OBJECT_TYPE_RECURSIVE_MUTEX) &&
            va_rtx5_objects[object].owner == thread_id)
            va_rtx5_objects[object].owner = NULL;
    }
#endif
    int i = _va_find_task_index(thread_id);
#if VA_NEEDS_SWITCH_HOOK
    if (va_rtx5_running == thread_id) va_rtx5_running_destroyed = true;
#endif
    if (i >= 0) {
#if VA_TRACE_STACK_USAGE
        if (!va_rtx5_tasks[i].freed)
            _va_send_stack_usage_packet(taskMap[i].id, _va_get_timestamp_unlocked(),
                va_adapter_calculate_stack_usage(thread_id), va_adapter_get_total_stack_size(thread_id));
#endif
        va_rtx5_cancel_wait(thread_id, (uint32_t)osErrorResource);
        va_rtx5_tasks[i].destroyed = true;
        va_logTaskState(thread_id, VA_TASK_DELETED);
#if VA_NEEDS_SWITCH_HOOK
        /* Retain the running task's id until the physical switch boundary.
           The boundary must not inspect its freed TCB or stack. */
        if (va_rtx5_running != thread_id)
#endif
            va_taskdeleted(thread_id);
    }
    VA_CS_EXIT();
}

void osRtxThreadBeforeFree(osRtxThread_t *thread)
{
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    int i = _va_find_task_index(thread);
    /* Join/detach of an already destroyed thread must not re-register it. */
    if (i >= 0 && !va_rtx5_tasks[i].destroyed && !va_rtx5_tasks[i].freed) {
#if VA_TRACE_STACK_USAGE
        _va_send_stack_usage_packet(taskMap[i].id, _va_get_timestamp_unlocked(),
            va_adapter_calculate_stack_usage(thread), va_adapter_get_total_stack_size(thread));
#endif
        va_rtx5_tasks[i].freed = true;
    }
    VA_CS_EXIT();
}
#endif

#if VA_NEEDS_SWITCH_HOOK || VA_NEEDS_TASK_REGISTRY
void va_rtx5_exception_return(void)
{
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    va_rtx5_sample_priorities();
    VA_Rtx5Transfer *transfer = va_rtx5_transfer();
    if (transfer) memset(transfer, 0, sizeof(*transfer));
#if VA_NEEDS_SWITCH_HOOK
    osRtxThread_t *thread = osRtxInfo.thread.run.next;
#if VA_NEEDS_TASK_SWITCH_EVENTS || VA_TRACE_SLEEP
    int previous = _va_find_task_index(va_rtx5_running);
#endif
    bool deleted = va_rtx5_running_destroyed;
    if (va_rtx5_running && !deleted) va_rtx5_priority(va_rtx5_running, 0);
    va_rtx5_register_thread(thread);
    va_rtx5_priority(thread, 0);
    if (va_rtx5_running != thread) {
        if (va_rtx5_running) {
            if (deleted) {
#if VA_NEEDS_TASK_SWITCH_EVENTS
                if (previous >= 0)
                    _va_send_event_packet(VA_EVENT_TASK_SWITCH, taskMap[previous].id, _va_get_timestamp_unlocked());
#endif
                va_taskdeleted(va_rtx5_running);
            } else {
              if (va_rtx5_running->state == osRtxThreadWaitingDelay) {
                va_logTaskState(va_rtx5_running, VA_TASK_SLEEPING);
#if VA_TRACE_SLEEP
                if (previous >= 0 && !va_rtx5_tasks[previous].sleeping) {
                    va_rtx5_tasks[previous].sleeping = true;
                    va_logSleepEnter(va_rtx5_running);
                }
#endif
              }
#if VA_TRACE_TASK_STATES
              else if (previous >= 0 && va_rtx5_tasks[previous].blocked)
                  va_logTaskState(va_rtx5_running, VA_TASK_BLOCKED);
              if (previous >= 0) va_rtx5_tasks[previous].blocked = false;
#endif
              va_taskswitchedout(va_rtx5_running);
            }
        }
        if (thread) va_taskswitchedin(thread);
        va_rtx5_running = thread;
        va_rtx5_running_destroyed = false;
    }
#endif
    VA_CS_EXIT();
}
#endif

#if VA_TRACE_ISRS
void va_rtx5_isr_enter(void)
{
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    if (!va_rtx5_isrs_named) {
        VA_RegisterUserTrace(VA_ISR_ID_SYSTICK, "SysTick", VA_USER_TYPE_ISR);
        VA_RegisterUserTrace(VA_ISR_ID_PENDSV, "PendSV", VA_USER_TYPE_ISR);
        VA_RegisterUserTrace(11, "SVCall", VA_USER_TYPE_ISR);
        va_rtx5_isrs_named = true;
    }
    uint32_t exception = __get_IPSR();
    VA_LogISRStart(exception == 15 ? VA_ISR_ID_SYSTICK : exception == 14 ? VA_ISR_ID_PENDSV : 11);
    VA_CS_EXIT();
}
void va_rtx5_isr_exit(void)
{
    uint32_t exception = __get_IPSR();
    VA_LogISREnd(exception == 15 ? VA_ISR_ID_SYSTICK : exception == 14 ? VA_ISR_ID_PENDSV : 11);
}
#endif

#if VA_TRACE_TASK_STATES
void EvrRtxThreadBlocked(osThreadId_t thread_id, uint32_t timeout)
{
    (void)timeout;
    VA_CS_ENTER();
    va_rtx5_register_thread(thread_id);
    int i = _va_find_task_index(thread_id);
    if (i >= 0) va_rtx5_tasks[i].blocked = true;
    VA_CS_EXIT();
}
#endif
#if VA_NEEDS_TASK_REGISTRY
void EvrRtxThreadSuspended(osThreadId_t thread_id)
{
    VA_CS_ENTER();
    va_rtx5_cancel_wait(thread_id, (uint32_t)osErrorTimeout);
    va_logTaskState(thread_id, VA_TASK_SUSPENDED);
    VA_CS_EXIT();
}
void EvrRtxThreadResumed(osThreadId_t thread_id)
{
    VA_CS_ENTER();
    va_rtx5_cancel_wait(thread_id, (uint32_t)osErrorTimeout);
    va_logTaskState(thread_id, VA_TASK_READY);
    VA_CS_EXIT();
}
#endif
#if VA_NEEDS_TASK_REGISTRY
void EvrRtxThreadUnblocked(osThreadId_t thread_id, uint32_t ret_val)
{
    VA_CS_ENTER();
    va_rtx5_unblocked(thread_id, ret_val);
#if VA_TRACE_TASK_STATES
    int i = _va_find_task_index(thread_id);
    if (i >= 0) va_rtx5_tasks[i].blocked = false;
#endif
    va_logTaskState(thread_id, VA_TASK_READY);
    VA_CS_EXIT();
}
#endif
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
void EvrRtxThreadPriorityUpdated(osThreadId_t thread_id, osPriority_t priority)
{
    (void)priority;
    va_rtx5_priority(thread_id, 1);
}
#endif
#if VA_TRACE_SLEEP
void EvrRtxDelayCompleted(osThreadId_t thread_id)
{
    VA_CS_ENTER();
    va_rtx5_cancel_wait(thread_id, (uint32_t)osErrorTimeout);
    VA_CS_EXIT();
}
#endif

#if VA_TRACE_STACK_USAGE
uint32_t va_adapter_get_total_stack_size(void *handle)
{
    const osRtxThread_t *thread = handle;
    return thread && thread->id == osRtxIdThread ? thread->stack_size : 0;
}
uint32_t va_adapter_calculate_stack_usage(void *handle)
{
    const osRtxThread_t *thread = handle;
    uint32_t size = va_adapter_get_total_stack_size(handle), free_bytes = 0;
    if (!size || !thread->stack_mem) return 0;
    const uint8_t *stack = thread->stack_mem;
    uint32_t word;
    if (size >= 4) {
        memcpy(&word, stack, 4);
        if (word == osRtxStackMagicWord) free_bytes = 4;
    }
    if (!free_bytes) return size;
    while (free_bytes + 4 <= size) {
        memcpy(&word, stack + free_bytes, 4);
        if (word != osRtxStackFillPattern) break;
        free_bytes += 4;
    }
    return size - free_bytes;
}
#endif
#if VA_NEEDS_OBJECT_REGISTRY
VA_QueueObjectType_t va_adapter_get_queue_object_type(void *object)
{
    int i = _va_find_queue_object_index(object);
    return i >= 0 ? queueObjectMap[i].type : VA_OBJECT_TYPE_QUEUE;
}
#endif
#if VA_NEEDS_BLOCKING_HOOK
void va_adapter_check_mutex_contention(void *object, uint16_t id)
{
    osRtxMutex_t *mutex = object;
    osRtxThread_t *waiter = osRtxInfo.thread.run.curr;
    if (!mutex || !mutex->owner_thread || !waiter || waiter == mutex->owner_thread) return;
    va_rtx5_register_thread(waiter);
    va_rtx5_register_thread(mutex->owner_thread);
    uint16_t w = _va_find_task_id(waiter), h = _va_find_task_id(mutex->owner_thread);
    if (w && h) _va_send_mutex_contention_packet(id, w, h, _va_get_timestamp());
}
#endif
#define VA_RTX5_HOOK(...) do { if (VA_IsInit()) { VA_CS_ENTER(); __VA_ARGS__; VA_CS_EXIT(); } } while (0)

static inline VA_QueueObjectType_t va_rtx5_wait_type(unsigned kind)
{
    switch (kind) {
    case VA_WAIT_MUTEX: return VA_OBJECT_TYPE_MUTEX;
    case VA_WAIT_SEMAPHORE: return VA_OBJECT_TYPE_COUNTING_SEM;
    case VA_WAIT_EVENT_FLAGS: return VA_OBJECT_TYPE_EVENTFLAG;
    case VA_WAIT_MEM_SLAB: return VA_OBJECT_TYPE_MEM_SLAB;
    default: return VA_OBJECT_TYPE_QUEUE;
    }
}

static inline osRtxThread_t *va_rtx5_caller(uint16_t *exception)
{
    uint32_t ipsr = __get_IPSR();
    *exception = (ipsr == 0 || ipsr == 11 || ipsr == 14) ? 0 : (uint16_t)ipsr;
    return ipsr == 0 || ipsr == 11 ? osRtxInfo.thread.run.curr : NULL;
}

static inline VA_Rtx5Transfer *va_rtx5_transfer(void)
{
#if VA_RTX5_WAITS
    switch (__get_IPSR()) {
    case 11: return &va_rtx5_transfers[0];
    case 14: return &va_rtx5_transfers[1];
    case 15: return &va_rtx5_transfers[2];
    case 0: return &va_rtx5_transfers[3];
    default: break; /* An external IRQ cannot consume an interrupted latch. */
    }
#endif
    return NULL;
}

static inline osRtxThread_t *va_rtx5_actor(void *object, unsigned kind, uint16_t *exception)
{
    VA_Rtx5Transfer *transfer = va_rtx5_transfer();
    osRtxThread_t *thread = va_rtx5_caller(exception);
    if (transfer && transfer->object == object && transfer->kind == kind) {
        thread = transfer->thread;
        *exception = 0;
        memset(transfer, 0, sizeof(*transfer));
    }
    va_rtx5_register_thread(thread);
    return thread;
}

static inline void va_rtx5_object(void *object, const char *name, VA_QueueObjectType_t type)
{
#if VA_NEEDS_OBJECT_REGISTRY
    if (!object) return;
    if (!_va_find_queue_object_id(object)) {
        va_logQueueObjectCreateTyped(object, name, type);
#if VA_RTX5_OBJECT_SHADOWS
        int i = _va_find_queue_object_index(object);
        if (i >= 0) memset(&va_rtx5_objects[i], 0, sizeof(va_rtx5_objects[i]));
#endif
    }
#else
    (void)object; (void)name; (void)type;
#endif
}

static inline void va_rtx5_operation(void *object, VA_QueueObjectType_t type,
                                    unsigned event, unsigned operation,
                                    uint32_t value, uint32_t detail,
                                    osRtxThread_t *thread, uint16_t exception)
{
#if VA_NEEDS_RTOS_OPERATIONS
    va_rtx5_register_thread(thread);
    va_logRtosOperation(object, type, (uint8_t)event,
        (VA_RtosOperation_t)(operation | VA_OP_EXPLICIT_ACTOR), value, detail, thread, exception);
#else
    (void)object; (void)type; (void)event; (void)operation;
    (void)value; (void)detail; (void)thread; (void)exception;
#endif
}

static inline void va_rtx5_mark_priority(osRtxThread_t *thread)
{
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
    int i = _va_find_task_index(thread);
    if (i >= 0 && !va_rtx5_tasks[i].dirty_priority) {
        va_rtx5_tasks[i].dirty_priority = true;
        ++va_rtx5_dirty_priorities;
    }
#else
    (void)thread;
#endif
}

static inline void va_rtx5_sample_priorities(void)
{
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
    /* Ordinary ticks pay one test; only a known owner change scans slots. */
    if (va_rtx5_dirty_priorities) {
        for (unsigned i = 0; i < VA_MAX_TASKS; ++i) {
            if (!va_rtx5_tasks[i].dirty_priority) continue;
            va_rtx5_tasks[i].dirty_priority = false;
            if (taskMap[i].active && !va_rtx5_tasks[i].freed && !va_rtx5_tasks[i].destroyed)
                va_rtx5_priority(taskMap[i].handle, 0);
        }
        va_rtx5_dirty_priorities = 0;
    }
#endif
}

static inline void va_rtx5_fail(void *object, unsigned kind, uint32_t detail,
                               uint8_t options, osRtxThread_t *thread, uint16_t exception)
{
    VA_QueueObjectType_t type = va_rtx5_wait_type(kind);
    if (kind == VA_WAIT_MUTEX || kind == VA_WAIT_SEMAPHORE) {
        va_logSyncActor(object, type, 2, thread, exception);
    } else if (kind == VA_WAIT_MEM_SLAB) {
#if VA_TRACE_MEM_SLABS
        va_rtx5_operation(object, type, VA_EVENT_MEM_SLAB, VA_OP_ALLOC_FAILED,
            ((osRtxMemoryPool_t *)object)->mp_info.used_blocks, (uint32_t)osErrorResource, thread, exception);
#endif
    } else {
        va_logObjectOpFailedTyped(object, type, kind == VA_WAIT_QUEUE_SEND, detail);
#if VA_HAS_QUEUE_DETAILS
        if (kind == VA_WAIT_QUEUE_SEND || kind == VA_WAIT_QUEUE_RECEIVE)
            va_rtx5_operation(object, type, VA_EVENT_QUEUE_DETAILS,
                kind == VA_WAIT_QUEUE_SEND ? VA_QUEUE_SEND_FAILED : VA_QUEUE_RECEIVE_FAILED,
                ((osRtxMessageQueue_t *)object)->msg_count, 0, thread, exception);
#endif
#if VA_HAS_EVENT_FLAG_DETAILS
        if (kind == VA_WAIT_EVENT_FLAGS)
            va_rtx5_operation(object, type, VA_EVENT_FLAG_DETAILS, VA_FLAGS_WAIT_TIMEOUT | options,
                0, detail, thread, exception);
#endif
    }
    (void)options;
}

static inline void va_rtx5_pending(void *object, unsigned kind, uint32_t detail, uint8_t options)
{
#if VA_RTX5_WAITS
    osRtxThread_t *thread = osRtxInfo.thread.run.curr;
    va_rtx5_register_thread(thread);
    int i = _va_find_task_index(thread);
    if (i >= 0) {
        va_rtx5_tasks[i].object = object;
        va_rtx5_tasks[i].wait_kind = (uint8_t)kind;
        va_rtx5_tasks[i].detail = detail;
        va_rtx5_tasks[i].options = options;
    }
    va_logTaskWait(thread, (VA_WaitReason_t)kind, object, va_rtx5_wait_type(kind), detail);
#else
    (void)object; (void)kind; (void)detail; (void)options;
#endif
}

static inline void va_rtx5_finish_pending(void *handle, bool success, uint32_t result)
{
#if VA_RTX5_WAITS
    int i = _va_find_task_index(handle);
    if (i < 0 || !va_rtx5_tasks[i].wait_kind) return;
    VA_Rtx5Task *state = &va_rtx5_tasks[i];
    if (!success) {
        if (state->wait_kind == VA_WAIT_NOTIFICATION)
            va_logThreadFlagDetail(handle, handle, 0, VA_FLAGS_WAIT_TIMEOUT | state->options, result, state->detail);
        else if (state->wait_kind != VA_WAIT_JOIN)
            va_rtx5_fail(state->object, state->wait_kind, state->detail, state->options, handle, 0);
    }
    if (state->wait_kind == VA_WAIT_MUTEX) {
        /* Cancellation and timeout can demote a non-running owner. The
           object is still live until RTX unblocks its waiters on delete. */
        va_rtx5_mark_priority(((osRtxMutex_t *)state->object)->owner_thread);
    }
    state->object = NULL;
    state->wait_kind = 0;
    va_clearTaskWait(handle);
#else
    (void)handle; (void)success; (void)result;
#endif
}

static inline void va_rtx5_unblocked(void *handle, uint32_t result)
{
#if VA_RTX5_WAITS
    int i = _va_find_task_index(handle);
    if (i < 0) return;
    VA_Rtx5Task *state = &va_rtx5_tasks[i];
    unsigned kind = state->wait_kind;
    if (!kind) return;
    bool success = kind == VA_WAIT_MEM_SLAB ? result != 0 :
        (kind == VA_WAIT_EVENT_FLAGS || kind == VA_WAIT_NOTIFICATION) ? (result & 0x80000000u) == 0 : result == (uint32_t)osOK;
    VA_Rtx5Transfer *transfer = va_rtx5_transfer();
    if (transfer) {
        memset(transfer, 0, sizeof(*transfer));
        if (success) {
            transfer->object = state->object; transfer->kind = (uint8_t)kind; transfer->thread = handle;
        }
    }
    va_rtx5_finish_pending(handle, success, result);
#else
    (void)handle; (void)result;
#endif
}

static inline void va_rtx5_timeout(void *object, unsigned kind)
{
#if VA_RTX5_WAITS
    osRtxThread_t *thread = osRtxInfo.thread.run.curr;
    int i = _va_find_task_index(thread);
    /* WaitEnter can fail without a subsequent Unblocked hook. Timer-list
       expiry is resolved by Unblocked's explicit thread, never by list head. */
    if (i >= 0 && va_rtx5_tasks[i].object == object && va_rtx5_tasks[i].wait_kind == kind &&
        thread->state == osRtxThreadRunning) va_rtx5_finish_pending(thread, false, (uint32_t)osErrorTimeout);
#else
    (void)object; (void)kind;
#endif
}

#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
void EvrRtxMutexCreated(osMutexId_t id, const char *name)
{ VA_RTX5_HOOK(va_rtx5_object(id, name, VA_OBJECT_TYPE_MUTEX)); }
void EvrRtxMutexAcquirePending(osMutexId_t id, uint32_t timeout)
{
    VA_RTX5_HOOK(
        va_rtx5_pending(id, VA_WAIT_MUTEX, timeout, 0);
        va_logQueueObjectBlocking(id);
        va_rtx5_priority(((osRtxMutex_t *)id)->owner_thread, 0);
    );
}
void EvrRtxMutexAcquireTimeout(osMutexId_t id)
{
    VA_RTX5_HOOK(
        va_rtx5_timeout(id, VA_WAIT_MUTEX);
        va_rtx5_priority(((osRtxMutex_t *)id)->owner_thread, 0);
    );
}
void EvrRtxMutexAcquired(osMutexId_t id, uint32_t lock)
{
    VA_RTX5_HOOK(
        osRtxThread_t *owner = ((osRtxMutex_t *)id)->owner_thread;
        va_rtx5_register_thread(owner);
        VA_Rtx5Transfer *transfer = va_rtx5_transfer();
        if (transfer && transfer->object == id && transfer->kind == VA_WAIT_MUTEX)
            memset(transfer, 0, sizeof(*transfer));
        if (lock == 1) {
            va_logSyncActor(id, VA_OBJECT_TYPE_MUTEX, 0, owner, 0);
            int i = _va_find_queue_object_index(id);
            if (i >= 0) va_rtx5_objects[i].owner = owner;
        }
    );
}
void EvrRtxMutexReleased(osMutexId_t id, uint32_t lock)
{
    VA_RTX5_HOOK(
        if (lock == 0) {
            osRtxThread_t *owner = ((osRtxMutex_t *)id)->owner_thread;
            va_logSyncActor(id, VA_OBJECT_TYPE_MUTEX, 1, owner, 0);
            va_rtx5_mark_priority(owner);
            int i = _va_find_queue_object_index(id);
            if (i >= 0) va_rtx5_objects[i].owner = NULL;
        }
    );
}
void EvrRtxMutexNotAcquired(osMutexId_t id)
{ VA_RTX5_HOOK(va_logSyncActor(id, VA_OBJECT_TYPE_MUTEX, 2, osRtxInfo.thread.run.curr, 0)); }
void EvrRtxMutexDelete(osMutexId_t id)
{
    VA_RTX5_HOOK(
        /* API-entry pointers are unvalidated: consult only our registry. */
        int i = _va_find_queue_object_index(id);
        if (i >= 0) va_rtx5_mark_priority(va_rtx5_objects[i].owner);
    );
}
void EvrRtxMutexDestroyed(osMutexId_t id)
{
    VA_RTX5_HOOK(
        int i = _va_find_queue_object_index(id);
        if (i >= 0 && va_rtx5_objects[i].owner) {
            va_logSyncActor(id, VA_OBJECT_TYPE_MUTEX, 1, va_rtx5_objects[i].owner, 0);
            va_rtx5_mark_priority(va_rtx5_objects[i].owner);
        }
        va_logQueueObjectDelete(id);
    );
}
#endif

#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
void EvrRtxSemaphoreCreated(osSemaphoreId_t id, const char *name)
{ VA_RTX5_HOOK(va_rtx5_object(id, name, VA_OBJECT_TYPE_COUNTING_SEM)); }
void EvrRtxSemaphoreAcquirePending(osSemaphoreId_t id, uint32_t timeout)
{ VA_RTX5_HOOK(va_rtx5_pending(id, VA_WAIT_SEMAPHORE, timeout, 0)); }
void EvrRtxSemaphoreAcquireTimeout(osSemaphoreId_t id)
{ VA_RTX5_HOOK(va_rtx5_timeout(id, VA_WAIT_SEMAPHORE)); }
void EvrRtxSemaphoreAcquired(osSemaphoreId_t id, uint32_t tokens)
{
    (void)tokens;
    VA_RTX5_HOOK(
        uint16_t exception;
        osRtxThread_t *actor = va_rtx5_actor(id, VA_WAIT_SEMAPHORE, &exception);
        va_logSyncActor(id, VA_OBJECT_TYPE_COUNTING_SEM, 0, actor, exception);
    );
}
void EvrRtxSemaphoreReleased(osSemaphoreId_t id, uint32_t tokens)
{
    (void)tokens;
    VA_RTX5_HOOK(
        uint16_t exception;
        osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_register_thread(actor);
        va_logSyncActor(id, VA_OBJECT_TYPE_COUNTING_SEM, 1, actor, exception);
    );
}
void EvrRtxSemaphoreNotAcquired(osSemaphoreId_t id)
{
    VA_RTX5_HOOK(
        uint16_t exception;
        osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_logSyncActor(id, VA_OBJECT_TYPE_COUNTING_SEM, 2, actor, exception);
    );
}
void EvrRtxSemaphoreError(osSemaphoreId_t id, int32_t status)
{
    VA_RTX5_HOOK(
        if (status == osRtxErrorSemaphoreCountLimit && _va_find_queue_object_id(id)) {
            uint16_t exception;
            osRtxThread_t *actor = va_rtx5_caller(&exception);
            va_logSyncActor(id, VA_OBJECT_TYPE_COUNTING_SEM, 3, actor, exception);
        }
    );
}
void EvrRtxSemaphoreDestroyed(osSemaphoreId_t id)
{ VA_RTX5_HOOK(va_logQueueObjectDelete(id)); }
#endif

#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
static void va_rtx5_queue(void *id, bool give, bool pending_isr)
{
    uint16_t exception;
    osRtxThread_t *actor = va_rtx5_actor(id, give ? VA_WAIT_QUEUE_SEND : VA_WAIT_QUEUE_RECEIVE, &exception);
    /* ISR sends are recorded when accepted, before PendSV transfers their
       message. A woken blocked sender has a latch and is a new operation. */
    if (__get_IPSR() == 14 && give && !actor) return;
    if (give) va_logQueueObjectGiveTyped(id, VA_OBJECT_TYPE_QUEUE);
    else va_logQueueObjectTakeTyped(id, VA_OBJECT_TYPE_QUEUE);
#if VA_HAS_QUEUE_DETAILS
    /* RTX prioritizes messages. Without a priority in its completed native
       hook, report SEND's unknown ordering instead of inventing FIFO. */
    va_rtx5_operation(id, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS,
        give ? VA_QUEUE_SEND : VA_QUEUE_RECEIVE,
        pending_isr ? UINT32_MAX : ((osRtxMessageQueue_t *)id)->msg_count, 0, actor, exception);
#endif
    (void)pending_isr; (void)actor; (void)exception;
}
void EvrRtxMessageQueueCreated(osMessageQueueId_t id, const char *name)
{
    VA_RTX5_HOOK(
        va_rtx5_object(id, name, VA_OBJECT_TYPE_QUEUE);
#if VA_HAS_QUEUE_DETAILS
        osRtxMessageQueue_t *q = id;
        va_logRtosObjectInfo(id, VA_OBJECT_TYPE_QUEUE, q->mp_info.max_blocks, q->msg_size);
        uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_operation(id, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS, VA_QUEUE_INIT, q->msg_count, 0, actor, exception);
#endif
    );
}
void EvrRtxMessageQueuePutPending(osMessageQueueId_t id, const void *ptr, uint32_t timeout)
{ (void)ptr; VA_RTX5_HOOK(va_rtx5_pending(id, VA_WAIT_QUEUE_SEND, timeout, 0)); }
void EvrRtxMessageQueueGetPending(osMessageQueueId_t id, void *ptr, uint32_t timeout)
{ (void)ptr; VA_RTX5_HOOK(va_rtx5_pending(id, VA_WAIT_QUEUE_RECEIVE, timeout, 0)); }
void EvrRtxMessageQueuePutTimeout(osMessageQueueId_t id)
{ VA_RTX5_HOOK(va_rtx5_timeout(id, VA_WAIT_QUEUE_SEND)); }
void EvrRtxMessageQueueGetTimeout(osMessageQueueId_t id)
{ VA_RTX5_HOOK(va_rtx5_timeout(id, VA_WAIT_QUEUE_RECEIVE)); }
void EvrRtxMessageQueueInserted(osMessageQueueId_t id, const void *ptr)
{ (void)ptr; VA_RTX5_HOOK(va_rtx5_queue(id, true, false)); }
void EvrRtxMessageQueueInsertPending(osMessageQueueId_t id, const void *ptr)
{ (void)ptr; VA_RTX5_HOOK(va_rtx5_queue(id, true, true)); }
void EvrRtxMessageQueueRetrieved(osMessageQueueId_t id, void *ptr)
{
    /* A NULL destination is queue reset discarding a message, not a receive. */
    if (ptr) VA_RTX5_HOOK(va_rtx5_queue(id, false, false));
}
void EvrRtxMessageQueueNotInserted(osMessageQueueId_t id, const void *ptr)
{
    (void)ptr;
    VA_RTX5_HOOK(uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_fail(id, VA_WAIT_QUEUE_SEND, 0, 0, actor, exception));
}
void EvrRtxMessageQueueNotRetrieved(osMessageQueueId_t id, void *ptr)
{
    (void)ptr;
    VA_RTX5_HOOK(uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_fail(id, VA_WAIT_QUEUE_RECEIVE, 0, 0, actor, exception));
}
void EvrRtxMessageQueueResetDone(osMessageQueueId_t id)
{
    VA_RTX5_HOOK(
#if VA_HAS_QUEUE_DETAILS
        uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_operation(id, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS, VA_QUEUE_RESET,
            ((osRtxMessageQueue_t *)id)->msg_count, 0, actor, exception);
#endif
    );
    (void)id;
}
void EvrRtxMessageQueueDestroyed(osMessageQueueId_t id)
{ VA_RTX5_HOOK(va_logQueueObjectDelete(id)); }
#endif

#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
void EvrRtxMemoryPoolCreated(osMemoryPoolId_t id, const char *name)
{
    VA_RTX5_HOOK(
        va_rtx5_object(id, name, VA_OBJECT_TYPE_MEM_SLAB);
#if VA_TRACE_MEM_SLABS
        osRtxMemoryPool_t *pool = id;
        va_logRtosObjectInfo(id, VA_OBJECT_TYPE_MEM_SLAB, pool->mp_info.max_blocks, pool->mp_info.block_size);
#endif
    );
}
void EvrRtxMemoryPoolAllocPending(osMemoryPoolId_t id, uint32_t timeout)
{ VA_RTX5_HOOK(va_rtx5_pending(id, VA_WAIT_MEM_SLAB, timeout, 0)); }
void EvrRtxMemoryPoolAllocTimeout(osMemoryPoolId_t id)
{ VA_RTX5_HOOK(va_rtx5_timeout(id, VA_WAIT_MEM_SLAB)); }
void EvrRtxMemoryPoolAllocated(osMemoryPoolId_t id, void *block)
{
    (void)block;
    VA_RTX5_HOOK(
#if VA_TRACE_MEM_SLABS
        uint16_t exception; osRtxThread_t *actor = va_rtx5_actor(id, VA_WAIT_MEM_SLAB, &exception);
        va_rtx5_operation(id, VA_OBJECT_TYPE_MEM_SLAB, VA_EVENT_MEM_SLAB, VA_OP_ALLOC,
            ((osRtxMemoryPool_t *)id)->mp_info.used_blocks, 0, actor, exception);
#endif
    );
    (void)id;
}
void EvrRtxMemoryPoolDeallocated(osMemoryPoolId_t id, void *block)
{
    (void)block;
    VA_RTX5_HOOK(
#if VA_TRACE_MEM_SLABS
        uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_operation(id, VA_OBJECT_TYPE_MEM_SLAB, VA_EVENT_MEM_SLAB, VA_OP_FREE,
            ((osRtxMemoryPool_t *)id)->mp_info.used_blocks, 0, actor, exception);
#endif
    );
    (void)id;
}
void EvrRtxMemoryPoolAllocFailed(osMemoryPoolId_t id)
{
    VA_RTX5_HOOK(uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_fail(id, VA_WAIT_MEM_SLAB, 0, 0, actor, exception));
}
void EvrRtxMemoryPoolDestroyed(osMemoryPoolId_t id)
{ VA_RTX5_HOOK(va_logQueueObjectDelete(id)); }
#endif

#if VA_TRACE_TIMERS
void EvrRtxKernelErrorNotify(uint32_t code, void *object)
{
    VA_RTX5_HOOK(
        if (code == osRtxErrorTimerQueueOverflow && object != NULL)
            va_logObjectOpFailedTyped(object, VA_OBJECT_TYPE_TIMER, true, code);
    );
}
void EvrRtxTimerCreated(osTimerId_t id, const char *name)
{ VA_RTX5_HOOK(va_rtx5_object(id, name, VA_OBJECT_TYPE_TIMER)); }
static void va_rtx5_timer_request(osTimerId_t id, uint8_t operation)
{
    uint16_t exception;
    void *caller = va_rtx5_caller(&exception);
    va_logTimerRequest(id, operation, caller, exception);
}
void EvrRtxTimerStarted(osTimerId_t id)
{
    VA_RTX5_HOOK(
        osRtxTimer_t *timer = id;
        va_rtx5_timer_request(id, 0);
        va_logTimerArmTicks(id, timer->load,
            (timer->attr & osRtxTimerPeriodic) ? timer->load : 0,
            osRtxConfig.tick_freq, VA_TIMER_CALLBACKS_CAN_REMAIN_QUEUED);
    );
}
void EvrRtxTimerStopped(osTimerId_t id)
{ VA_RTX5_HOOK(va_rtx5_timer_request(id, 3); va_logQueueObjectTakeTyped(id, VA_OBJECT_TYPE_TIMER)); }
void EvrRtxTimerDestroyed(osTimerId_t id)
{ VA_RTX5_HOOK(va_rtx5_timer_request(id, 4); va_logQueueObjectTakeTyped(id, VA_OBJECT_TYPE_TIMER); va_logQueueObjectDelete(id)); }
#if VA_TRACE_TIMER_CALLBACKS
void VA_RTX5_TimerEnter(osTimerId_t id)
{
    VA_RTX5_HOOK(
        if (id && _va_find_queue_object_id(id) != 0 && ((osRtxTimer_t *)id)->id == osRtxIdTimer) {
            va_logTimerCallback(id, true, false, (void *)((osRtxTimer_t *)id)->finfo.func);
        }
    );
}
void VA_RTX5_TimerExit(osTimerId_t id)
{ VA_RTX5_HOOK(if (id && _va_find_queue_object_id(id) != 0 && ((osRtxTimer_t *)id)->id == osRtxIdTimer) va_logTimerCallback(id, false, false, (void *)((osRtxTimer_t *)id)->finfo.func)); }
#endif
#endif

#if VA_TRACE_RTOS_HEAPS
static void va_rtx5_heap(void *mem)
{
    int i = _va_find_queue_object_index(mem);
    if (i < 0) return;
    /* RTX 5.9.1's LIVE allocator header is two uint32_t words: size, used.
       Freed block headers have already been unlinked; never inspect them. */
    uint32_t header[2]; memcpy(header, mem, sizeof(header));
    uint32_t before = va_rtx5_objects[i].used;
    /* The shared heap event carries total allocated bytes, never a delta. */
    if (header[1] > before) va_logHeapAlloc(mem, header[1]);
    else if (header[1] < before) va_logHeapFree(mem, header[1]);
    va_rtx5_objects[i].used = header[1];
}
void EvrRtxMemoryInit(void *mem, uint32_t size, uint32_t result)
{
    if (result) VA_RTX5_HOOK(
        va_rtx5_object(mem, "RTX heap", VA_OBJECT_TYPE_HEAP);
        va_logHeapCapacity(mem, "RTX heap", size); va_rtx5_heap(mem);
    );
}
void EvrRtxMemoryAlloc(void *mem, uint32_t size, uint32_t type, void *block)
{
    (void)type;
    VA_RTX5_HOOK(
        if (block) va_rtx5_heap(mem);
        else if (_va_find_queue_object_id(mem)) va_logHeapAllocFailed(mem, size);
    );
}
void EvrRtxMemoryFree(void *mem, void *block, uint32_t result)
{ (void)block; if (result) VA_RTX5_HOOK(va_rtx5_heap(mem)); }
#endif

#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_NOTIFICATIONS
/* Nonblocking API entries can be preempted before SVC entry. Key requests by
   their calling thread, or the exact IRQ, rather than one global "last set".
   Exhaustion is explicitly reported; it never borrows another caller's mask. */
static inline VA_Rtx5FlagRequest *va_rtx5_flag_request(void *object, bool create, bool thread_flags)
{
    uint16_t exception;
    osRtxThread_t *thread = va_rtx5_caller(&exception);
    VA_Rtx5FlagRequest *empty = NULL;
    for (unsigned i = 0; i < VA_RTX5_FLAG_REQUESTS; ++i) {
        VA_Rtx5FlagRequest *request = &va_rtx5_flag_requests[i];
        if (!request->object) { if (!empty) empty = request; continue; }
        if (request->thread == thread && request->exception == exception &&
            (create || (request->object == object && request->thread_flags == thread_flags))) return request;
    }
    if (create && empty) return empty;
    if (create) _va_send_setup_packet(VA_SETUP_INFO, 0, "ERR:RTX5_FLAGS_FULL");
    return NULL;
}

static void va_rtx5_flag_begin(void *object, uint32_t mask, unsigned operation, bool thread_flags)
{
    VA_Rtx5FlagRequest *request = va_rtx5_flag_request(object, true, thread_flags);
    if (!request) return;
    memset(request, 0, sizeof(*request));
    request->thread_flags = thread_flags;
    request->object = object; request->mask = mask; request->operation = (uint8_t)operation;
    request->thread = va_rtx5_caller(&request->exception);
    va_rtx5_register_thread(request->thread);
}

static void va_rtx5_flag_commit(void *object, bool final, bool thread_flags)
{
    VA_Rtx5FlagRequest *request = va_rtx5_flag_request(object, false, thread_flags);
    if (request && !request->emitted) {
        if (thread_flags) {
            va_rtx5_register_thread(object);
            va_logThreadFlagDetail(object, request->thread, request->exception,
                request->operation, request->mask, request->mask);
        } else {
            if (request->operation == VA_FLAGS_SET) va_logEventFlagSet(object, request->mask);
#if VA_HAS_EVENT_FLAG_DETAILS
            va_rtx5_operation(object, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
                request->operation, request->mask, request->mask, request->thread, request->exception);
#endif
        }
        request->emitted = true;
    }
    if (final) {
        if (thread_flags) {
            uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
            va_rtx5_register_thread(object);
            va_logThreadFlagDetail(object, actor, exception, VA_FLAGS_SNAPSHOT,
                ((osRtxThread_t *)object)->thread_flags, 0);
        } else {
#if VA_HAS_EVENT_FLAG_DETAILS
            uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
            va_rtx5_operation(object, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
                VA_FLAGS_SNAPSHOT, ((osRtxEventFlags_t *)object)->event_flags, 0, actor, exception);
#endif
        }
        if (request) memset(request, 0, sizeof(*request));
    }
}
#endif

#if VA_TRACE_EVENT_FLAGS
void EvrRtxEventFlagsSet(osEventFlagsId_t id, uint32_t flags)
{ VA_RTX5_HOOK(va_rtx5_flag_begin(id, flags, VA_FLAGS_SET, false)); }
void EvrRtxEventFlagsClear(osEventFlagsId_t id, uint32_t flags)
{ VA_RTX5_HOOK(va_rtx5_flag_begin(id, flags, VA_FLAGS_CLEAR, false)); }
void EvrRtxEventFlagsSetDone(osEventFlagsId_t id, uint32_t flags)
{ (void)flags; VA_RTX5_HOOK(va_rtx5_flag_commit(id, true, false)); }
void EvrRtxEventFlagsClearDone(osEventFlagsId_t id, uint32_t flags)
{ (void)flags; VA_RTX5_HOOK(va_rtx5_flag_commit(id, true, false)); }
void EvrRtxEventFlagsError(osEventFlagsId_t id, int32_t status)
{
    (void)status;
    VA_RTX5_HOOK(
        VA_Rtx5FlagRequest *request = va_rtx5_flag_request(id, false, false);
        if (request) memset(request, 0, sizeof(*request));
    );
}
#endif

#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES || VA_TRACE_TASK_NOTIFICATIONS
static inline uint8_t va_rtx5_flag_options(uint32_t options)
{
    return ((options & osFlagsWaitAll) ? VA_FLAGS_ALL : 0) |
        ((options & osFlagsNoClear) ? 0 : VA_FLAGS_CLEAR_ON_EXIT);
}
#endif
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES
void EvrRtxEventFlagsCreated(osEventFlagsId_t id, const char *name)
{
    VA_RTX5_HOOK(
        va_rtx5_object(id, name, VA_OBJECT_TYPE_EVENTFLAG);
#if VA_HAS_EVENT_FLAG_DETAILS
        uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_operation(id, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
            VA_FLAGS_SNAPSHOT, ((osRtxEventFlags_t *)id)->event_flags, 0, actor, exception);
#endif
    );
}
void EvrRtxEventFlagsWaitPending(osEventFlagsId_t id, uint32_t flags, uint32_t options, uint32_t timeout)
{
    VA_RTX5_HOOK(
        va_rtx5_pending(id, VA_WAIT_EVENT_FLAGS, flags, va_rtx5_flag_options(options));
#if VA_HAS_EVENT_FLAG_DETAILS
        va_rtx5_operation(id, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
            VA_FLAGS_WAIT_BEGIN | va_rtx5_flag_options(options), ((osRtxEventFlags_t *)id)->event_flags,
            flags, osRtxInfo.thread.run.curr, 0);
#endif
    );
    (void)timeout;
}
void EvrRtxEventFlagsWaitTimeout(osEventFlagsId_t id)
{ VA_RTX5_HOOK(va_rtx5_timeout(id, VA_WAIT_EVENT_FLAGS)); }
void EvrRtxEventFlagsWaitCompleted(osEventFlagsId_t id, uint32_t flags, uint32_t options, uint32_t value)
{
    VA_RTX5_HOOK(
#if VA_TRACE_EVENT_FLAGS
        /* First successful waiter proves the Set succeeded. Emit its source
           before consumption, even though RTX emits SetDone after waiters. */
        va_rtx5_flag_commit(id, false, false);
#endif
        uint16_t exception; osRtxThread_t *actor = va_rtx5_actor(id, VA_WAIT_EVENT_FLAGS, &exception);
        va_logEventFlagWaitEnd(id, value);
#if VA_HAS_EVENT_FLAG_DETAILS
        va_rtx5_operation(id, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
            VA_FLAGS_WAIT_OK | va_rtx5_flag_options(options), value, flags, actor, exception);
        va_rtx5_operation(id, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
            VA_FLAGS_SNAPSHOT, ((osRtxEventFlags_t *)id)->event_flags, 0, actor, exception);
#endif
        (void)actor; (void)exception;
    );
    (void)flags; (void)options;
}
void EvrRtxEventFlagsWaitNotCompleted(osEventFlagsId_t id, uint32_t flags, uint32_t options)
{
    VA_RTX5_HOOK(
        uint16_t exception; osRtxThread_t *actor = va_rtx5_caller(&exception);
        va_rtx5_fail(id, VA_WAIT_EVENT_FLAGS, flags, va_rtx5_flag_options(options), actor, exception);
    );
}
void EvrRtxEventFlagsDestroyed(osEventFlagsId_t id)
{ VA_RTX5_HOOK(va_logQueueObjectDelete(id)); }
#endif

#if VA_TRACE_TASK_STATES
void EvrRtxThreadJoinPending(osThreadId_t id)
{
    VA_RTX5_HOOK(
        va_rtx5_register_thread(id);
        va_rtx5_pending(NULL, VA_WAIT_JOIN, _va_find_task_id(id), 0);
    );
}
#endif

#if VA_TRACE_TASK_NOTIFICATIONS
void EvrRtxThreadFlagsSet(osThreadId_t id, uint32_t flags)
{ VA_RTX5_HOOK(va_rtx5_flag_begin(id, flags, VA_FLAGS_SET, true)); }
void EvrRtxThreadFlagsSetDone(osThreadId_t id, uint32_t flags)
{ (void)flags; VA_RTX5_HOOK(va_rtx5_flag_commit(id, true, true)); }
void EvrRtxThreadFlagsClear(uint32_t flags)
{ VA_RTX5_HOOK(va_rtx5_flag_begin(osRtxInfo.thread.run.curr, flags, VA_FLAGS_CLEAR, true)); }
void EvrRtxThreadFlagsClearDone(uint32_t flags)
{ (void)flags; VA_RTX5_HOOK(va_rtx5_flag_commit(osRtxInfo.thread.run.curr, true, true)); }
void EvrRtxThreadFlagsError(osThreadId_t id, int32_t status)
{
    (void)id; (void)status;
    VA_RTX5_HOOK(
        /* Error hooks can report NULL instead of the API's target. Retire
           this caller's request, never inspect the unvalidated argument. */
        uint16_t exception; osRtxThread_t *thread = va_rtx5_caller(&exception);
        for (unsigned i = 0; i < VA_RTX5_FLAG_REQUESTS; ++i) {
            VA_Rtx5FlagRequest *request = &va_rtx5_flag_requests[i];
            if (request->thread_flags && request->thread == thread && request->exception == exception)
                memset(request, 0, sizeof(*request));
        }
    );
}
#endif

#if VA_TRACE_TASK_NOTIFICATIONS || VA_TRACE_TASK_STATES
void EvrRtxThreadFlagsWaitPending(uint32_t flags, uint32_t options, uint32_t timeout)
{
    VA_RTX5_HOOK(
        osRtxThread_t *thread = osRtxInfo.thread.run.curr;
        va_rtx5_pending(NULL, VA_WAIT_NOTIFICATION, flags, va_rtx5_flag_options(options));
        va_logThreadFlagDetail(thread, thread, 0, VA_FLAGS_WAIT_BEGIN | va_rtx5_flag_options(options),
            thread->thread_flags, flags);
    );
    (void)timeout;
}
void EvrRtxThreadFlagsWaitTimeout(osThreadId_t id)
{ VA_RTX5_HOOK(va_rtx5_finish_pending(id, false, (uint32_t)osErrorTimeout)); }
void EvrRtxThreadFlagsWaitCompleted(uint32_t flags, uint32_t options, uint32_t value, osThreadId_t id)
{
    VA_RTX5_HOOK(
        va_rtx5_register_thread(id);
#if VA_TRACE_TASK_NOTIFICATIONS
        /* SetDone follows this hook for a task sender. ISR SetDone has
           already committed before PendSV wakes the explicit recipient. */
        va_rtx5_flag_commit(id, false, true);
#endif
        va_logThreadFlagDetail(id, id, 0, VA_FLAGS_WAIT_OK | va_rtx5_flag_options(options), value, flags);
        va_logThreadFlagDetail(id, id, 0, VA_FLAGS_SNAPSHOT, ((osRtxThread_t *)id)->thread_flags, 0);
        va_rtx5_finish_pending(id, true, value);
    );
}
void EvrRtxThreadFlagsWaitNotCompleted(uint32_t flags, uint32_t options)
{
    VA_RTX5_HOOK(
        osRtxThread_t *thread = osRtxInfo.thread.run.curr;
        va_rtx5_register_thread(thread);
        va_logThreadFlagDetail(thread, thread, 0, VA_FLAGS_WAIT_TIMEOUT | va_rtx5_flag_options(options),
            (uint32_t)osErrorResource, flags);
    );
}
#endif

static inline void va_rtx5_flags_forget_thread(void *thread)
{
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_NOTIFICATIONS
    for (unsigned i = 0; i < VA_RTX5_FLAG_REQUESTS; ++i) {
        if (va_rtx5_flag_requests[i].thread == thread && va_rtx5_flag_requests[i].exception == 0)
            memset(&va_rtx5_flag_requests[i], 0, sizeof(va_rtx5_flag_requests[i]));
    }
#else
    (void)thread;
#endif
}

#endif
