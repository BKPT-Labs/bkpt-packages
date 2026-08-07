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

/* Suggested FreeRTOS config: gives traceTASK_CREATE a real stack depth. */
#ifndef configRECORD_STACK_HIGH_ADDRESS
#define configRECORD_STACK_HIGH_ADDRESS 1
#endif

/* Each section refuses to stomp (or be stomped by) another trace tool's
   macros: a silent redefinition would leave one of the two tools recording
   nothing. */

/* ── Task switches ───────────────────────────────────────────────── */
/* Switch-OUT also takes the stack high-water sample, so it stays installed
   for a stack-usage-only build. */
#if VA_NEEDS_SWITCH_HOOK
#if defined(traceTASK_SWITCHED_IN) || defined(traceTASK_SWITCHED_OUT)
#error "ViewAlyzer: a task-switch trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif
#endif

#if VA_TRACE_TASKS
#define traceTASK_SWITCHED_IN() va_taskswitchedin((void *)pxCurrentTCB)
#endif

#if VA_NEEDS_SWITCH_HOOK
#define traceTASK_SWITCHED_OUT() va_taskswitchedout((void *)pxCurrentTCB)
#endif

/* ── Task creation / deletion ────────────────────────────────────── */
/* Installed whenever any category needs task ids/names. */
#if VA_NEEDS_TASK_REGISTRY

#if defined(traceTASK_CREATE) || defined(traceTASK_DELETE)
#error "ViewAlyzer: a task-lifecycle trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#if (configRECORD_STACK_HIGH_ADDRESS == 1)
#define CALCULATE_STACK_DEPTH(pxTCB) \
    g_task_ulStackDepth = (uint32_t)((pxTCB)->pxEndOfStack - (pxTCB)->pxStack)
#else
#define CALCULATE_STACK_DEPTH(pxTCB) \
    g_task_ulStackDepth = 0
#endif

#define traceTASK_CREATE(pxNewTCB)                                   \
    do                                                               \
    {                                                                \
        g_task_pxStack = (pxNewTCB)->pxStack;                        \
        g_task_uxPriority = (pxNewTCB)->uxPriority;                  \
        g_task_pxEndOfStack = NULL;                                  \
        g_task_uxBasePriority = (pxNewTCB)->uxPriority;              \
        CALCULATE_STACK_DEPTH(pxNewTCB);                             \
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

#if defined(traceQUEUE_CREATE) || defined(traceQUEUE_DELETE) || defined(traceQUEUE_SEND) || defined(traceQUEUE_RECEIVE)
#error "ViewAlyzer: a queue trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

/* traceQUEUE_CREATE fires for mutexes/semaphores too; the recorder
   registers first and corrects the type on traceCREATE_MUTEX. */
#define traceQUEUE_CREATE(pxNewQueue) \
    va_logQueueObjectCreateWithType((pxNewQueue), "Queue")

/* vQueueDelete fires this for queues, mutexes, and semaphores alike;
   the slot is freed so a recycled handle cannot inherit the old name. */
#define traceQUEUE_DELETE(pxQueue) \
    va_logQueueObjectDelete((pxQueue))

#define traceCREATE_MUTEX(pxNewMutex) \
    va_updateQueueObjectType((pxNewMutex), "Mutex")

#define traceQUEUE_SEND(pxQueue) \
    va_logQueueObjectGive((pxQueue), xTicksToWait)

#define traceQUEUE_SEND_FROM_ISR(pxQueue) \
    va_logQueueObjectGive((pxQueue), 0)

#define traceQUEUE_RECEIVE(pxQueue) \
    va_logQueueObjectTake((pxQueue), xTicksToWait)

#define traceQUEUE_RECEIVE_FROM_ISR(pxQueue) \
    va_logQueueObjectTake((pxQueue), 0)

/* Recursive mutexes DO have their own trace points. */
#if VA_TRACE_MUTEXES
#define traceGIVE_MUTEX_RECURSIVE(pxMutex) \
    va_logQueueObjectGive((pxMutex), 0)

#define traceTAKE_MUTEX_RECURSIVE(pxMutex) \
    va_logQueueObjectTake((pxMutex), xTicksToWait)
#endif

/* A failed give/take is not a state change and the wire format has no
   "failed" variant; emit nothing. */
#define traceGIVE_MUTEX_RECURSIVE_FAILED(pxMutex) ((void)(pxMutex))
#define traceTAKE_MUTEX_RECURSIVE_FAILED(pxMutex) ((void)(pxMutex))
#define traceCREATE_MUTEX_FAILED()                ((void)0)

/* Covered by traceQUEUE_CREATE (created through xQueueGenericCreate). */
#define traceCREATE_COUNTING_SEMAPHORE() ((void)0)
#define traceCREATE_BINARY_SEMAPHORE()   ((void)0)

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

#endif /* VA_ENABLED && VA_RTOS_FREERTOS */

#endif /* __ASSEMBLER__ */

#endif /* VIEWALYZER_FREERTOS_HOOK_COMMON_H */
