/**
 * @file ViewAlyzerFreeRTOSHook_Common.h
 * @brief ViewAlyzer FreeRTOS trace hooks shared by every kernel version
 *
 * Do NOT include this file directly. Include one of:
 *   ViewAlyzerFreeRTOSHook.h            - FreeRTOS before v10.4
 *   ViewAlyzerFreeRTOSHook_V10_4_Plus.h - FreeRTOS v10.4 and later
 * from the bottom of your FreeRTOSConfig.h. They differ only in the task
 * notification macro signatures (v10.4 added the notification index
 * parameter); everything else lives here.
 *
 * Which hooks get installed follows the VA_TRACE_* categories in
 * ViewAlyzerConfig.h. A hook that is not installed keeps the kernel's own
 * no-op default, so a disabled category costs literally nothing.
 *
 * IMPORTANT: this header is compiled into the KERNEL (through
 * FreeRTOSConfig.h), while ViewAlyzer.c is compiled into the recorder. Both
 * read the same ViewAlyzerConfig.h, so the switches cannot disagree - but
 * only if the configuration reaches both targets. Defines made locally
 * inside FreeRTOSConfig.h are invisible to ViewAlyzer.c; use -D, -include,
 * or VA_CONFIG_HEADER.
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

#ifndef VIEWALYZER_FREERTOS_HOOK_COMMON_H
#define VIEWALYZER_FREERTOS_HOOK_COMMON_H

/* Some FreeRTOS ports include FreeRTOSConfig.h from assembly. Everything
   below is C. */
#ifndef __ASSEMBLER__

#include "ViewAlyzer.h"

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_FREERTOS)

/* The recorder's critical sections are PRIMASK-based and per-core; on an
   SMP kernel the cores would race the registries and interleave packet
   bytes on the wire. */
#if defined(configNUMBER_OF_CORES) && (configNUMBER_OF_CORES > 1)
#error "ViewAlyzer: FreeRTOS SMP (configNUMBER_OF_CORES > 1) is not supported"
#endif

#if VA_TRACE_TASK_STATES || VA_TRACE_STREAM_BUFFERS || VA_HAS_QUEUE_DETAILS || VA_HAS_EVENT_FLAG_DETAILS || VA_HAS_NOTIFICATION_DETAILS
#ifndef INCLUDE_xTaskGetCurrentTaskHandle
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#elif INCLUDE_xTaskGetCurrentTaskHandle != 1
#error "ViewAlyzer: scheduler/buffer tracing requires INCLUDE_xTaskGetCurrentTaskHandle=1"
#endif
#endif

/* Suggested FreeRTOS config: gives traceTASK_CREATE a real stack depth. */
#ifndef configRECORD_STACK_HIGH_ADDRESS
#define configRECORD_STACK_HIGH_ADDRESS 1
#endif

/* Stack sampling reads the high-water mark; FreeRTOS defaults the INCLUDE
   to 0, which would silently disable the category. Defined here (bottom of
   FreeRTOSConfig.h) so it lands before FreeRTOS.h applies its default; an
   explicit 0 set above the hook include still wins. */
#if VA_TRACE_STACK_USAGE
#ifndef INCLUDE_uxTaskGetStackHighWaterMark
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#endif
#endif

#if VA_HAS_QUEUE_DETAILS || VA_HAS_EVENT_FLAG_DETAILS || VA_HAS_NOTIFICATION_DETAILS
#ifdef __cplusplus
extern "C" {
#endif
void va_freertos_queue_detail(void *queue, uint8_t operation, uint32_t used,
                               uint32_t capacity, uint32_t itemSize, uint32_t detail);
void va_freertos_flags(void *group, uint8_t operation, uint32_t bits, uint32_t mask);
void va_freertos_notify(void *destination, void *sender, uint8_t operation,
                        uint32_t value, uint32_t index);
#ifdef __cplusplus
}
#endif
#endif
#if VA_HAS_QUEUE_DETAILS
/* Native Queue_t fields are read only in queue.c, under its own lock.
   SEND/RECEIVE hooks precede the count update. Full sends are overwrites. */
#define VA_FREERTOS_QUEUE(q, op, used, arg) \
    va_freertos_queue_detail((q), (op), (uint32_t)(used), (uint32_t)(q)->uxLength, \
                             (uint32_t)(q)->uxItemSize, (uint32_t)(arg))
#else
#define VA_FREERTOS_QUEUE(q, op, used, arg) ((void)0)
#endif

/* Each section refuses to stomp (or be stomped by) another trace tool's
   macros: a silent redefinition would leave one of the two tools recording
   nothing. */

/* ── Task switches ───────────────────────────────────────────────── */
/* Switch-OUT also takes the stack high-water sample, so it stays installed
   for a stack-usage-only build. Switch-IN goes through the adapter, which
   lazily registers tasks the create hook never saw (created before
   VA_Init). */
#if VA_NEEDS_SWITCH_HOOK
#if defined(traceTASK_SWITCHED_IN) || defined(traceTASK_SWITCHED_OUT)
#error "ViewAlyzer: a task-switch trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#ifdef __cplusplus
extern "C" void va_freertos_taskswitchedin(void *taskHandle);
#else
void va_freertos_taskswitchedin(void *taskHandle);
#endif

#define traceTASK_SWITCHED_IN() va_freertos_taskswitchedin((void *)pxCurrentTCB)
#define traceTASK_SWITCHED_OUT() va_taskswitchedout((void *)pxCurrentTCB)
#endif

/* ── Task creation / deletion ────────────────────────────────────── */
/* Installed whenever any category needs task ids/names. */
#if VA_NEEDS_TASK_REGISTRY

#if defined(traceTASK_CREATE) || defined(traceTASK_DELETE)
#error "ViewAlyzer: a task-lifecycle trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#if (configRECORD_STACK_HIGH_ADDRESS == 1)
#define VA_CALCULATE_STACK_DEPTH(pxTCB) \
    g_task_ulStackDepth = (uint32_t)((pxTCB)->pxEndOfStack - (pxTCB)->pxStack)
#else
#define VA_CALCULATE_STACK_DEPTH(pxTCB) \
    g_task_ulStackDepth = 0
#endif

#define traceTASK_CREATE(pxNewTCB)                                   \
    do                                                               \
    {                                                                \
        g_task_pxStack = (pxNewTCB)->pxStack;                        \
        g_task_uxPriority = (pxNewTCB)->uxPriority;                  \
        g_task_pxEndOfStack = NULL;                                  \
        g_task_uxBasePriority = (pxNewTCB)->uxPriority;              \
        VA_CALCULATE_STACK_DEPTH(pxNewTCB);                          \
        va_taskcreated((void *)(pxNewTCB), pcTaskGetName(pxNewTCB));  \
    } while (0)

/* Frees the registry slot so a recycled TCB address cannot inherit the
   dead task's identity. */
#define traceTASK_DELETE(pxTaskToDelete) \
    va_taskdeleted((void *)(pxTaskToDelete))

#endif /* VA_NEEDS_TASK_REGISTRY */

/* ── Queues, mutexes, and semaphores ─────────────────────────────── */
/* FreeRTOS routes all non-recursive mutex/semaphore traffic through the
   shared traceQUEUE_SEND / traceQUEUE_RECEIVE hooks; the recorder separates
   them at run time by object type. */
#if VA_NEEDS_SYNC_HOOKS

#if defined(traceQUEUE_CREATE) || defined(traceQUEUE_DELETE) || defined(traceQUEUE_SEND) || defined(traceQUEUE_RECEIVE) || \
    defined(traceQUEUE_SEND_FROM_ISR) || defined(traceQUEUE_RECEIVE_FROM_ISR) || defined(traceCREATE_MUTEX) || \
    defined(traceGIVE_MUTEX_RECURSIVE) || defined(traceTAKE_MUTEX_RECURSIVE) || defined(traceQUEUE_REGISTRY_ADD)
#error "ViewAlyzer: a queue trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

/* traceQUEUE_CREATE fires for mutexes/semaphores too; the recorder
   registers first and corrects the type on traceCREATE_MUTEX. */
#define traceQUEUE_CREATE(pxNewQueue) \
    do { va_logQueueObjectCreateWithType((pxNewQueue), "Queue"); \
         VA_FREERTOS_QUEUE((pxNewQueue), VA_QUEUE_INIT, (pxNewQueue)->uxMessagesWaiting, 0); } while (0)

/* vQueueDelete fires this for queues, mutexes, and semaphores alike;
   the slot is freed so a recycled handle cannot inherit the old name. */
#define traceQUEUE_DELETE(pxQueue) \
    va_logQueueObjectDelete((pxQueue))

#define traceCREATE_MUTEX(pxNewMutex) \
    va_updateQueueObjectType((pxNewMutex), "Mutex")

#define traceQUEUE_SEND(pxQueue) \
    do { va_logQueueObjectGive((pxQueue), 0); \
         VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_SEND, ((pxQueue)->uxMessagesWaiting < (pxQueue)->uxLength ? (pxQueue)->uxMessagesWaiting + 1 : (pxQueue)->uxLength), 1); } while (0)

#define traceQUEUE_SEND_FROM_ISR(pxQueue) \
    do { va_logQueueObjectGive((pxQueue), 0); \
         VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_SEND, ((pxQueue)->uxMessagesWaiting < (pxQueue)->uxLength ? (pxQueue)->uxMessagesWaiting + 1 : (pxQueue)->uxLength), 1); } while (0)

#define traceQUEUE_RECEIVE(pxQueue) \
    do { va_logQueueObjectTake((pxQueue), 0); \
         VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_RECEIVE, ((pxQueue)->uxMessagesWaiting - 1), 1); } while (0)

#define traceQUEUE_RECEIVE_FROM_ISR(pxQueue) \
    do { va_logQueueObjectTake((pxQueue), 0); \
         VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_RECEIVE, ((pxQueue)->uxMessagesWaiting - 1), 1); } while (0)

/* User-assigned names from vQueueAddToRegistry() replace the auto-generated
   ones. */
#define traceQUEUE_REGISTRY_ADD(xQueue, pcQueueName) \
    va_logQueueObjectSetName((xQueue), (pcQueueName))

/* Recursive-mutex trace points are deliberately NOT hooked. They fire at
   ENTRY, before the outcome is known, and a first (non-nested) take/final
   give also runs the shared queue hooks - hooking them would emit doubled
   events plus a phantom take on timeout. The shared hooks alone yield
   exactly one take at first acquisition and one give at final release;
   nested takes/gives change no ownership and emit nothing. */
#define traceGIVE_MUTEX_RECURSIVE(pxMutex) ((void)(pxMutex))
#define traceTAKE_MUTEX_RECURSIVE(pxMutex) ((void)(pxMutex))

/* Failed sends and receives (timeout, queue full/empty) - the shared hooks
   cover queues, mutexes, and semaphores alike, filtered by object type. */
#if defined(traceQUEUE_SEND_FAILED) || defined(traceQUEUE_RECEIVE_FAILED)
#error "ViewAlyzer: a queue failed-op trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif
#define traceQUEUE_SEND_FAILED(pxQueue)             do { va_logQueueObjectOpFailed((pxQueue), true, 0); \
    VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_SEND_FAILED, (pxQueue)->uxMessagesWaiting, 0); } while (0)
#define traceQUEUE_SEND_FROM_ISR_FAILED(pxQueue)    do { va_logQueueObjectOpFailed((pxQueue), true, 0); \
    VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_SEND_FAILED, (pxQueue)->uxMessagesWaiting, 0); } while (0)
#define traceQUEUE_RECEIVE_FAILED(pxQueue)          do { va_logQueueObjectOpFailed((pxQueue), false, 0); \
    VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_RECEIVE_FAILED, (pxQueue)->uxMessagesWaiting, 0); } while (0)
#define traceQUEUE_RECEIVE_FROM_ISR_FAILED(pxQueue) do { va_logQueueObjectOpFailed((pxQueue), false, 0); \
    VA_FREERTOS_QUEUE((pxQueue), VA_QUEUE_RECEIVE_FAILED, (pxQueue)->uxMessagesWaiting, 0); } while (0)

/* The recursive variants stay silent: their final failed take also runs
   traceQUEUE_RECEIVE_FAILED through the shared path. */
#define traceGIVE_MUTEX_RECURSIVE_FAILED(pxMutex) ((void)(pxMutex))
#define traceTAKE_MUTEX_RECURSIVE_FAILED(pxMutex) ((void)(pxMutex))
#define traceCREATE_MUTEX_FAILED()                ((void)0)

/* Covered by traceQUEUE_CREATE (created through xQueueGenericCreate). */
#define traceCREATE_COUNTING_SEMAPHORE() ((void)0)
#define traceCREATE_BINARY_SEMAPHORE()   ((void)0)

#if VA_HAS_QUEUE_DETAILS
#if defined(traceQUEUE_PEEK) || defined(traceQUEUE_PEEK_FROM_ISR) || defined(traceQUEUE_PEEK_FAILED) || defined(traceQUEUE_PEEK_FROM_ISR_FAILED) || defined(traceENTER_xQueueGenericSend) || defined(traceENTER_xQueueGenericSendFromISR) || defined(traceRETURN_xQueueGenericReset)
#error "ViewAlyzer: detailed queue trace macro already defined"
#endif
#define traceQUEUE_PEEK(q) VA_FREERTOS_QUEUE(q, VA_QUEUE_PEEK, (q)->uxMessagesWaiting, 0)
#define traceQUEUE_PEEK_FROM_ISR(q) traceQUEUE_PEEK(q)
#define traceQUEUE_PEEK_FAILED(q) VA_FREERTOS_QUEUE(q, VA_QUEUE_PEEK_FAILED, (q)->uxMessagesWaiting, 0)
#define traceQUEUE_PEEK_FROM_ISR_FAILED(q) traceQUEUE_PEEK_FAILED(q)
/* FreeRTOS 11 invokes these stock hooks. Earlier kernels simply never call
   them; do not wrap the public APIs or guess copy position at shared hooks. */
#define traceENTER_xQueueGenericSend(q, item, ticks, pos) \
    VA_FREERTOS_QUEUE(((Queue_t *)(q)), VA_QUEUE_SEND_BEGIN, UINT32_MAX, pos)
#define traceENTER_xQueueGenericSendFromISR(q, item, woken, pos) \
    VA_FREERTOS_QUEUE(((Queue_t *)(q)), VA_QUEUE_SEND_BEGIN, UINT32_MAX, pos)
#define traceRETURN_xQueueGenericReset(result) \
    do { if ((result) == pdPASS && xNewQueue == pdFALSE) \
        VA_FREERTOS_QUEUE(pxQueue, VA_QUEUE_RESET, pxQueue->uxMessagesWaiting, 0); } while (0)
#endif

#endif /* VA_NEEDS_SYNC_HOOKS */

/* ── Mutex contention ────────────────────────────────────────────── */
/* Fires just before a task blocks, while the holder can still be sampled. */
#if VA_NEEDS_BLOCKING_HOOK
#if defined(traceBLOCKING_ON_QUEUE_RECEIVE)
#error "ViewAlyzer: traceBLOCKING_ON_QUEUE_RECEIVE is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif
#define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue) \
    va_logQueueObjectBlocking((pxQueue))
#endif

/* ── Software timers ─────────────────────────────────────────────── */
/* A timer arms as a give (start/reset/period change) and closes as a take
   (expiry or stop), so a one-shot timer's give-to-take pairing is its armed
   window. COMMAND_SEND is hooked rather than COMMAND_RECEIVED: it fires in
   the requesting context at the moment of the request, including the
   FromISR variants, and only when the command was accepted (xReturn). */
#if VA_TRACE_TIMERS
#if defined(traceTIMER_CREATE) || defined(traceTIMER_COMMAND_SEND) || defined(traceTIMER_EXPIRED)
#error "ViewAlyzer: a timer trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif
void va_freertos_timer_command(void *timer, int32_t commandId, uint32_t optionalValue);
void va_freertos_timer_expired(void *timer);
#ifdef __cplusplus
}
#endif

#define traceTIMER_CREATE(pxNewTimer) \
    va_logQueueObjectCreateTyped((void *)(pxNewTimer), (pxNewTimer)->pcTimerName, VA_OBJECT_TYPE_TIMER)

/* xOptionalValue rides along: for the change-period commands it carries the
   NEW period in ticks, which the kernel has only queued at this point. */
#define traceTIMER_COMMAND_SEND(xTimer, xCommandID, xOptionalValue, xReturn)          \
    do                                                                                \
    {                                                                                 \
        if ((xReturn) != pdFAIL)                                                      \
            va_freertos_timer_command((void *)(xTimer), (int32_t)(xCommandID),        \
                                      (uint32_t)(xOptionalValue));                    \
    } while (0)

#define traceTIMER_EXPIRED(pxTimer) \
    va_freertos_timer_expired((void *)(pxTimer))

#endif /* VA_TRACE_TIMERS */

/* ── Event groups ────────────────────────────────────────────────── */
/* Set-bits is a give-shaped event (value = the bits), a satisfied wait or
   sync is the matching take-shaped event (value = the bits waited for),
   and a timed-out wait emits a failed-op event instead. Clear-bits and
   the pre-block points are not traced. */
#if VA_TRACE_EVENT_FLAGS
#if defined(traceEVENT_GROUP_CREATE) || defined(traceEVENT_GROUP_DELETE) \
    || defined(traceEVENT_GROUP_SET_BITS) || defined(traceEVENT_GROUP_WAIT_BITS_END) \
    || defined(traceEVENT_GROUP_SYNC_END)
#error "ViewAlyzer: an event-group trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#define traceEVENT_GROUP_CREATE(xEventGroup) \
    va_logQueueObjectCreateTyped((void *)(xEventGroup), NULL, VA_OBJECT_TYPE_EVENTFLAG)

#define traceEVENT_GROUP_DELETE(xEventGroup) \
    va_logQueueObjectDelete((void *)(xEventGroup))

#define traceEVENT_GROUP_SET_BITS(xEventGroup, uxBitsToSet) \
    va_logEventFlagSet((void *)(xEventGroup), (uint32_t)(uxBitsToSet))

#define traceEVENT_GROUP_SET_BITS_FROM_ISR(xEventGroup, uxBitsToSet) \
    va_logEventFlagSet((void *)(xEventGroup), (uint32_t)(uxBitsToSet))

#define traceEVENT_GROUP_WAIT_BITS_END(xEventGroup, uxBitsToWaitFor, xTimeoutOccurred)      \
    do                                                                                      \
    {                                                                                       \
        if (xTimeoutOccurred)                                                               \
            va_logObjectOpFailedTyped((void *)(xEventGroup), VA_OBJECT_TYPE_EVENTFLAG,      \
                                      false, (uint32_t)(uxBitsToWaitFor));                  \
        else                                                                                \
            va_logEventFlagWaitEnd((void *)(xEventGroup), (uint32_t)(uxBitsToWaitFor));     \
    } while (0)

#define traceEVENT_GROUP_SYNC_END(xEventGroup, uxBitsToSet, uxBitsToWaitFor, xTimeoutOccurred) \
    traceEVENT_GROUP_WAIT_BITS_END(xEventGroup, uxBitsToWaitFor, xTimeoutOccurred)

#if VA_HAS_EVENT_FLAG_DETAILS
#if defined(traceEVENT_GROUP_CLEAR_BITS) || defined(traceEVENT_GROUP_CLEAR_BITS_FROM_ISR)
#error "ViewAlyzer: event-group clear trace macro already defined"
#endif
#define traceEVENT_GROUP_CLEAR_BITS(group, bits) \
    va_freertos_flags((group), VA_FLAGS_CLEAR, 0, (bits))
/* 11.x's return hook supplies a final value without dereferencing the group. */
#if defined(traceRETURN_xEventGroupSetBits)
#error "ViewAlyzer: event-group return trace macro already defined"
#endif
#define traceRETURN_xEventGroupSetBits(result) \
    va_freertos_flags(xEventGroup, VA_FLAGS_SNAPSHOT, (result), 0)
#define traceEVENT_GROUP_CLEAR_BITS_FROM_ISR(group, bits) \
    va_freertos_flags((group), VA_FLAGS_CLEAR_DEFERRED, 0, (bits))
#undef traceEVENT_GROUP_SET_BITS
#define traceEVENT_GROUP_SET_BITS(group, bits) \
    do { va_logEventFlagSet((group), (bits)); \
         va_freertos_flags((group), VA_FLAGS_SET, (bits), (bits)); } while (0)
#undef traceEVENT_GROUP_SET_BITS_FROM_ISR
#define traceEVENT_GROUP_SET_BITS_FROM_ISR(group, bits) \
    va_freertos_flags((group), VA_FLAGS_SET_DEFERRED, 0, (bits))
#undef traceEVENT_GROUP_WAIT_BITS_END
#define traceEVENT_GROUP_WAIT_BITS_END(group, bits, timedOut) \
    do { \
        va_freertos_flags((group), ((xWaitForAllBits ? (uxReturn & (bits)) == (bits) : (uxReturn & (bits)) != 0) ? VA_FLAGS_WAIT_OK : VA_FLAGS_WAIT_TIMEOUT) | \
            (xWaitForAllBits ? VA_FLAGS_ALL : 0) | (xClearOnExit ? VA_FLAGS_CLEAR_ON_EXIT : 0), uxReturn, (bits)); \
    } while (0)
#undef traceEVENT_GROUP_SYNC_END
#define traceEVENT_GROUP_SYNC_END(group, setBits, waitBits, timedOut) \
    do { \
        va_freertos_flags((group), (((uxReturn & (waitBits)) == (waitBits)) ? VA_FLAGS_WAIT_OK : VA_FLAGS_WAIT_TIMEOUT) | \
            VA_FLAGS_ALL | VA_FLAGS_CLEAR_ON_EXIT, uxReturn, (waitBits)); \
    } while (0)
#endif

#endif /* VA_TRACE_EVENT_FLAGS */

/* ── Sleep (vTaskDelay / vTaskDelayUntil / suspend / resume) ─────── */
/* Delay and suspend have no exit trace point in the kernel; the sleep is
   closed at the task's next switch-in (delay expiry) or at the resume call.
   Blocking on queues or mutexes is deliberately NOT counted as sleep. */
#if VA_TRACE_SLEEP || VA_TRACE_TASK_STATES
#if defined(traceTASK_DELAY) || defined(traceTASK_DELAY_UNTIL) || defined(traceTASK_SUSPEND) \
    || defined(traceTASK_RESUME) || defined(traceTASK_RESUME_FROM_ISR)
#error "ViewAlyzer: a delay/suspend trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif
void va_freertos_sleep_enter(void *taskHandle);
void va_freertos_sleep_exit(void *taskHandle);
#ifdef __cplusplus
}
#endif

#define traceTASK_DELAY() do { va_logTaskState((void *)pxCurrentTCB, VA_TASK_SLEEPING); va_freertos_sleep_enter((void *)pxCurrentTCB); } while (0)
/* Variadic: FreeRTOS v9 invokes this with no argument, v10+ with the wake
   time. Neither is needed. */
#define traceTASK_DELAY_UNTIL(...) traceTASK_DELAY()
/* The kernel resolves the handle before these fire, so pxTCB is never NULL
   (a NULL argument means "the current task"). */
#define traceTASK_SUSPEND(pxTaskToSuspend) do { va_logTaskState((void *)(pxTaskToSuspend), VA_TASK_SUSPENDED); va_freertos_sleep_enter((void *)(pxTaskToSuspend)); } while (0)
#define traceTASK_RESUME(pxTaskToResume)          va_freertos_sleep_exit((void *)(pxTaskToResume))
#define traceTASK_RESUME_FROM_ISR(pxTaskToResume) va_freertos_sleep_exit((void *)(pxTaskToResume))

#endif /* VA_TRACE_SLEEP */

/* ── Kernel heap (pvPortMalloc / vPortFree) ──────────────────────── */
/* The reported value is bytes allocated since tracing started (a running
   total from the trace hook sizes), which works identically across every
   heap_n scheme; allocations made before VA_Init are not counted. A NULL
   return reports a failed allocation with the requested size. */
#if VA_TRACE_RTOS_HEAPS
#if defined(traceMALLOC) || defined(traceFREE)
#error "ViewAlyzer: a heap trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif
void va_freertos_heap_alloc(void *address, uint32_t size);
void va_freertos_heap_free(void *address, uint32_t size);
#ifdef __cplusplus
}
#endif

#define traceMALLOC(pvAddress, uiSize) va_freertos_heap_alloc((pvAddress), (uint32_t)(uiSize))
#define traceFREE(pvAddress, uiSize)   va_freertos_heap_free((pvAddress), (uint32_t)(uiSize))

#endif /* VA_TRACE_RTOS_HEAPS */

/* ── Tickless idle (configUSE_TICKLESS_IDLE) ─────────────────────── */
/* Each suppressed-tick sleep window becomes a power-management suspend
   span, same as Zephyr's pm_system_suspend. Only fires on builds with
   configUSE_TICKLESS_IDLE enabled. */
#if VA_TRACE_PM
#if defined(traceLOW_POWER_IDLE_BEGIN) || defined(traceLOW_POWER_IDLE_END)
#error "ViewAlyzer: a low-power trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif
#define traceLOW_POWER_IDLE_BEGIN() va_logPMSuspendEnter()
#define traceLOW_POWER_IDLE_END()   va_logPMSuspendExit(0)
#endif /* VA_TRACE_PM */


/* Scheduler state is independent of successful synchronization traffic. */
#if VA_TRACE_TASK_STATES
#if defined(traceMOVED_TASK_TO_READY_STATE) || defined(traceTASK_PRIORITY_SET) || defined(traceTASK_PRIORITY_INHERIT) || defined(traceTASK_PRIORITY_DISINHERIT)
#error "ViewAlyzer: scheduler trace macros are already defined"
#endif
#ifdef __cplusplus
extern "C" {
#endif
void va_freertos_task_ready(void *task, const char *name, uint32_t priority, uint32_t base);
void va_freertos_block(void *object, bool send);
void va_freertos_wait(VA_WaitReason_t reason, void *object, VA_QueueObjectType_t type, uint32_t detail);
#ifdef __cplusplus
}
#endif
#if configUSE_MUTEXES == 1
#define VA_FREERTOS_BASE(t) ((t)->uxBasePriority)
/* vTaskPrioritySet changed its inherited-priority rule in FreeRTOS 11.
   Expand the version at the hook call: task.h defines it AFTER config.h. */
#define VA_FREERTOS_SET_EFFECTIVE(t, p) (((t)->uxPriority == (t)->uxBasePriority || (tskKERNEL_VERSION_MAJOR >= 11 && (p) > (t)->uxPriority)) ? (p) : (t)->uxPriority)
#else
#define VA_FREERTOS_BASE(t) ((t)->uxPriority)
#define VA_FREERTOS_SET_EFFECTIVE(t, p) (p)
#endif
#define traceMOVED_TASK_TO_READY_STATE(t) va_freertos_task_ready((void *)(t), (t)->pcTaskName, (t)->uxPriority, VA_FREERTOS_BASE(t))
#define traceTASK_PRIORITY_SET(t, p) va_logTaskPriority((void *)(t), (int32_t)VA_FREERTOS_SET_EFFECTIVE(t, p), (int32_t)(p), 1)
#define traceTASK_PRIORITY_INHERIT(t, p) va_logTaskPriority((void *)(t), (int32_t)(p), (int32_t)VA_FREERTOS_BASE(t), 2)
#define traceTASK_PRIORITY_DISINHERIT(t, p) va_logTaskPriority((void *)(t), (int32_t)(p), (int32_t)VA_FREERTOS_BASE(t), 3)
#if defined(traceBLOCKING_ON_QUEUE_SEND) || defined(traceBLOCKING_ON_QUEUE_PEEK)
#error "ViewAlyzer: queue blocking trace macros are already defined"
#endif
/* The receive hook below also preserves the existing contention event. */
#if !VA_NEEDS_BLOCKING_HOOK && defined(traceBLOCKING_ON_QUEUE_RECEIVE)
#error "ViewAlyzer: queue receive blocking trace macro is already defined"
#endif
#undef traceBLOCKING_ON_QUEUE_RECEIVE
#define traceBLOCKING_ON_QUEUE_RECEIVE(q) do { va_freertos_block((void *)(q), false); va_logQueueObjectBlocking((void *)(q)); } while (0)
#define traceBLOCKING_ON_QUEUE_SEND(q) va_freertos_block((void *)(q), true)
#define traceBLOCKING_ON_QUEUE_PEEK(q) va_freertos_block((void *)(q), false)
#if defined(traceEVENT_GROUP_WAIT_BITS_BLOCK) || defined(traceEVENT_GROUP_SYNC_BLOCK)
#error "ViewAlyzer: event-group blocking trace macros are already defined"
#endif
#define traceEVENT_GROUP_WAIT_BITS_BLOCK(g, bits) va_freertos_wait(VA_WAIT_EVENT_FLAGS, (void *)(g), VA_OBJECT_TYPE_EVENTFLAG, (uint32_t)(bits))
#define traceEVENT_GROUP_SYNC_BLOCK(g, set, wait) va_freertos_wait(VA_WAIT_EVENT_FLAGS, (void *)(g), VA_OBJECT_TYPE_EVENTFLAG, (uint32_t)(wait))
#endif

#if VA_HAS_EVENT_FLAG_DETAILS
#if VA_TRACE_TASK_STATES
#undef traceEVENT_GROUP_WAIT_BITS_BLOCK
#undef traceEVENT_GROUP_SYNC_BLOCK
#define VA_FLAGS_BLOCK_STATE(g, bits) va_freertos_wait(VA_WAIT_EVENT_FLAGS, (g), VA_OBJECT_TYPE_EVENTFLAG, (bits))
#else
#if defined(traceEVENT_GROUP_WAIT_BITS_BLOCK) || defined(traceEVENT_GROUP_SYNC_BLOCK)
#error "ViewAlyzer: event-group block trace macro already defined"
#endif
#define VA_FLAGS_BLOCK_STATE(g, bits) ((void)0)
#endif
#define traceEVENT_GROUP_WAIT_BITS_BLOCK(g, bits) \
    do { VA_FLAGS_BLOCK_STATE(g, bits); \
        va_freertos_flags(g, VA_FLAGS_WAIT_BEGIN | (xWaitForAllBits ? VA_FLAGS_ALL : 0) | \
            (xClearOnExit ? VA_FLAGS_CLEAR_ON_EXIT : 0), 0, bits); } while (0)
#define traceEVENT_GROUP_SYNC_BLOCK(g, set, wait) \
    do { VA_FLAGS_BLOCK_STATE(g, wait); \
        va_freertos_flags(g, VA_FLAGS_WAIT_BEGIN | VA_FLAGS_ALL | VA_FLAGS_CLEAR_ON_EXIT, 0, wait); } while (0)
#endif

/* stream_buffer.c calls these hooks for both stream and message buffers.
   Native fields are read HERE, inside the kernel TU, never through a guessed
   structure layout in the adapter. xLength includes the ring's spare byte. */
#if VA_TRACE_STREAM_BUFFERS || VA_TRACE_TASK_STATES
#if defined(traceSTREAM_BUFFER_CREATE) || defined(traceSTREAM_BUFFER_SEND) || defined(traceSTREAM_BUFFER_RECEIVE) || defined(traceBLOCKING_ON_STREAM_BUFFER_SEND) || defined(traceBLOCKING_ON_STREAM_BUFFER_RECEIVE)
#error "ViewAlyzer: stream-buffer trace macros are already defined"
#endif
#ifdef __cplusplus
extern "C" {
#endif
void va_freertos_stream(void *buffer, bool message, uint32_t capacity,
                        uint8_t operation, uint32_t transferred, uint32_t requested);
#ifdef __cplusplus
}
#endif
#define VA_FREERTOS_STREAM(b, op, n, requested) \
    va_freertos_stream((void *)(b), ((((StreamBuffer_t *)(b))->ucFlags & sbFLAGS_IS_MESSAGE_BUFFER) != 0), \
        (uint32_t)(((StreamBuffer_t *)(b))->xLength - 1), (op), (uint32_t)(n), (uint32_t)(requested))
#define traceSTREAM_BUFFER_CREATE(b, type) VA_FREERTOS_STREAM(b, 0, 0, 0)
#define traceSTREAM_BUFFER_DELETE(b) va_logQueueObjectDelete((void *)(b))
#define traceSTREAM_BUFFER_RESET(b) VA_FREERTOS_STREAM(b, VA_OP_RESET, 0, 0)
#define traceSTREAM_BUFFER_RESET_FROM_ISR(b) VA_FREERTOS_STREAM(b, VA_OP_RESET, 0, 0)
#define traceSTREAM_BUFFER_SEND(b, n) VA_FREERTOS_STREAM(b, VA_OP_SEND, n, xDataLengthBytes)
#define traceSTREAM_BUFFER_SEND_FAILED(b) VA_FREERTOS_STREAM(b, VA_OP_SEND_FAILED, 0, xDataLengthBytes)
#define traceSTREAM_BUFFER_SEND_FROM_ISR(b, n) VA_FREERTOS_STREAM(b, (n) != 0 ? VA_OP_SEND : VA_OP_SEND_FAILED, n, xDataLengthBytes)
#define traceSTREAM_BUFFER_RECEIVE(b, n) VA_FREERTOS_STREAM(b, VA_OP_RECEIVE, n, xBufferLengthBytes)
#define traceSTREAM_BUFFER_RECEIVE_FAILED(b) VA_FREERTOS_STREAM(b, VA_OP_RECEIVE_FAILED, 0, xBufferLengthBytes)
#define traceSTREAM_BUFFER_RECEIVE_FROM_ISR(b, n) VA_FREERTOS_STREAM(b, (n) != 0 ? VA_OP_RECEIVE : VA_OP_RECEIVE_FAILED, n, xBufferLengthBytes)
#define traceBLOCKING_ON_STREAM_BUFFER_SEND(b) VA_FREERTOS_STREAM(b, VA_OP_WAIT_BEGIN, 0, xDataLengthBytes)
#define traceBLOCKING_ON_STREAM_BUFFER_RECEIVE(b) VA_FREERTOS_STREAM(b, VA_OP_WAIT_BEGIN, 1, xBufferLengthBytes)
#endif

#endif /* VA_ENABLED && VA_RTOS_FREERTOS */

#endif /* __ASSEMBLER__ */

#endif /* VIEWALYZER_FREERTOS_HOOK_COMMON_H */
