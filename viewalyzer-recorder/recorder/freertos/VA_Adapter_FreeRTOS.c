/**
 * @file VA_Adapter_FreeRTOS.c
 * @brief ViewAlyzer FreeRTOS Adapter - RTOS-specific logic
 *
 * Contains everything that depends on FreeRTOS internals:
 *   - Queue-type detection (QueueDefinitionMirror hack)
 *   - Exact word-wise stack-usage calculation, with RTOS API fallback
 *   - Mutex-contention detection via xSemaphoreGetMutexHolder
 *
 * This file is compiled ONLY when VA_RTOS_SELECT == VA_RTOS_FREERTOS.
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

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_FREERTOS)

#include "VA_Internal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "list.h"
#include "queue.h"
#if defined(INCLUDE_xSemaphoreGetMutexHolder) || defined(INCLUDE_xQueueGetMutexHolder)
#include "semphr.h"
#endif
#if VA_TRACE_TIMERS
#include "timers.h"
#include <string.h>
#endif

#if VA_NEEDS_OBJECT_REGISTRY && (configUSE_TRACE_FACILITY != 1)
#warning "ViewAlyzer: set configUSE_TRACE_FACILITY to 1 in FreeRTOSConfig.h. Without it Queue_t has no ucQueueType field, so mutexes and semaphores cannot be told apart and per-category filtering degrades to 'item size 0 means binary semaphore'."
#endif

/* ── Queue-type detection - mirrors the private FreeRTOS Queue_t layout ─── */

#if VA_NEEDS_OBJECT_REGISTRY
VA_QueueObjectType_t va_adapter_get_queue_object_type(void *handle)
{
    if (handle == NULL)
        return VA_OBJECT_TYPE_QUEUE;

    typedef struct
    {
        int8_t *pcTail;
        int8_t *pcReadFrom;
    } QueuePointers_t;

    typedef struct
    {
        TaskHandle_t xMutexHolder;
        UBaseType_t uxRecursiveCallCount;
    } SemaphoreData_t;

    typedef struct QueueDefinitionMirror
    {
        int8_t *pcHead;
        int8_t *pcWriteTo;
        union
        {
            QueuePointers_t xQueue;
            SemaphoreData_t xSemaphore;
        } u;
        List_t xTasksWaitingToSend;
        List_t xTasksWaitingToReceive;
        volatile UBaseType_t uxMessagesWaiting;
        UBaseType_t uxLength;
        UBaseType_t uxItemSize;
        volatile int8_t cRxLock;
        volatile int8_t cTxLock;
#if ((configSUPPORT_STATIC_ALLOCATION == 1) && (configSUPPORT_DYNAMIC_ALLOCATION == 1))
        uint8_t ucStaticallyAllocated;
#endif
#if (configUSE_QUEUE_SETS == 1)
        struct QueueDefinitionMirror *pxQueueSetContainer;
#endif
#if (configUSE_TRACE_FACILITY == 1)
        UBaseType_t uxQueueNumber;
        uint8_t ucQueueType;
#endif
    } QueueDefinitionMirror;

    QueueDefinitionMirror *pxQueue = (QueueDefinitionMirror *)handle;

    if (pxQueue->pcHead == NULL)
    {
        return VA_OBJECT_TYPE_MUTEX;
    }

#if (configUSE_TRACE_FACILITY == 1)
    return (VA_QueueObjectType_t)(pxQueue->ucQueueType);
#else
    if (pxQueue->uxItemSize == 0)
    {
        return VA_OBJECT_TYPE_BINARY_SEM;
    }
    return VA_OBJECT_TYPE_QUEUE;
#endif
}
#endif /* VA_NEEDS_OBJECT_REGISTRY */

/* ── Stack usage ─────────────────────────────────────────────────── */

#if VA_TRACE_STACK_USAGE
/* Adapters report stack usage in BYTES; FreeRTOS accounts in StackType_t
   words internally, so the conversion happens at this boundary. */
uint32_t va_adapter_calculate_stack_usage(void *taskHandle)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    int idx = _va_find_task_index(taskHandle);
    if (idx < 0 || taskMap[idx].ulStackDepth == 0) return UINT32_MAX;
    uint32_t total_words = taskMap[idx].ulStackDepth / (uint32_t)sizeof(StackType_t);
    uint32_t free_stack_words;
#if (portSTACK_GROWTH < 0)
    /* Scan standard FreeRTOS fill within known bounds, in whole stack words. */
    if (sizeof(StackType_t) == sizeof(uint32_t) && idx >= 0 &&
        taskMap[idx].pxStack != NULL && taskMap[idx].ulStackDepth > 0 &&
        ((uintptr_t)taskMap[idx].pxStack % sizeof(uint32_t)) == 0)
    {
        const uint8_t *stack = (const uint8_t *)taskMap[idx].pxStack;
        free_stack_words = 0;
        while (free_stack_words < total_words)
        {
            uint32_t fill;
            memcpy(&fill, stack + free_stack_words * sizeof(fill), sizeof(fill));
            if (fill != UINT32_C(0xa5a5a5a5))
                break;
            ++free_stack_words;
        }
    }
    else
#endif
        free_stack_words = uxTaskGetStackHighWaterMark((TaskHandle_t)taskHandle);
    if (free_stack_words > total_words) return UINT32_MAX;
    return (total_words - free_stack_words) * (uint32_t)sizeof(StackType_t);
#else
    (void)taskHandle;
    return 0;
#endif
}

uint32_t va_adapter_get_total_stack_size(void *taskHandle)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0)
    {
        return taskMap[idx].ulStackDepth;
    }
    return 0;
#else
    /* No high-water mark available: return 0 so no stack packet is emitted
       at all, instead of confidently reporting 0 bytes used. */
    (void)taskHandle;
    return 0;
#endif
}
#endif /* VA_TRACE_STACK_USAGE */

/* ── Sleep (traceTASK_DELAY / suspend / resume) ──────────────────── */

#if VA_TRACE_SLEEP || VA_TRACE_TASK_STATES
/* The sleeping flag makes enter/exit idempotent: a resume of a task that
   never slept emits nothing, and a resumed task's switch-in does not emit
   a second exit. */
void va_freertos_sleep_enter(void *taskHandle)
{
    if (!VA_IsInit() || taskHandle == NULL)
        return;
#if VA_TRACE_SLEEP
    bool emit = false;
    VA_CS_ENTER();
    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0 && !taskMap[idx].sleeping)
    {
        taskMap[idx].sleeping = true;
        emit = true;
    }
    VA_CS_EXIT();
    if (emit)
        va_logSleepEnter(taskHandle);
#endif
}

void va_freertos_sleep_exit(void *taskHandle)
{
    if (!VA_IsInit() || taskHandle == NULL)
        return;
#if VA_TRACE_SLEEP
    bool emit = false;
    VA_CS_ENTER();
    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0 && taskMap[idx].sleeping)
    {
        taskMap[idx].sleeping = false;
        emit = true;
    }
    VA_CS_EXIT();
    if (emit)
        va_logSleepExit(taskHandle);
#endif
}
#endif /* VA_TRACE_SLEEP */

/* ── Task switch-in with lazy registration ───────────────────────── */

#if VA_NEEDS_SWITCH_HOOK
/* The kernel-compiled switch hook captures native fields before entering
   here. No locking FreeRTOS getter is called in the scheduler path. */
void va_freertos_taskswitchedin(void *taskHandle)
{
#if VA_TRACE_SLEEP
    /* A delayed task waking up: close its sleep before the switch event. */
    va_freertos_sleep_exit(taskHandle);
#endif
    va_taskswitchedin(taskHandle);
}
#endif /* VA_NEEDS_SWITCH_HOOK */

#if VA_NEEDS_TASK_REGISTRY
bool va_freertos_task_needs_metadata(void *taskHandle)
{
    if (!VA_IsInit() || taskHandle == NULL) return false;
    int idx = _va_find_task_index(taskHandle);
    return idx < 0 || !taskMap[idx].hasNativeTaskMetadata;
}

/* Scratch fields are read directly from the TCB by the kernel hook. A task
   lazily named by an object operation must also get its actual priority and
   bounds when it next reaches a scheduler hook. */
void va_freertos_task_metadata(void *taskHandle, const char *name)
{
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    va_taskcreated(taskHandle, name);
    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0) {
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
        taskMap[idx].uxPriority = g_task_uxPriority;
        taskMap[idx].uxBasePriority = g_task_uxBasePriority;
#endif
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES || VA_TRACE_STACK_USAGE
        taskMap[idx].ulStackDepth = g_task_ulStackDepth;
#endif
#if VA_TRACE_STACK_USAGE
        taskMap[idx].pxStack = (void *)g_task_pxStack;
        taskMap[idx].pxEndOfStack = (void *)g_task_pxEndOfStack;
        /* Newly learned bounds allow the first real sample immediately. */
        taskMap[idx].hasStackSample = false;
#endif
        taskMap[idx].hasNativeTaskMetadata = true;
    }
    VA_CS_EXIT();
}
#endif

/* ── Software timers ─────────────────────────────────────────────── */

#if VA_TRACE_TIMERS
/* A timer created before VA_Init lost its registration when VA_Init reset
   the registry; re-register it by name before its first event. */
static void va_freertos_timer_ensure_registered(void *timer)
{
    if (VA_IsInit() && _va_find_queue_object_id(timer) == 0)
        va_logQueueObjectCreateTyped(timer, pcTimerGetName((TimerHandle_t)timer),
                                     VA_OBJECT_TYPE_TIMER);
}

void va_freertos_timer_request(void *timer, int32_t commandId, int32_t accepted)
{
    if (!VA_IsInit() || accepted != pdPASS) return;
    uint8_t operation;
    switch (commandId) {
    case tmrCOMMAND_START: operation = 0; break;
    case tmrCOMMAND_RESET: operation = 1; break;
    case tmrCOMMAND_CHANGE_PERIOD: operation = 2; break;
    case tmrCOMMAND_STOP: operation = 3; break;
    case tmrCOMMAND_DELETE: operation = 4; break;
#if defined(tmrCOMMAND_START_FROM_ISR)
    case tmrCOMMAND_START_FROM_ISR: operation = 0; break;
    case tmrCOMMAND_RESET_FROM_ISR: operation = 1; break;
    case tmrCOMMAND_CHANGE_PERIOD_FROM_ISR: operation = 2; break;
    case tmrCOMMAND_STOP_FROM_ISR: operation = 3; break;
#endif
    default: return; /* daemon-internal catch-up is not a user request */
    }
    uint16_t exception = (uint16_t)__get_IPSR();
    va_logTimerRequest(timer, operation,
        exception == 0 && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED
            ? (void *)xTaskGetCurrentTaskHandle() : NULL, exception);
}

/* Called when the daemon processes a command, not when a caller queues it.
   Start/reset deadlines use the command's tick stamp; change-period uses
   the processing tick. No ISR-unsafe reload API is called from an ISR. */
void va_freertos_timer_command(void *timer, int32_t commandId, uint32_t optionalValue,
                               uint32_t periodTicks)
{
    if (!VA_IsInit() || timer == NULL) return;
    va_freertos_timer_ensure_registered(timer);
    /* Kernel getters may exit a FreeRTOS critical section by enabling IRQs
       on PRIMASK ports. Read them before the packet builder takes VA_CS. */
    switch (commandId) {
    case tmrCOMMAND_START:
#if defined(tmrCOMMAND_START_DONT_TRACE)
    /* Older kernels queue this behind user commands during catch-up. It can
       re-arm even after STOP; expose actual processing without a user request. */
    case tmrCOMMAND_START_DONT_TRACE:
#endif
    case tmrCOMMAND_RESET:
    case tmrCOMMAND_CHANGE_PERIOD:
#if defined(tmrCOMMAND_START_FROM_ISR)
    case tmrCOMMAND_START_FROM_ISR:
    case tmrCOMMAND_RESET_FROM_ISR:
    case tmrCOMMAND_CHANGE_PERIOD_FROM_ISR:
#endif
    {
        bool change = commandId == tmrCOMMAND_CHANGE_PERIOD;
#if defined(tmrCOMMAND_CHANGE_PERIOD_FROM_ISR)
        change = change || commandId == tmrCOMMAND_CHANGE_PERIOD_FROM_ISR;
#endif
        uint32_t period = change ? optionalValue : periodTicks;
        uint8_t flags = VA_TIMER_EVERY_EXPIRY_TRACED | (change ? VA_TIMER_REQUEST_HOOK : 0);
        uint32_t reload = 0;
        /* This API was introduced in 10.3. The version macros are available
           here, after task.h, unlike when FreeRTOSConfig includes the hooks. */
#if (tskKERNEL_VERSION_MAJOR > 10) || (tskKERNEL_VERSION_MAJOR == 10 && tskKERNEL_VERSION_MINOR >= 3)
        if (uxTimerGetReloadMode((TimerHandle_t)timer)) reload = period;
#else
        flags |= VA_TIMER_RELOAD_UNKNOWN;
#endif
#if (tskKERNEL_VERSION_MAJOR < 10) || (tskKERNEL_VERSION_MAJOR == 10 && (tskKERNEL_VERSION_MINOR < 4 || (tskKERNEL_VERSION_MINOR == 4 && tskKERNEL_VERSION_BUILD < 4)))
        /* Internal reloads can remain behind a user reset/stop in the daemon
           queue. Once overdue commands interleave, association is uncertain. */
        if (reload != 0 || (flags & VA_TIMER_RELOAD_UNKNOWN) != 0)
            flags |= VA_TIMER_CALLBACKS_CAN_REMAIN_QUEUED;
#endif
        int64_t remaining = period;
        if (!change) {
            /* TickType_t subtraction deliberately preserves tick wrap. */
            TickType_t elapsed = (TickType_t)(xTaskGetTickCount() - (TickType_t)optionalValue);
            remaining -= (int64_t)elapsed;
        }
        va_logTimerArmTicks(timer, remaining, reload, configTICK_RATE_HZ, flags);
        break;
    }
    case tmrCOMMAND_STOP:
#if defined(tmrCOMMAND_STOP_FROM_ISR)
    case tmrCOMMAND_STOP_FROM_ISR:
#endif
        va_logQueueObjectTakeTyped(timer, VA_OBJECT_TYPE_TIMER);
        break;
    case tmrCOMMAND_DELETE: {
        VA_CS_ENTER();
        va_logQueueObjectTakeTyped(timer, VA_OBJECT_TYPE_TIMER);
        va_logQueueObjectDelete(timer);
        VA_CS_EXIT();
        break;
    }
    default: break;
    }
}

void va_freertos_timer_expired(void *timer, void *handler, const char *hookFunction)
{
    if (!VA_IsInit() || timer == NULL) return;
    va_freertos_timer_ensure_registered(timer);
    uint8_t kind = 5; /* vendor-patched hook position/order is unknown */
#if (tskKERNEL_VERSION_MAJOR < 10) || (tskKERNEL_VERSION_MAJOR == 10 && (tskKERNEL_VERSION_MINOR < 4 || (tskKERNEL_VERSION_MINOR == 4 && tskKERNEL_VERSION_BUILD < 4)))
    /* Through 10.4.3 only the overdue command path calls the handler BEFORE
       traceTIMER_EXPIRED. Normal expiry and tick-wrap hooks precede it. */
    if (strcmp(hookFunction, "prvProcessReceivedCommands") == 0) kind = 3;
    else if (strcmp(hookFunction, "prvProcessExpiredTimer") == 0 ||
             strcmp(hookFunction, "prvSwitchTimerLists") == 0) kind = 2;
#else
    /* Since 10.4.4, reload catch-up callbacks precede the original expiry.
       Keep their entry timestamps, but do not assign chronological ordinals. */
    if (strcmp(hookFunction, "prvReloadTimer") == 0) kind = 4;
    else if (strcmp(hookFunction, "prvProcessExpiredTimer") == 0 ||
             strcmp(hookFunction, "prvProcessReceivedCommands") == 0) kind = 2;
#endif
    va_logTimerExpiryObserved(timer, handler, kind);
}
#endif /* VA_TRACE_TIMERS */

/* ── Kernel heap (traceMALLOC / traceFREE) ───────────────────────── */

#if VA_TRACE_RTOS_HEAPS
/* The allocator suspends scheduling around both hooks. Read its free-byte
   counter before taking VA_CS; never walk the free list from traceFREE,
   because heap_4/5 have not yet inserted the freed block. Keep this cache
   before VA_Init too, so early allocations are part of the first snapshot. */
static uint8_t s_va_heap_sentinel;
#if VA_FREERTOS_HEAP_SCHEME != 0 && VA_FREERTOS_HEAP_SCHEME != 3
static size_t s_va_heap_capacity;
static bool s_va_heap_seen;
static bool s_va_heap_capacity_known;
static bool s_va_heap_invalid;
static size_t s_va_heap_live_allocations;

#if VA_FREERTOS_HEAP_SCHEME == 5
void VA_FreeRTOSDefineHeapRegions(const HeapRegion_t *regions)
{
    /* Use instead of vPortDefineHeapRegions, before any allocation. The
       kernel enforces that ordering and computes alignment/end-marker loss. */
    vPortDefineHeapRegions(regions);
    size_t capacity = xPortGetFreeHeapSize();
    VA_CS_ENTER();
    s_va_heap_capacity = capacity;
    s_va_heap_capacity_known = capacity > 0 && capacity <= UINT32_MAX;
    s_va_heap_seen = true;
    VA_CS_EXIT();
}
#endif

static bool va_freertos_heap_snapshot(void *address, uint32_t size, bool allocation,
                                      uint32_t *used, uint32_t *capacity)
{
    size_t free_bytes = xPortGetFreeHeapSize();
    (void)size;
    VA_CS_ENTER();
#if VA_FREERTOS_HEAP_SCHEME != 5
    if (!s_va_heap_seen && allocation) {
        /* A fresh single-region heap has one free block. If an allocation
           leaves free space it was split, so its hook size is exact. A first
           whole-block allocation is ambiguous until the heap is empty again. */
        if (address == NULL || free_bytes > 0) {
            s_va_heap_capacity = free_bytes + (address ? size : 0);
            s_va_heap_capacity_known = s_va_heap_capacity > 0 && s_va_heap_capacity >= free_bytes && s_va_heap_capacity <= UINT32_MAX;
        }
    }
#endif
    s_va_heap_seen = true;
    if (address) {
        if (allocation) {
            if (s_va_heap_live_allocations == SIZE_MAX) s_va_heap_invalid = true;
            else ++s_va_heap_live_allocations;
        } else if (s_va_heap_live_allocations > 0) {
            --s_va_heap_live_allocations;
            if (s_va_heap_live_allocations == 0 && !s_va_heap_capacity_known) {
                s_va_heap_capacity = free_bytes;
                s_va_heap_capacity_known = free_bytes > 0 && free_bytes <= UINT32_MAX;
            }
        } else s_va_heap_invalid = true;
    }
    if (s_va_heap_capacity_known && free_bytes > s_va_heap_capacity) s_va_heap_invalid = true;
    bool known = s_va_heap_capacity_known && !s_va_heap_invalid;
    *capacity = known ? (uint32_t)s_va_heap_capacity : 0;
    *used = known ? (uint32_t)(s_va_heap_capacity - free_bytes) : 0;
    VA_CS_EXIT();
    return known;
}
#endif

static void va_freertos_heap_ensure_registered(uint32_t capacity)
{
    /* Looking up the registry also handles VA_Init resetting it. Capacity 0
       means unknown, never an empty heap or configTOTAL_HEAP_SIZE. */
    int index = _va_find_queue_object_index(&s_va_heap_sentinel);
    if (index < 0 || queueObjectMap[index].heapCapacity != capacity)
        va_logHeapCapacity(&s_va_heap_sentinel, "FreeRTOSHeap", capacity);
}

void va_freertos_heap_alloc(void *address, uint32_t size)
{
    uint32_t used = 0, capacity = 0;
    bool known = false;
#if VA_FREERTOS_HEAP_SCHEME != 0 && VA_FREERTOS_HEAP_SCHEME != 3
    known = va_freertos_heap_snapshot(address, size, true, &used, &capacity);
#endif
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    va_freertos_heap_ensure_registered(capacity);
    if (address == NULL) {
        /* Some kernels replace overflowing requests with size 0. Preserve
           the failure; the payload is the kernel hook size, not user bytes. */
        va_logHeapAllocFailed(&s_va_heap_sentinel, size);
    } else if (known) va_logHeapAlloc(&s_va_heap_sentinel, used);
    VA_CS_EXIT();
}

void va_freertos_heap_free(void *address, uint32_t size)
{
    if (address == NULL) return;
    uint32_t used = 0, capacity = 0;
    bool known = false;
    (void)size;
#if VA_FREERTOS_HEAP_SCHEME != 0 && VA_FREERTOS_HEAP_SCHEME != 3
    known = va_freertos_heap_snapshot(address, size, false, &used, &capacity);
#endif
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    va_freertos_heap_ensure_registered(capacity);
    if (known) va_logHeapFree(&s_va_heap_sentinel, used);
    VA_CS_EXIT();
}
#endif /* VA_TRACE_RTOS_HEAPS */

/* ── Mutex contention detection ──────────────────────────────────── */

#if VA_NEEDS_BLOCKING_HOOK
void va_adapter_check_mutex_contention(void *queueObject, uint16_t queue_va_id)
{
#if ((defined(INCLUDE_xSemaphoreGetMutexHolder) && (INCLUDE_xSemaphoreGetMutexHolder == 1)) || \
     (defined(INCLUDE_xQueueGetMutexHolder) && (INCLUDE_xQueueGetMutexHolder == 1)))
    {
        TaskHandle_t holder = NULL;
#if (defined(INCLUDE_xSemaphoreGetMutexHolder) && (INCLUDE_xSemaphoreGetMutexHolder == 1))
        holder = xSemaphoreGetMutexHolder((QueueHandle_t)queueObject);
#else
        holder = xQueueGetMutexHolder((QueueHandle_t)queueObject);
#endif
        if (holder != NULL)
        {
            TaskHandle_t current = xTaskGetCurrentTaskHandle();
            if (holder != current)
            {
                uint16_t holder_id = _va_find_task_id((void *)holder);
                uint16_t waiter_id = _va_find_task_id((void *)current);
                if (holder_id != 0 && waiter_id != 0)
                {
                    _va_send_mutex_contention_packet(queue_va_id, waiter_id, holder_id, _va_get_timestamp());
                }
            }
        }
    }
#else
    (void)queueObject;
    (void)queue_va_id;
#endif
}
#endif /* VA_NEEDS_BLOCKING_HOOK */

/* Nothing above may have survived the category selection; give the
   translation unit something so strict toolchains do not warn about an
   empty object file. */
const char va_adapter_freertos_present = 1;

#if VA_TRACE_TASK_STATES
void va_freertos_task_ready(void *task, const char *name, uint32_t priority, uint32_t base)
{
    if (!VA_IsInit()) return;
    VA_CS_ENTER();
    int idx = _va_find_task_index(task);
    if (idx < 0) {
        g_task_pxStack = NULL;
        g_task_pxEndOfStack = NULL;
        g_task_ulStackDepth = 0;
        g_task_uxPriority = priority;
        g_task_uxBasePriority = base;
        va_taskcreated(task, name);
        idx = _va_find_task_index(task);
    }
    /* A task first named by an object hook may have only placeholder
       priorities. READY supplies the real values without another metadata
       check on every wakeup; stack bounds wait until its first switch-in. */
    if (idx >= 0 && !taskMap[idx].hasNativeTaskMetadata &&
        (taskMap[idx].uxPriority != priority || taskMap[idx].uxBasePriority != base))
        va_logTaskPriority(task, (int32_t)priority, (int32_t)base, 0);
    /* Reordering a running task after a priority change is not a wakeup. */
    if (idx >= 0 && taskMap[idx].state != VA_TASK_RUNNING)
        va_logTaskState(task, VA_TASK_READY);
    VA_CS_EXIT();
}

void va_freertos_wait(VA_WaitReason_t reason, void *object, VA_QueueObjectType_t type, uint32_t detail)
{
    if (!VA_IsInit()) return;
    void *task = (void *)xTaskGetCurrentTaskHandle();
    va_logTaskWait(task, reason, object, type, detail);
    va_logTaskState(task, VA_TASK_BLOCKED);
}

void va_freertos_block(void *object, bool send)
{
    if (!VA_IsInit()) return;
    VA_QueueObjectType_t type = va_adapter_get_queue_object_type(object);
    VA_WaitReason_t reason = send ? VA_WAIT_QUEUE_SEND : VA_WAIT_QUEUE_RECEIVE;
    if (type == VA_OBJECT_TYPE_MUTEX || type == VA_OBJECT_TYPE_RECURSIVE_MUTEX)
        reason = VA_WAIT_MUTEX;
    else if (type == VA_OBJECT_TYPE_BINARY_SEM || type == VA_OBJECT_TYPE_COUNTING_SEM)
        reason = VA_WAIT_SEMAPHORE;
    va_freertos_wait(reason, object, type, 0);
}
#endif

#if VA_TRACE_STREAM_BUFFERS || VA_TRACE_TASK_STATES
void va_freertos_stream(void *buffer, bool message, uint32_t capacity,
                        uint8_t operation, uint32_t transferred, uint32_t requested)
{
    if (!VA_IsInit()) return;
    VA_QueueObjectType_t type = message ? VA_OBJECT_TYPE_MESSAGE_BUFFER : VA_OBJECT_TYPE_STREAM_BUFFER;
    uint16_t exception = (uint16_t)__get_IPSR();
    void *task = exception == 0 ? (void *)xTaskGetCurrentTaskHandle() : NULL;
#if VA_TRACE_STREAM_BUFFERS
    /* Buffer-only builds may have no switch hook to register a task that
       existed before VA_Init. Its name is still needed for attribution. */
    if (task != NULL && _va_find_task_id(task) == 0) {
        VA_ATOMIC(
            g_task_pxStack = NULL;
            g_task_pxEndOfStack = NULL;
            g_task_ulStackDepth = 0;
            g_task_uxPriority = INT32_MIN;
            g_task_uxBasePriority = INT32_MIN;
            va_taskcreated(task, pcTaskGetName((TaskHandle_t)task)));
    }
#endif
#if VA_TRACE_TASK_STATES
    if (operation == VA_OP_WAIT_BEGIN) {
        va_freertos_wait(transferred == 0 ? VA_WAIT_STREAM_SEND : VA_WAIT_STREAM_RECEIVE,
                         buffer, type, requested);
        return;
    }
    if (task != NULL && operation != 0) va_clearTaskWait(task);
#endif
#if VA_TRACE_STREAM_BUFFERS
    va_logRtosObjectInfo(buffer, type, capacity, message ? (uint32_t)sizeof(configMESSAGE_BUFFER_LENGTH_TYPE) : 1);
    if (operation != 0 && operation != VA_OP_WAIT_BEGIN)
        va_logRtosOperation(buffer, type, VA_EVENT_STREAM_BUFFER,
            (VA_RtosOperation_t)operation, transferred, requested, task, exception);
#else
    (void)capacity; (void)requested;
#endif
}
#endif

#if VA_HAS_QUEUE_DETAILS || VA_HAS_EVENT_FLAG_DETAILS || VA_HAS_NOTIFICATION_DETAILS
static void *va_freertos_detail_task(void)
{
    if (__get_IPSR() != 0) return NULL;
    void *task = (void *)xTaskGetCurrentTaskHandle();
    if (VA_IsInit() && task != NULL && _va_find_task_id(task) == 0) {
        VA_ATOMIC(
            g_task_pxStack = NULL; g_task_pxEndOfStack = NULL; g_task_ulStackDepth = 0;
            g_task_uxPriority = INT32_MIN; g_task_uxBasePriority = INT32_MIN;
            va_taskcreated(task, pcTaskGetName((TaskHandle_t)task)));
    }
    return task;
}
#endif
#if VA_HAS_QUEUE_DETAILS
void va_freertos_queue_detail(void *queue, uint8_t operation, uint32_t used,
                               uint32_t capacity, uint32_t itemSize, uint32_t detail)
{
    if (!VA_IsInit() || itemSize == 0) return; /* shared hooks also see semaphores */
    void *task = va_freertos_detail_task();
    va_logRtosObjectInfo(queue, VA_OBJECT_TYPE_QUEUE, capacity, itemSize);
    va_logRtosOperation(queue, VA_OBJECT_TYPE_QUEUE, VA_EVENT_QUEUE_DETAILS,
        (VA_RtosOperation_t)operation, used, detail, task, (uint16_t)__get_IPSR());
}
#endif
#if VA_HAS_EVENT_FLAG_DETAILS
void va_freertos_flags(void *group, uint8_t operation, uint32_t bits, uint32_t mask)
{
    if (!VA_IsInit()) return;
    void *task = va_freertos_detail_task();
    va_logRtosOperation(group, VA_OBJECT_TYPE_EVENTFLAG, VA_EVENT_FLAG_DETAILS,
        (VA_RtosOperation_t)operation, bits, mask, task, (uint16_t)__get_IPSR());
}
#endif
#if VA_HAS_NOTIFICATION_DETAILS
void va_freertos_notify(void *destination, void *sender, uint8_t operation,
                        uint32_t value, uint32_t index)
{
    if (!VA_IsInit()) return;
    (void)va_freertos_detail_task();
    va_logNotifyDetail(destination, sender, (uint16_t)__get_IPSR(), operation, value, index);
}
#endif

#endif /* VA_ENABLED && VA_RTOS_FREERTOS */
