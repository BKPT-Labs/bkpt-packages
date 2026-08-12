/**
 * @file VA_Adapter_FreeRTOS.c
 * @brief ViewAlyzer FreeRTOS Adapter - RTOS-specific logic
 *
 * Contains everything that depends on FreeRTOS internals:
 *   - Queue-type detection (QueueDefinitionMirror hack)
 *   - Stack-usage calculation via uxTaskGetStackHighWaterMark
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
    uint32_t free_stack_words = uxTaskGetStackHighWaterMark((TaskHandle_t)taskHandle);
    int idx = _va_find_task_index(taskHandle);
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

#if VA_TRACE_SLEEP
/* The sleeping flag makes enter/exit idempotent: a resume of a task that
   never slept emits nothing, and a resumed task's switch-in does not emit
   a second exit. */
void va_freertos_sleep_enter(void *taskHandle)
{
    if (!VA_IsInit() || taskHandle == NULL)
        return;
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
}

void va_freertos_sleep_exit(void *taskHandle)
{
    if (!VA_IsInit() || taskHandle == NULL)
        return;
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

#endif /* VA_ENABLED && VA_RTOS_FREERTOS */
