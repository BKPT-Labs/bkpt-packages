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
    uint32_t free_stack_words;
#if (portSTACK_GROWTH < 0)
    /* Scan standard FreeRTOS fill within known bounds, in whole stack words. */
    if (sizeof(StackType_t) == sizeof(uint32_t) && idx >= 0 &&
        taskMap[idx].pxStack != NULL && taskMap[idx].ulStackDepth > 0 &&
        ((uintptr_t)taskMap[idx].pxStack % sizeof(uint32_t)) == 0)
    {
        const uint8_t *stack = (const uint8_t *)taskMap[idx].pxStack;
        free_stack_words = 0;
        while (free_stack_words < taskMap[idx].ulStackDepth)
        {
            uint32_t fill;
            memcpy(&fill, stack + free_stack_words * sizeof(fill), sizeof(fill));
            if (fill != UINT32_C(0xa5a5a5a5))
                break;
            ++free_stack_words;
        }
        if (free_stack_words == taskMap[idx].ulStackDepth)
            free_stack_words = uxTaskGetStackHighWaterMark((TaskHandle_t)taskHandle);
    }
    else
#endif
        free_stack_words = uxTaskGetStackHighWaterMark((TaskHandle_t)taskHandle);
    if (idx >= 0 && taskMap[idx].ulStackDepth > 0)
    {
        uint32_t used_stack_words = taskMap[idx].ulStackDepth - free_stack_words;
        return used_stack_words * (uint32_t)sizeof(StackType_t);
    }
    return free_stack_words * (uint32_t)sizeof(StackType_t);
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
        return taskMap[idx].ulStackDepth * (uint32_t)sizeof(StackType_t);
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
/* Registers tasks the create hook never saw (created before VA_Init).
   Runs in the context-switch path: only lock-free FreeRTOS accessors are
   safe here, so priority and stack depth stay 0 for late registrations. */
void va_freertos_taskswitchedin(void *taskHandle)
{
    if (VA_IsInit() && _va_find_task_id(taskHandle) == 0)
    {
        g_task_pxStack       = NULL;
        g_task_pxEndOfStack  = NULL;
        g_task_uxPriority    = 0;
        g_task_uxBasePriority = 0;
        g_task_ulStackDepth  = 0;
        va_taskcreated(taskHandle, pcTaskGetName((TaskHandle_t)taskHandle));
    }
#if VA_TRACE_SLEEP
    /* A delayed task waking up: close its sleep before the switch event. */
    va_freertos_sleep_exit(taskHandle);
#endif
    va_taskswitchedin(taskHandle);
}
#endif /* VA_NEEDS_SWITCH_HOOK */

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

/* Duration/period for the arm event: FreeRTOS timers first fire one full
   period after starting, so duration = period; one-shot timers report
   period 0. periodTicks is the period THIS arm takes effect with (for
   change-period commands: the queued new value, not xTimerGetPeriod).
   uxTimerGetReloadMode takes a task-level critical section that is
   illegal in ISR context, and does not exist before kernel 10.2; where
   it cannot be called, timers report as periodic (the safer reading). */
static void va_freertos_timer_arm(void *timer, uint32_t periodTicks, bool fromIsr)
{
    uint32_t periodMs = (uint32_t)(periodTicks * portTICK_PERIOD_MS);
    bool autoReload = true;
#if (tskKERNEL_VERSION_MAJOR > 10) \
    || (tskKERNEL_VERSION_MAJOR == 10 && tskKERNEL_VERSION_MINOR >= 2)
    if (!fromIsr)
        autoReload = uxTimerGetReloadMode((TimerHandle_t)timer) != 0;
#else
    (void)fromIsr;
#endif
    va_logTimerArm(timer, periodMs, autoReload ? periodMs : 0);
}

void va_freertos_timer_command(void *timer, int32_t commandId, uint32_t optionalValue)
{
    if (timer == NULL)
        return;
    va_freertos_timer_ensure_registered(timer);

    switch (commandId)
    {
    case tmrCOMMAND_START:
    case tmrCOMMAND_RESET:
        va_logQueueObjectGiveTyped(timer, VA_OBJECT_TYPE_TIMER);
        va_freertos_timer_arm(timer, (uint32_t)xTimerGetPeriod((TimerHandle_t)timer),
                              false);
        break;

    case tmrCOMMAND_CHANGE_PERIOD:
        va_logQueueObjectGiveTyped(timer, VA_OBJECT_TYPE_TIMER);
        va_freertos_timer_arm(timer, optionalValue, false);
        break;

#if defined(tmrCOMMAND_START_FROM_ISR)
    case tmrCOMMAND_START_FROM_ISR:
    case tmrCOMMAND_RESET_FROM_ISR:
        va_logQueueObjectGiveTyped(timer, VA_OBJECT_TYPE_TIMER);
        va_freertos_timer_arm(timer, (uint32_t)xTimerGetPeriod((TimerHandle_t)timer),
                              true);
        break;

    case tmrCOMMAND_CHANGE_PERIOD_FROM_ISR:
        va_logQueueObjectGiveTyped(timer, VA_OBJECT_TYPE_TIMER);
        va_freertos_timer_arm(timer, optionalValue, true);
        break;
#endif

    case tmrCOMMAND_STOP:
#if defined(tmrCOMMAND_STOP_FROM_ISR)
    case tmrCOMMAND_STOP_FROM_ISR:
#endif
        va_logQueueObjectTakeTyped(timer, VA_OBJECT_TYPE_TIMER);
        break;

    case tmrCOMMAND_DELETE:
        va_logQueueObjectDelete(timer);
        break;

    default:
        break;
    }
}

void va_freertos_timer_expired(void *timer)
{
    if (timer == NULL)
        return;
    va_freertos_timer_ensure_registered(timer);
    va_logQueueObjectTakeTyped(timer, VA_OBJECT_TYPE_TIMER);
}
#endif /* VA_TRACE_TIMERS */

/* ── Kernel heap (traceMALLOC / traceFREE) ───────────────────────── */

#if VA_TRACE_RTOS_HEAPS
/* One heap object for the kernel allocator; the sentinel address stands in
   for a handle. The running total is derived from the trace hook sizes, so
   it is scheme-independent (heap_1 through heap_5). Heap functions run with
   the scheduler suspended and are not ISR-callable, but the counter update
   stays atomic for safety. */
static uint8_t  s_va_heap_sentinel;
static uint32_t s_va_heap_allocated;
static bool     s_va_heap_registered;

static void va_freertos_heap_ensure_registered(void)
{
    if (s_va_heap_registered || !VA_IsInit())
        return;
    s_va_heap_registered = true;
#if defined(configTOTAL_HEAP_SIZE)
    va_logHeapCapacity(&s_va_heap_sentinel, "FreeRTOSHeap", (uint32_t)configTOTAL_HEAP_SIZE);
#endif
}

void va_freertos_heap_alloc(void *address, uint32_t size)
{
    if (!VA_IsInit())
        return;
    va_freertos_heap_ensure_registered();

    if (address == NULL)
    {
        if (size > 0)
            va_logHeapAllocFailed(&s_va_heap_sentinel, size);
        return;
    }

    uint32_t total;
    VA_ATOMIC(s_va_heap_allocated += size; total = s_va_heap_allocated);
    va_logHeapAlloc(&s_va_heap_sentinel, total);
}

void va_freertos_heap_free(void *address, uint32_t size)
{
    if (!VA_IsInit() || address == NULL)
        return;
    va_freertos_heap_ensure_registered();

    uint32_t total;
    VA_ATOMIC(
        s_va_heap_allocated = (size <= s_va_heap_allocated) ? s_va_heap_allocated - size : 0;
        total = s_va_heap_allocated);
    va_logHeapFree(&s_va_heap_sentinel, total);
}
#endif /* VA_TRACE_RTOS_HEAPS */

/* ── Mutex contention detection ──────────────────────────────────── */

#if VA_NEEDS_BLOCKING_HOOK
void va_adapter_check_mutex_contention(void *queueObject, uint8_t queue_va_id)
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
                uint8_t holder_id = _va_find_task_id((void *)holder);
                uint8_t waiter_id = _va_find_task_id((void *)current);
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
            g_task_uxPriority = 0;
            g_task_uxBasePriority = 0;
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
            g_task_uxPriority = 0; g_task_uxBasePriority = 0;
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
