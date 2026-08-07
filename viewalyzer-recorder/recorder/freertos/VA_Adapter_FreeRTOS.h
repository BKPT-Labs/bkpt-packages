/**
 * @file VA_Adapter_FreeRTOS.h
 * @brief ViewAlyzer FreeRTOS Adapter - declarations
 *
 * This header is included automatically by the core when
 * VA_RTOS_SELECT == VA_RTOS_FREERTOS.  User code does not need
 * to include it directly.
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

#ifndef VA_ADAPTER_FREERTOS_H
#define VA_ADAPTER_FREERTOS_H

#include "ViewAlyzer.h"

#if (VA_ENABLED == 1) && (VA_RTOS_SELECT == VA_RTOS_FREERTOS)

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The adapter interface functions are declared in ViewAlyzer.h under
 * the VA_HAS_RTOS guard.  This file provides the FreeRTOS-specific
 * implementations.  Nothing additional needs to be forward-declared here
 * beyond what ViewAlyzer.h already exposes.
 */

#ifdef __cplusplus
}
#endif

#endif /* VA_ENABLED && FREERTOS */
#endif /* VA_ADAPTER_FREERTOS_H */
