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
/* Every kernel hook and public API wrapper names a configuration-specific
 * symbol. Different recorder configurations across app/kernel/core fail at
 * link time instead of silently omitting events. No runtime branch is needed. */
#ifndef VA_THREADX_CONFIG_H
#define VA_THREADX_CONFIG_H
#include "ViewAlyzerConfig.h"
#if VA_ENABLED
#define VA_TX_C0 1
#else
#define VA_TX_C0 0
#endif
#if VA_TRACE_TASKS
#define VA_TX_C1 1
#else
#define VA_TX_C1 0
#endif
#if VA_TRACE_TASK_STATES
#define VA_TX_C2 1
#else
#define VA_TX_C2 0
#endif
#if VA_TRACE_STACK_USAGE
#define VA_TX_C3 1
#else
#define VA_TX_C3 0
#endif
#if VA_TRACE_SLEEP
#define VA_TX_C4 1
#else
#define VA_TX_C4 0
#endif
#if VA_TRACE_ISRS
#define VA_TX_C5 1
#else
#define VA_TX_C5 0
#endif
#if VA_TRACE_MUTEXES
#define VA_TX_C6 1
#else
#define VA_TX_C6 0
#endif
#if VA_TRACE_MUTEX_CONTENTION
#define VA_TX_C7 1
#else
#define VA_TX_C7 0
#endif
#if VA_TRACE_SEMAPHORES
#define VA_TX_C8 1
#else
#define VA_TX_C8 0
#endif
#if VA_TRACE_QUEUES
#define VA_TX_C9 1
#else
#define VA_TX_C9 0
#endif
#if VA_TRACE_EVENT_FLAGS
#define VA_TX_C10 1
#else
#define VA_TX_C10 0
#endif
#if VA_TRACE_TIMERS
#define VA_TX_C11 1
#else
#define VA_TX_C11 0
#endif
#if VA_TRACE_TIMER_CALLBACKS
#define VA_TX_C12 1
#else
#define VA_TX_C12 0
#endif
#if VA_TRACE_RTOS_HEAPS
#define VA_TX_C13 1
#else
#define VA_TX_C13 0
#endif
#if VA_TRACE_MEM_SLABS
#define VA_TX_C14 1
#else
#define VA_TX_C14 0
#endif
#if VA_TRACE_QUEUE_DETAILS
#define VA_TX_C15 1
#else
#define VA_TX_C15 0
#endif
#if VA_TRACE_EVENT_FLAG_DETAILS
#define VA_TX_C16 1
#else
#define VA_TX_C16 0
#endif
#if VA_METADATA
#define VA_TX_C17 1
#else
#define VA_TX_C17 0
#endif
#if VA_TRANSPORT_BUFFERED
#define VA_TX_C18 1
#else
#define VA_TX_C18 0
#endif
#if VA_ALLOWED_TO_DISABLE_INTERRUPTS
#define VA_TX_C19 1
#else
#define VA_TX_C19 0
#endif
#if VA_TRACE_TASK_NOTIFICATIONS
#define VA_TX_C20 1
#else
#define VA_TX_C20 0
#endif
#if VA_NEEDS_RTOS_OPERATIONS
#define VA_TX_C21 1
#else
#define VA_TX_C21 0
#endif
#define VA_TX_SYMBOL_INNER(p,r,t,s,n,c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19,c20,c21) p##_r##r##_t##t##_s##s##_n##n##_c##c0##c1##c2##c3##c4##c5##c6##c7##c8##c9##c10##c11##c12##c13##c14##c15##c16##c17##c18##c19##c20##c21
#define VA_TX_SYMBOL_EXPAND(p,r,t,s,n,c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19,c20,c21) VA_TX_SYMBOL_INNER(p,r,t,s,n,c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19,c20,c21)
#define VA_TX_SYMBOL(p) VA_TX_SYMBOL_EXPAND(p,VA_RTOS_SELECT,VA_TRANSPORT,VA_TIMESTAMP_SOURCE,VA_MAX_TASK_NAME_LEN,VA_TX_C0,VA_TX_C1,VA_TX_C2,VA_TX_C3,VA_TX_C4,VA_TX_C5,VA_TX_C6,VA_TX_C7,VA_TX_C8,VA_TX_C9,VA_TX_C10,VA_TX_C11,VA_TX_C12,VA_TX_C13,VA_TX_C14,VA_TX_C15,VA_TX_C16,VA_TX_C17,VA_TX_C18,VA_TX_C19,VA_TX_C20,VA_TX_C21)
#define va_threadx_native_event VA_TX_SYMBOL(va_threadx_native_event)
#define va_threadx_api_begin VA_TX_SYMBOL(va_threadx_api_begin)
#define va_threadx_api_end VA_TX_SYMBOL(va_threadx_api_end)
#define va_threadx_wait_begin VA_TX_SYMBOL(va_threadx_wait_begin)
#define va_threadx_wait_end VA_TX_SYMBOL(va_threadx_wait_end)
#define va_threadx_core_config VA_TX_SYMBOL(va_threadx_core_config)
#define va_threadx_validate_kernel VA_TX_SYMBOL(va_threadx_validate_kernel)
#endif
