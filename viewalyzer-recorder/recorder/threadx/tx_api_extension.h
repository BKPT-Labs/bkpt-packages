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
/* Official tx_api.h extension point. Wrappers retain the selected upstream
 * error-checking API and evaluate each argument once. Kernel-internal calls
 * are intentionally not reported as completed public API operations. */
#ifndef VA_TX_API_EXTENSION_H
#define VA_TX_API_EXTENSION_H
#include "ViewAlyzerConfig.h"
#if VA_ENABLED && (VA_RTOS_SELECT == VA_RTOS_THREADX)
#include "VA_Adapter_ThreadX.h"
#if !defined(VA_THREADX_IMPLEMENTATION) && !defined(TX_SOURCE_CODE)
#ifndef TX_NAME_CONST
#define TX_NAME_CONST
#endif
static inline UINT va_tx_thread_delete(TX_THREAD *thread_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_THREAD_DELETE, (void *)thread_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_thread_delete(thread_ptr);
    va_threadx_api_end(VA_TX_THREAD_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_thread_delete
#define tx_thread_delete va_tx_thread_delete

static inline UINT va_tx_thread_terminate(TX_THREAD *thread_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_THREAD_TERMINATE, (void *)thread_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_thread_terminate(thread_ptr);
    va_threadx_api_end(VA_TX_THREAD_TERMINATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_thread_terminate
#define tx_thread_terminate va_tx_thread_terminate

static inline UINT va_tx_thread_reset(TX_THREAD *thread_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_THREAD_RESET, (void *)thread_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_thread_reset(thread_ptr);
    va_threadx_api_end(VA_TX_THREAD_RESET, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_thread_reset
#define tx_thread_reset va_tx_thread_reset

static inline UINT va_tx_thread_resume(TX_THREAD *thread_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_THREAD_RESUME, (void *)thread_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_thread_resume(thread_ptr);
    va_threadx_api_end(VA_TX_THREAD_RESUME, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_thread_resume
#define tx_thread_resume va_tx_thread_resume

static inline UINT va_tx_thread_suspend(TX_THREAD *thread_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_THREAD_SUSPEND, (void *)thread_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_thread_suspend(thread_ptr);
    va_threadx_api_end(VA_TX_THREAD_SUSPEND, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_thread_suspend
#define tx_thread_suspend va_tx_thread_suspend

static inline UINT va_tx_thread_sleep(ULONG timer_ticks)
{
    void *context = va_threadx_api_begin(VA_TX_THREAD_SLEEP, (void *)0, (uintptr_t)timer_ticks, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_thread_sleep(timer_ticks);
    va_threadx_api_end(VA_TX_THREAD_SLEEP, context, (uintptr_t)timer_ticks, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_thread_sleep
#define tx_thread_sleep va_tx_thread_sleep

static inline UINT va_tx_thread_priority_change(TX_THREAD *thread_ptr, UINT new_priority, UINT *old_priority)
{
    void *context = va_threadx_api_begin(VA_TX_THREAD_PRIORITY_CHANGE, (void *)thread_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_thread_priority_change(thread_ptr, new_priority, old_priority);
    va_threadx_api_end(VA_TX_THREAD_PRIORITY_CHANGE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_thread_priority_change
#define tx_thread_priority_change va_tx_thread_priority_change

static inline UINT va_tx_queue_create(TX_QUEUE *queue_ptr, TX_NAME_CONST CHAR *name_ptr, UINT message_size, VOID *queue_start, ULONG queue_size)
{
    void *context = va_threadx_api_begin(VA_TX_QUEUE_CREATE, (void *)queue_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_queue_create(queue_ptr, name_ptr, message_size, queue_start, queue_size);
    va_threadx_api_end(VA_TX_QUEUE_CREATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_queue_create
#define tx_queue_create va_tx_queue_create

static inline UINT va_tx_queue_delete(TX_QUEUE *queue_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_QUEUE_DELETE, (void *)queue_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_queue_delete(queue_ptr);
    va_threadx_api_end(VA_TX_QUEUE_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_queue_delete
#define tx_queue_delete va_tx_queue_delete

static inline UINT va_tx_queue_send(TX_QUEUE *queue_ptr, VOID *source_ptr, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_QUEUE_SEND, (void *)queue_ptr, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_queue_send(queue_ptr, source_ptr, wait_option);
    va_threadx_wait_end(VA_TX_QUEUE_SEND, context, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_queue_send
#define tx_queue_send va_tx_queue_send

static inline UINT va_tx_queue_front_send(TX_QUEUE *queue_ptr, VOID *source_ptr, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_QUEUE_FRONT_SEND, (void *)queue_ptr, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_queue_front_send(queue_ptr, source_ptr, wait_option);
    va_threadx_wait_end(VA_TX_QUEUE_FRONT_SEND, context, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_queue_front_send
#define tx_queue_front_send va_tx_queue_front_send

static inline UINT va_tx_queue_receive(TX_QUEUE *queue_ptr, VOID *destination_ptr, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_QUEUE_RECEIVE, (void *)queue_ptr, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_queue_receive(queue_ptr, destination_ptr, wait_option);
    va_threadx_wait_end(VA_TX_QUEUE_RECEIVE, context, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_queue_receive
#define tx_queue_receive va_tx_queue_receive

static inline UINT va_tx_queue_flush(TX_QUEUE *queue_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_QUEUE_FLUSH, (void *)queue_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_queue_flush(queue_ptr);
    va_threadx_api_end(VA_TX_QUEUE_FLUSH, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_queue_flush
#define tx_queue_flush va_tx_queue_flush

static inline UINT va_tx_mutex_create(TX_MUTEX *mutex_ptr, TX_NAME_CONST CHAR *name_ptr, UINT inherit)
{
    void *context = va_threadx_api_begin(VA_TX_MUTEX_CREATE, (void *)mutex_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_mutex_create(mutex_ptr, name_ptr, inherit);
    va_threadx_api_end(VA_TX_MUTEX_CREATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_mutex_create
#define tx_mutex_create va_tx_mutex_create

static inline UINT va_tx_mutex_delete(TX_MUTEX *mutex_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_MUTEX_DELETE, (void *)mutex_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_mutex_delete(mutex_ptr);
    va_threadx_api_end(VA_TX_MUTEX_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_mutex_delete
#define tx_mutex_delete va_tx_mutex_delete

static inline UINT va_tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_MUTEX_GET, (void *)mutex_ptr, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_mutex_get(mutex_ptr, wait_option);
    va_threadx_wait_end(VA_TX_MUTEX_GET, context, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_mutex_get
#define tx_mutex_get va_tx_mutex_get

static inline UINT va_tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_MUTEX_PUT, (void *)mutex_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_mutex_put(mutex_ptr);
    va_threadx_api_end(VA_TX_MUTEX_PUT, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_mutex_put
#define tx_mutex_put va_tx_mutex_put

static inline UINT va_tx_semaphore_create(TX_SEMAPHORE *semaphore_ptr, TX_NAME_CONST CHAR *name_ptr, ULONG initial_count)
{
    void *context = va_threadx_api_begin(VA_TX_SEMAPHORE_CREATE, (void *)semaphore_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_semaphore_create(semaphore_ptr, name_ptr, initial_count);
    va_threadx_api_end(VA_TX_SEMAPHORE_CREATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_semaphore_create
#define tx_semaphore_create va_tx_semaphore_create

static inline UINT va_tx_semaphore_delete(TX_SEMAPHORE *semaphore_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_SEMAPHORE_DELETE, (void *)semaphore_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_semaphore_delete(semaphore_ptr);
    va_threadx_api_end(VA_TX_SEMAPHORE_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_semaphore_delete
#define tx_semaphore_delete va_tx_semaphore_delete

static inline UINT va_tx_semaphore_get(TX_SEMAPHORE *semaphore_ptr, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_SEMAPHORE_GET, (void *)semaphore_ptr, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_semaphore_get(semaphore_ptr, wait_option);
    va_threadx_wait_end(VA_TX_SEMAPHORE_GET, context, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_semaphore_get
#define tx_semaphore_get va_tx_semaphore_get

static inline UINT va_tx_semaphore_put(TX_SEMAPHORE *semaphore_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_SEMAPHORE_PUT, (void *)semaphore_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_semaphore_put(semaphore_ptr);
    va_threadx_api_end(VA_TX_SEMAPHORE_PUT, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_semaphore_put
#define tx_semaphore_put va_tx_semaphore_put

static inline UINT va_tx_semaphore_ceiling_put(TX_SEMAPHORE *semaphore_ptr, ULONG ceiling)
{
    void *context = va_threadx_api_begin(VA_TX_SEMAPHORE_CEILING_PUT, (void *)semaphore_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_semaphore_ceiling_put(semaphore_ptr, ceiling);
    va_threadx_api_end(VA_TX_SEMAPHORE_CEILING_PUT, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_semaphore_ceiling_put
#define tx_semaphore_ceiling_put va_tx_semaphore_ceiling_put

static inline UINT va_tx_event_flags_create(TX_EVENT_FLAGS_GROUP *group_ptr, TX_NAME_CONST CHAR *name_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_EVENT_FLAGS_CREATE, (void *)group_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_event_flags_create(group_ptr, name_ptr);
    va_threadx_api_end(VA_TX_EVENT_FLAGS_CREATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_event_flags_create
#define tx_event_flags_create va_tx_event_flags_create

static inline UINT va_tx_event_flags_delete(TX_EVENT_FLAGS_GROUP *group_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_EVENT_FLAGS_DELETE, (void *)group_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_event_flags_delete(group_ptr);
    va_threadx_api_end(VA_TX_EVENT_FLAGS_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_event_flags_delete
#define tx_event_flags_delete va_tx_event_flags_delete

static inline UINT va_tx_event_flags_get(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG requested_flags, UINT get_option, ULONG *actual_flags_ptr, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_EVENT_FLAGS_GET, (void *)group_ptr, (uintptr_t)wait_option, (uintptr_t)requested_flags, (uintptr_t)get_option);
    UINT status = tx_event_flags_get(group_ptr, requested_flags, get_option, actual_flags_ptr, wait_option);
    va_threadx_wait_end(VA_TX_EVENT_FLAGS_GET, context, (uintptr_t)get_option, (uintptr_t)requested_flags, (uintptr_t)(status == TX_SUCCESS ? *actual_flags_ptr : 0), status);
    return status;
}
#undef tx_event_flags_get
#define tx_event_flags_get va_tx_event_flags_get

static inline UINT va_tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set, UINT set_option)
{
    void *context = va_threadx_api_begin(VA_TX_EVENT_FLAGS_SET, (void *)group_ptr, (uintptr_t)flags_to_set, (uintptr_t)set_option, (uintptr_t)0);
    UINT status = tx_event_flags_set(group_ptr, flags_to_set, set_option);
    va_threadx_api_end(VA_TX_EVENT_FLAGS_SET, context, (uintptr_t)flags_to_set, (uintptr_t)set_option, (uintptr_t)0, status);
    return status;
}
#undef tx_event_flags_set
#define tx_event_flags_set va_tx_event_flags_set

static inline UINT va_tx_timer_create(TX_TIMER *timer_ptr, TX_NAME_CONST CHAR *name_ptr, VOID (*expiration_function)(ULONG input), ULONG expiration_input, ULONG initial_ticks, ULONG reschedule_ticks, UINT auto_activate)
{
    void *context = va_threadx_api_begin(VA_TX_TIMER_CREATE, (void *)timer_ptr, (uintptr_t)auto_activate, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_timer_create(timer_ptr, name_ptr, expiration_function, expiration_input, initial_ticks, reschedule_ticks, auto_activate);
    va_threadx_api_end(VA_TX_TIMER_CREATE, context, (uintptr_t)auto_activate, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_timer_create
#define tx_timer_create va_tx_timer_create

static inline UINT va_tx_timer_delete(TX_TIMER *timer_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_TIMER_DELETE, (void *)timer_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_timer_delete(timer_ptr);
    va_threadx_api_end(VA_TX_TIMER_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_timer_delete
#define tx_timer_delete va_tx_timer_delete

static inline UINT va_tx_timer_activate(TX_TIMER *timer_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_TIMER_ACTIVATE, (void *)timer_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_timer_activate(timer_ptr);
    va_threadx_api_end(VA_TX_TIMER_ACTIVATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_timer_activate
#define tx_timer_activate va_tx_timer_activate

static inline UINT va_tx_timer_deactivate(TX_TIMER *timer_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_TIMER_DEACTIVATE, (void *)timer_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_timer_deactivate(timer_ptr);
    va_threadx_api_end(VA_TX_TIMER_DEACTIVATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_timer_deactivate
#define tx_timer_deactivate va_tx_timer_deactivate

static inline UINT va_tx_timer_change(TX_TIMER *timer_ptr, ULONG initial_ticks, ULONG reschedule_ticks)
{
    void *context = va_threadx_api_begin(VA_TX_TIMER_CHANGE, (void *)timer_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_timer_change(timer_ptr, initial_ticks, reschedule_ticks);
    va_threadx_api_end(VA_TX_TIMER_CHANGE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_timer_change
#define tx_timer_change va_tx_timer_change

static inline UINT va_tx_byte_pool_create(TX_BYTE_POOL *pool_ptr, TX_NAME_CONST CHAR *name_ptr, VOID *pool_start, ULONG pool_size)
{
    void *context = va_threadx_api_begin(VA_TX_BYTE_POOL_CREATE, (void *)pool_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_byte_pool_create(pool_ptr, name_ptr, pool_start, pool_size);
    va_threadx_api_end(VA_TX_BYTE_POOL_CREATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_byte_pool_create
#define tx_byte_pool_create va_tx_byte_pool_create

static inline UINT va_tx_byte_pool_delete(TX_BYTE_POOL *pool_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_BYTE_POOL_DELETE, (void *)pool_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_byte_pool_delete(pool_ptr);
    va_threadx_api_end(VA_TX_BYTE_POOL_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_byte_pool_delete
#define tx_byte_pool_delete va_tx_byte_pool_delete

static inline UINT va_tx_byte_allocate(TX_BYTE_POOL *pool_ptr, VOID **memory_ptr, ULONG memory_size, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_BYTE_ALLOCATE, (void *)pool_ptr, (uintptr_t)wait_option, (uintptr_t)memory_size, (uintptr_t)0);
    UINT status = tx_byte_allocate(pool_ptr, memory_ptr, memory_size, wait_option);
    va_threadx_wait_end(VA_TX_BYTE_ALLOCATE, context, (uintptr_t)wait_option, (uintptr_t)memory_size, (uintptr_t)0, status);
    return status;
}
#undef tx_byte_allocate
#define tx_byte_allocate va_tx_byte_allocate

static inline UINT va_tx_byte_release(VOID *memory_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_BYTE_RELEASE, (void *)memory_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_byte_release(memory_ptr);
    va_threadx_api_end(VA_TX_BYTE_RELEASE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_byte_release
#define tx_byte_release va_tx_byte_release

static inline UINT va_tx_block_pool_create(TX_BLOCK_POOL *pool_ptr, TX_NAME_CONST CHAR *name_ptr, ULONG block_size, VOID *pool_start, ULONG pool_size)
{
    void *context = va_threadx_api_begin(VA_TX_BLOCK_POOL_CREATE, (void *)pool_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_block_pool_create(pool_ptr, name_ptr, block_size, pool_start, pool_size);
    va_threadx_api_end(VA_TX_BLOCK_POOL_CREATE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_block_pool_create
#define tx_block_pool_create va_tx_block_pool_create

static inline UINT va_tx_block_pool_delete(TX_BLOCK_POOL *pool_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_BLOCK_POOL_DELETE, (void *)pool_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_block_pool_delete(pool_ptr);
    va_threadx_api_end(VA_TX_BLOCK_POOL_DELETE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_block_pool_delete
#define tx_block_pool_delete va_tx_block_pool_delete

static inline UINT va_tx_block_allocate(TX_BLOCK_POOL *pool_ptr, VOID **block_ptr, ULONG wait_option)
{
    VA_ThreadXWait context = va_threadx_wait_begin(VA_TX_BLOCK_ALLOCATE, (void *)pool_ptr, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_block_allocate(pool_ptr, block_ptr, wait_option);
    va_threadx_wait_end(VA_TX_BLOCK_ALLOCATE, context, (uintptr_t)wait_option, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_block_allocate
#define tx_block_allocate va_tx_block_allocate

static inline UINT va_tx_block_release(VOID *block_ptr)
{
    void *context = va_threadx_api_begin(VA_TX_BLOCK_RELEASE, (void *)block_ptr, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    UINT status = tx_block_release(block_ptr);
    va_threadx_api_end(VA_TX_BLOCK_RELEASE, context, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, status);
    return status;
}
#undef tx_block_release
#define tx_block_release va_tx_block_release


#endif
#endif
#endif
