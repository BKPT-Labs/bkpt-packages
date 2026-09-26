/**
 * @file ViewAlyzerFreeRTOSHook_V10_4_Plus.h
 * @brief ViewAlyzer trace hooks for FreeRTOS v10.4.0 and later
 *
 * Include from the BOTTOM of your FreeRTOSConfig.h (after
 * configUSE_TRACE_FACILITY and the INCLUDE_* options).
 *
 * v10.4 added the notification-index parameter to the task notification
 * trace macros; that is the only difference from the pre-10.4 header.
 * Everything else lives in ViewAlyzerFreeRTOSHook_Common.h.
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

#ifndef VIEWALYZER_FREERTOS_HOOK_V10_4_PLUS_H
#define VIEWALYZER_FREERTOS_HOOK_V10_4_PLUS_H

#ifdef VIEWALYZER_FREERTOS_HOOK_PRE_10_4_H
#error "ViewAlyzer: include only ONE hook header - this FreeRTOSConfig.h already includes ViewAlyzerFreeRTOSHook.h"
#endif

#ifndef __ASSEMBLER__

#include "ViewAlyzerFreeRTOSHook_Common.h"

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_TASK_NOTIFICATIONS

#if defined(traceTASK_NOTIFY) || defined(traceTASK_NOTIFY_TAKE) || defined(traceTASK_NOTIFY_WAIT)
#error "ViewAlyzer: a task-notification trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

/* The _BLOCK variants fire before the notification arrives. The optional
   detail layer records them as wait starts, never as successful receives.
   All give variants report the post-update notified value (the macros fire
   after the kernel applies the action), so task-context and ISR-context
   sends of the same notification report the same thing. */
#if VA_HAS_NOTIFICATION_DETAILS
/* The kernel has applied the send action. Wait/take hooks precede clearing;
   capture their actual result instead of treating every return as success. */
#define traceTASK_NOTIFY(index) va_freertos_notify((void *)pxTCB, (void *)pxCurrentTCB, xReturn == pdPASS ? (uint8_t)eAction + 1 : VA_NOTIFY_FAILED, pxTCB->ulNotifiedValue[(index)], (index))
#define traceTASK_NOTIFY_FROM_ISR(index) va_freertos_notify((void *)pxTCB, NULL, xReturn == pdPASS ? (uint8_t)eAction + 1 : VA_NOTIFY_FAILED, pxTCB->ulNotifiedValue[(index)], (index))
#define traceTASK_NOTIFY_GIVE_FROM_ISR(index) va_freertos_notify((void *)pxTCB, NULL, VA_NOTIFY_INCREMENT, pxTCB->ulNotifiedValue[(index)], (index))
#define traceTASK_NOTIFY_TAKE(index) va_freertos_notify((void *)pxCurrentTCB, (void *)pxCurrentTCB, pxCurrentTCB->ulNotifiedValue[(index)] == 0 ? VA_NOTIFY_TAKE_TIMEOUT : (xClearCountOnExit ? VA_NOTIFY_TAKE_CLEAR : VA_NOTIFY_TAKE_DECREMENT), pxCurrentTCB->ulNotifiedValue[(index)], (index))
#define traceTASK_NOTIFY_WAIT(index) \
    do { va_freertos_notify((void *)pxCurrentTCB, (void *)pxCurrentTCB, VA_NOTIFY_WAIT_CLEAR, ulBitsToClearOnExit, (index)); \
         va_freertos_notify((void *)pxCurrentTCB, (void *)pxCurrentTCB, pxCurrentTCB->ucNotifyState[(index)] == taskNOTIFICATION_RECEIVED ? VA_NOTIFY_WAIT_OK : VA_NOTIFY_WAIT_TIMEOUT, pxCurrentTCB->ulNotifiedValue[(index)], (index)); } while (0)
#else
#define traceTASK_NOTIFY(uxIndexToNotify) va_logtasknotifygive((void *)pxCurrentTCB, (void *)pxTCB, pxTCB->ulNotifiedValue[(uxIndexToNotify)])
#define traceTASK_NOTIFY_FROM_ISR(uxIndexToNotify) va_logtasknotifygive(NULL, (void *)pxTCB, pxTCB->ulNotifiedValue[(uxIndexToNotify)])
#define traceTASK_NOTIFY_GIVE_FROM_ISR(uxIndexToNotify) va_logtasknotifygive(NULL, (void *)pxTCB, pxTCB->ulNotifiedValue[(uxIndexToNotify)])
#define traceTASK_NOTIFY_TAKE(uxIndexToWait) va_logtasknotifytake((void *)pxCurrentTCB, pxCurrentTCB->ulNotifiedValue[(uxIndexToWait)])
#define traceTASK_NOTIFY_WAIT(uxIndexToWait) va_logtasknotifytake((void *)pxCurrentTCB, pxCurrentTCB->ulNotifiedValue[(uxIndexToWait)])

#endif

#endif /* notifications */


#if VA_ENABLED && (VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_TASK_STATES
#if defined(traceTASK_NOTIFY_TAKE_BLOCK) || defined(traceTASK_NOTIFY_WAIT_BLOCK)
#error "ViewAlyzer: notification blocking trace macros are already defined"
#endif
#define traceTASK_NOTIFY_TAKE_BLOCK(index) va_freertos_wait(VA_WAIT_NOTIFICATION, NULL, VA_OBJECT_TYPE_QUEUE, (uint32_t)(index))
#define traceTASK_NOTIFY_WAIT_BLOCK(index) va_freertos_wait(VA_WAIT_NOTIFICATION, NULL, VA_OBJECT_TYPE_QUEUE, (uint32_t)(index))
#endif

#if VA_ENABLED && VA_HAS_NOTIFICATION_DETAILS
#if VA_TRACE_TASK_STATES
#undef traceTASK_NOTIFY_TAKE_BLOCK
#undef traceTASK_NOTIFY_WAIT_BLOCK
#define VA_NOTIFY_BLOCK_STATE(i) va_freertos_wait(VA_WAIT_NOTIFICATION, NULL, VA_OBJECT_TYPE_QUEUE, (i))
#else
#if defined(traceTASK_NOTIFY_TAKE_BLOCK) || defined(traceTASK_NOTIFY_WAIT_BLOCK)
#error "ViewAlyzer: notification block trace macro already defined"
#endif
#define VA_NOTIFY_BLOCK_STATE(i) ((void)0)
#endif
#define traceTASK_NOTIFY_TAKE_BLOCK(index) \
    do { VA_NOTIFY_BLOCK_STATE((index)); va_freertos_notify((void *)pxCurrentTCB, (void *)pxCurrentTCB, VA_NOTIFY_WAIT_BEGIN, pxCurrentTCB->ulNotifiedValue[(index)], (index)); } while (0)
#define traceTASK_NOTIFY_WAIT_BLOCK(index) traceTASK_NOTIFY_TAKE_BLOCK(index)
#endif

#endif /* __ASSEMBLER__ */

#endif /* VIEWALYZER_FREERTOS_HOOK_V10_4_PLUS_H */
