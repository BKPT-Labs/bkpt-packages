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

#ifndef ViewAlyzer_CONFIG_V10_4_PLUS_H
#define ViewAlyzer_CONFIG_V10_4_PLUS_H

#ifndef __ASSEMBLER__

#include "ViewAlyzerFreeRTOSHook_Common.h"

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_TASK_NOTIFICATIONS

#if defined(traceTASK_NOTIFY) || defined(traceTASK_NOTIFY_TAKE) || defined(traceTASK_NOTIFY_WAIT)
#error "ViewAlyzer: a task-notification trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

/* The _BLOCK variants fire BEFORE the notification arrives (the task is
   about to block) and are deliberately left unhooked - hooking them would
   double every blocking take/wait with a stale value. */
#define traceTASK_NOTIFY(uxIndexToNotify) va_logtasknotifygive((void *)pxCurrentTCB, (void *)pxTCB, ulValue)
#define traceTASK_NOTIFY_FROM_ISR(uxIndexToNotify) va_logtasknotifygive(NULL, (void *)pxTCB, ulValue)
#define traceTASK_NOTIFY_GIVE_FROM_ISR(uxIndexToNotify) va_logtasknotifygive(NULL, (void *)pxTCB, pxTCB->ulNotifiedValue[(uxIndexToNotify)])
#define traceTASK_NOTIFY_TAKE(uxIndexToWait) va_logtasknotifytake((void *)pxCurrentTCB, pxCurrentTCB->ulNotifiedValue[(uxIndexToWait)])
#define traceTASK_NOTIFY_WAIT(uxIndexToWait) va_logtasknotifytake((void *)pxCurrentTCB, pxCurrentTCB->ulNotifiedValue[(uxIndexToWait)])

#endif /* notifications */

#endif /* __ASSEMBLER__ */

#endif /* ViewAlyzer_CONFIG_V10_4_PLUS_H */
