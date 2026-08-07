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
uint32_t va_adapter_calculate_stack_usage(void *taskHandle)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    uint32_t free_stack_words = uxTaskGetStackHighWaterMark((TaskHandle_t)taskHandle);
    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0 && taskMap[idx].ulStackDepth > 0)
    {
        uint32_t used_stack_words = taskMap[idx].ulStackDepth - free_stack_words;
        return used_stack_words;
    }
    return free_stack_words;
#else
    (void)taskHandle;
    return 0;
#endif
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
