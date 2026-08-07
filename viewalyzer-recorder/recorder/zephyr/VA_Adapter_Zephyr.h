/**
 * @file VA_Adapter_Zephyr.h
 * @brief ViewAlyzer Zephyr Adapter - declarations
 *
 * Bridges Zephyr's CONFIG_TRACING_USER weak callbacks to the ViewAlyzer
 * binary recorder protocol.  Include this header from your application.
 *
 * Traced events:
 *   - Thread create          → registers task name + ID
 *   - Thread switch in / out → native VA_EVENT_TASK_SWITCH events
 *   - ISR enter / exit       → VA_LogISRStart / VA_LogISREnd
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

#ifndef VA_ADAPTER_ZEPHYR_H
#define VA_ADAPTER_ZEPHYR_H

#include "ViewAlyzer.h"

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_ZEPHYR)

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Emit ViewAlyzer setup packets for all threads that were created
 *        before VA_Init() ran (static K_THREAD_DEFINE threads, idle, etc.).
 *
 * Call once after VA_Init() so the host knows the task-name → id mapping.
 *
 * Compiles to nothing when no enabled trace category needs thread ids, so
 * the call can stay in your application unconditionally.
 */
#if VA_NEEDS_TASK_REGISTRY
void VA_Zephyr_RegisterExistingThreads(void);
#else
#define VA_Zephyr_RegisterExistingThreads() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* VA_ENABLED && ZEPHYR */
#endif /* VA_ADAPTER_ZEPHYR_H */
