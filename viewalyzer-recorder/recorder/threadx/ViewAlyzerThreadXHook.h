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
/* Include from tx_user.h. Rebuild the entire kernel and application with
 * TX_INCLUDE_USER_DEFINE_FILE and the same recorder configuration. */
#ifndef VIEWALYZER_THREADX_HOOK_H
#define VIEWALYZER_THREADX_HOOK_H

#include "ViewAlyzerConfig.h"
#include "VA_ThreadXConfig.h"

#if VA_ENABLED && (VA_RTOS_SELECT == VA_RTOS_THREADX)
#if defined(TX_EXECUTION_PROFILE_ENABLE) || defined(TX_MISRA_ENABLE) || defined(TX_SAFETY_CRITICAL)
#error "ViewAlyzer ThreadX hooks require the standard single-core kernel without execution profiling, MISRA or safety wrappers"
#endif
#define TX_ENABLE_EVENT_TRACE
#define TX_ENABLE_EXECUTION_CHANGE_NOTIFY
#define TX_THREAD_API_EXTENSION

#if !defined(__ASSEMBLER__) && !defined(__ASSEMBLY__)
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void va_threadx_native_event(unsigned int event, void *object, uintptr_t a, uintptr_t b, uintptr_t c);
void va_threadx_priority_changed(void *thread);
void VA_ThreadX_ISR_Enter(void);
void VA_ThreadX_ISR_Exit(void);
void VA_ThreadX_TimerEnter(void *timer);
void VA_ThreadX_TimerExit(void *timer);
#ifdef __cplusplus
}
#endif

/* The native hook is an observation BEFORE most API operations. Only
 * validated lifecycle/scheduler observations are decoded here. Public API
 * wrappers below capture completed operations and their real return status.
 * No TraceX buffer is allocated; do not call tx_trace_enable concurrently. */
#ifdef TX_TRACE_IN_LINE_INSERT
#error "ViewAlyzer owns TX_TRACE_IN_LINE_INSERT"
#endif
#define TX_TRACE_IN_LINE_INSERT(i,a,b,c,d,f) \
    va_threadx_native_event((unsigned int)(i), (void *)(a), (uintptr_t)(b), (uintptr_t)(c), (uintptr_t)(d));
#ifdef TX_MUTEX_PRIORITY_CHANGE_EXTENSION
#error "Compose the existing mutex priority extension with ViewAlyzer explicitly"
#endif
#define TX_MUTEX_PRIORITY_CHANGE_EXTENSION va_threadx_priority_changed(thread_ptr);
#endif
#endif
#endif
