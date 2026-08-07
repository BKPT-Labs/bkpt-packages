/**
 * @file ViewAlyzerFreeRTOSHook.h
 * @brief ViewAlyzer trace hooks for FreeRTOS before v10.4.0
 *
 * Include from the BOTTOM of your FreeRTOSConfig.h (after
 * configUSE_TRACE_FACILITY and the INCLUDE_* options).
 *
 * The pre-10.4 task notification trace macros take no notification index,
 * and ulNotifiedValue is a scalar rather than an array; that is the only
 * difference from the v10.4+ header. Everything else lives in
 * ViewAlyzerFreeRTOSHook_Common.h.
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

#ifndef ViewAlyzer_CONFIG_H
#define ViewAlyzer_CONFIG_H

#ifndef __ASSEMBLER__

#include "ViewAlyzerFreeRTOSHook_Common.h"

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_TASK_NOTIFICATIONS

#if defined(traceTASK_NOTIFY) || defined(traceTASK_NOTIFY_TAKE) || defined(traceTASK_NOTIFY_WAIT)
#error "ViewAlyzer: a task-notification trace macro is already defined - another trace tool is installed in this FreeRTOSConfig.h"
#endif

#define traceTASK_NOTIFY() va_logtasknotifygive((void *)pxCurrentTCB, (void *)pxTCB, ulValue)
#define traceTASK_NOTIFY_FROM_ISR() va_logtasknotifygive(NULL, (void *)pxTCB, ulValue)
#define traceTASK_NOTIFY_GIVE_FROM_ISR() va_logtasknotifygive(NULL, (void *)pxTCB, pxTCB->ulNotifiedValue)
#define traceTASK_NOTIFY_TAKE() va_logtasknotifytake((void *)pxCurrentTCB, pxCurrentTCB->ulNotifiedValue)
#define traceTASK_NOTIFY_WAIT() va_logtasknotifytake((void *)pxCurrentTCB, pxCurrentTCB->ulNotifiedValue)

#endif /* notifications */

#endif /* __ASSEMBLER__ */

#endif /* ViewAlyzer_CONFIG_H */
