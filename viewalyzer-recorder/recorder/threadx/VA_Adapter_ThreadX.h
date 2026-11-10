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
#ifndef VA_ADAPTER_THREADX_H
#define VA_ADAPTER_THREADX_H
#include "VA_ThreadXConfig.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    VA_TX_THREAD_DELETE,
    VA_TX_THREAD_TERMINATE,
    VA_TX_THREAD_RESET,
    VA_TX_THREAD_RESUME,
    VA_TX_THREAD_SUSPEND,
    VA_TX_THREAD_SLEEP,
    VA_TX_THREAD_PRIORITY_CHANGE,
    VA_TX_QUEUE_CREATE,
    VA_TX_QUEUE_DELETE,
    VA_TX_QUEUE_SEND,
    VA_TX_QUEUE_FRONT_SEND,
    VA_TX_QUEUE_RECEIVE,
    VA_TX_QUEUE_FLUSH,
    VA_TX_MUTEX_CREATE,
    VA_TX_MUTEX_DELETE,
    VA_TX_MUTEX_GET,
    VA_TX_MUTEX_PUT,
    VA_TX_SEMAPHORE_CREATE,
    VA_TX_SEMAPHORE_DELETE,
    VA_TX_SEMAPHORE_GET,
    VA_TX_SEMAPHORE_PUT,
    VA_TX_SEMAPHORE_CEILING_PUT,
    VA_TX_EVENT_FLAGS_CREATE,
    VA_TX_EVENT_FLAGS_DELETE,
    VA_TX_EVENT_FLAGS_GET,
    VA_TX_EVENT_FLAGS_SET,
    VA_TX_TIMER_CREATE,
    VA_TX_TIMER_DELETE,
    VA_TX_TIMER_ACTIVATE,
    VA_TX_TIMER_DEACTIVATE,
    VA_TX_TIMER_CHANGE,
    VA_TX_BYTE_POOL_CREATE,
    VA_TX_BYTE_POOL_DELETE,
    VA_TX_BYTE_ALLOCATE,
    VA_TX_BYTE_RELEASE,
    VA_TX_BLOCK_POOL_CREATE,
    VA_TX_BLOCK_POOL_DELETE,
    VA_TX_BLOCK_ALLOCATE,
    VA_TX_BLOCK_RELEASE
};
void *va_threadx_api_begin(unsigned int operation, void *object, uintptr_t a, uintptr_t b, uintptr_t c);
void va_threadx_api_end(unsigned int operation, void *object, uintptr_t a, uintptr_t b, uintptr_t c, unsigned int status);
/* Blocking calls may return after delete has retired (or reused) the handle.
 * Keep the original wire identity on the caller's stack, not in a global map. */
typedef struct {
    void *object;
    uint16_t id;
    uint16_t slot; /* retained RAM slot + 1, independent of the wire ID */
    uint32_t generation;
    bool ownsReference;
} VA_ThreadXWait;
VA_ThreadXWait va_threadx_wait_begin(unsigned int operation, void *object, uintptr_t a, uintptr_t b, uintptr_t c);
void va_threadx_wait_end(unsigned int operation, VA_ThreadXWait context, uintptr_t a, uintptr_t b, uintptr_t c, unsigned int status);
void VA_ThreadX_ISR_Enter(void);
void VA_ThreadX_ISR_Exit(void);
void VA_ThreadX_TimerEnter(void *timer);
void VA_ThreadX_TimerExit(void *timer);
#ifdef __cplusplus
}
#endif
#endif
