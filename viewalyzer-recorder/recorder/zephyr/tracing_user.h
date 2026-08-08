/**
 * @file tracing_user.h
 * @brief ViewAlyzer wrapper over Zephyr's stock tracing_user.h
 *
 * Wraps (not replaces) the stock subsys/tracing/user/tracing_user.h:
 * #include_next pulls in Zephyr's own header, then only the kernel-object
 * trace points the recorder records are re-pointed at VA_Adapter_Zephyr.c.
 * Resolved first via the include path set up in CMakeLists.txt; the include
 * guard is deliberately NOT _TRACE_USER_H so #include_next still works.
 *
 * Copyright (c) 2020 Lexmark International, Inc.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc.
 * Copyright 2025-2026 BKPT, Inc. (ViewAlyzer extensions)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VIEWALYZER_TRACING_USER_WRAP_H
#define VIEWALYZER_TRACING_USER_WRAP_H

/* Zephyr's stock header, at the arity of the version being built. */
#include_next <tracing_user.h>

#include <zephyr/kernel.h>
#include <zephyr/version.h>

/* Dependency-free, so safe inside Zephyr's own kernel TUs. */
#include "ViewAlyzerConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Adapter dispatch declarations (defined in VA_Adapter_Zephyr.c) ─── */

#if VA_NEEDS_TASK_REGISTRY
void sys_trace_va_thread_abort(struct k_thread *thread);
#endif

#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION
void sys_trace_k_mutex_init(struct k_mutex *mutex, int ret);
void sys_trace_k_mutex_lock_enter(struct k_mutex *mutex, k_timeout_t timeout);
void sys_trace_k_mutex_lock_blocking(struct k_mutex *mutex, k_timeout_t timeout);
void sys_trace_k_mutex_lock_exit(struct k_mutex *mutex, k_timeout_t timeout, int ret);
void sys_trace_k_mutex_unlock_enter(struct k_mutex *mutex);
void sys_trace_k_mutex_unlock_exit(struct k_mutex *mutex, int ret);
#endif

#if VA_TRACE_SEMAPHORES
void sys_trace_k_sem_init(struct k_sem *sem, int ret);
void sys_trace_k_sem_give_enter(struct k_sem *sem);
void sys_trace_k_sem_take_enter(struct k_sem *sem, k_timeout_t timeout);
void sys_trace_k_sem_take_blocking(struct k_sem *sem, k_timeout_t timeout);
void sys_trace_k_sem_take_exit(struct k_sem *sem, k_timeout_t timeout, int ret);
#endif

#if VA_TRACE_QUEUES
void sys_trace_k_msgq_init(struct k_msgq *msgq);
void sys_trace_k_msgq_put_enter(struct k_msgq *msgq, k_timeout_t timeout);
void sys_trace_k_msgq_put_blocking(struct k_msgq *msgq, k_timeout_t timeout);
void sys_trace_k_msgq_put_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret);
void sys_trace_k_msgq_get_enter(struct k_msgq *msgq, k_timeout_t timeout);
void sys_trace_k_msgq_get_blocking(struct k_msgq *msgq, k_timeout_t timeout);
void sys_trace_k_msgq_get_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret);
void sys_trace_k_fifo_init(struct k_fifo *fifo);
void sys_trace_k_fifo_put(struct k_fifo *fifo);
void sys_trace_k_fifo_alloc_put(struct k_fifo *fifo, int ret);
void sys_trace_k_fifo_get(struct k_fifo *fifo, void *ret);
void sys_trace_k_lifo_init(struct k_lifo *lifo);
void sys_trace_k_lifo_put(struct k_lifo *lifo);
void sys_trace_k_lifo_alloc_put(struct k_lifo *lifo, int ret);
void sys_trace_k_lifo_get(struct k_lifo *lifo, void *ret);
#endif

#if VA_TRACE_EVENT_FLAGS
void sys_trace_k_event_init(struct k_event *event);
void sys_trace_k_event_post(struct k_event *event, uint32_t events);
void sys_trace_k_event_wait_exit(struct k_event *event, uint32_t events, uint32_t ret);
#endif

#if VA_TRACE_WORK
void sys_trace_k_work_submit(struct k_work *work, int ret);
void sys_trace_k_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay, int ret);
void sys_trace_k_work_cancel(struct k_work *work);
void sys_trace_k_work_cancel_delayable(struct k_work_delayable *dwork);
#endif

#if VA_TRACE_SLEEP
void sys_trace_va_thread_suspend(struct k_thread *thread);
void sys_trace_va_thread_resume(struct k_thread *thread);
void sys_trace_k_thread_sleep_enter(k_timeout_t timeout);
void sys_trace_k_thread_sleep_exit(k_timeout_t timeout, int ret);
void sys_trace_k_thread_msleep_enter(int32_t ms);
void sys_trace_k_thread_msleep_exit(int32_t ms, int ret);
void sys_trace_k_thread_usleep_enter(int32_t us);
void sys_trace_k_thread_usleep_exit(int32_t us, int ret);
#endif

#if VA_TRACE_TIMERS
void sys_trace_k_timer_init(struct k_timer *timer);
void sys_trace_k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period);
void sys_trace_k_timer_stop(struct k_timer *timer);
void sys_trace_k_timer_status_sync_blocking(struct k_timer *timer, k_timeout_t timeout);
#endif

#if VA_TRACE_RTOS_HEAPS
void sys_trace_k_heap_init(struct k_heap *heap);
void sys_trace_k_heap_alloc_exit_impl(struct k_heap *heap, uint32_t alloc_bytes, void *ret);
void sys_trace_k_heap_free(struct k_heap *heap);
void sys_trace_k_heap_alloc_blocking(struct k_heap *heap);
#endif

#if VA_TRACE_PM
void sys_trace_va_pm_system_suspend_enter(uint32_t ticks);
void sys_trace_va_pm_system_suspend_exit(uint32_t ticks, uint8_t state);
#endif

/* ── Overridden trace points ─────────────────────────────────────── */
/* #undef the stock no-op, re-point at the adapter. Disabled categories keep
   the stock no-op. */

#if VA_NEEDS_TASK_REGISTRY
/* The kernel signals abort through the enter/exit macro pair, not through
   sys_trace_thread_abort (self-aborting threads never reach exit, so the
   enter side is the reliable one). */
#undef  sys_port_trace_k_thread_abort_enter
#define sys_port_trace_k_thread_abort_enter(thread) sys_trace_va_thread_abort(thread)
#endif

#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION
#undef  sys_port_trace_k_mutex_init
#define sys_port_trace_k_mutex_init(mutex, ret) sys_trace_k_mutex_init(mutex, ret)
#undef  sys_port_trace_k_mutex_lock_enter
#define sys_port_trace_k_mutex_lock_enter(mutex, timeout) sys_trace_k_mutex_lock_enter(mutex, timeout)
#undef  sys_port_trace_k_mutex_lock_blocking
#define sys_port_trace_k_mutex_lock_blocking(mutex, timeout) sys_trace_k_mutex_lock_blocking(mutex, timeout)
#undef  sys_port_trace_k_mutex_lock_exit
#define sys_port_trace_k_mutex_lock_exit(mutex, timeout, ret) sys_trace_k_mutex_lock_exit(mutex, timeout, ret)
#undef  sys_port_trace_k_mutex_unlock_enter
#define sys_port_trace_k_mutex_unlock_enter(mutex) sys_trace_k_mutex_unlock_enter(mutex)
#undef  sys_port_trace_k_mutex_unlock_exit
#define sys_port_trace_k_mutex_unlock_exit(mutex, ret) sys_trace_k_mutex_unlock_exit(mutex, ret)
#endif

#if VA_TRACE_SEMAPHORES
#undef  sys_port_trace_k_sem_init
#define sys_port_trace_k_sem_init(sem, ret) sys_trace_k_sem_init(sem, ret)
#undef  sys_port_trace_k_sem_give_enter
#define sys_port_trace_k_sem_give_enter(sem) sys_trace_k_sem_give_enter(sem)
#undef  sys_port_trace_k_sem_take_enter
#define sys_port_trace_k_sem_take_enter(sem, timeout) sys_trace_k_sem_take_enter(sem, timeout)
#undef  sys_port_trace_k_sem_take_blocking
#define sys_port_trace_k_sem_take_blocking(sem, timeout) sys_trace_k_sem_take_blocking(sem, timeout)
#undef  sys_port_trace_k_sem_take_exit
#define sys_port_trace_k_sem_take_exit(sem, timeout, ret) sys_trace_k_sem_take_exit(sem, timeout, ret)
#endif

#if VA_TRACE_QUEUES
#undef  sys_port_trace_k_msgq_init
#define sys_port_trace_k_msgq_init(msgq) sys_trace_k_msgq_init(msgq)
#undef  sys_port_trace_k_msgq_put_enter
#define sys_port_trace_k_msgq_put_enter(msgq, timeout) sys_trace_k_msgq_put_enter(msgq, timeout)
#undef  sys_port_trace_k_msgq_put_blocking
#define sys_port_trace_k_msgq_put_blocking(msgq, timeout) sys_trace_k_msgq_put_blocking(msgq, timeout)
#undef  sys_port_trace_k_msgq_put_exit
#define sys_port_trace_k_msgq_put_exit(msgq, timeout, ret) sys_trace_k_msgq_put_exit(msgq, timeout, ret)
#undef  sys_port_trace_k_msgq_get_enter
#define sys_port_trace_k_msgq_get_enter(msgq, timeout) sys_trace_k_msgq_get_enter(msgq, timeout)
#undef  sys_port_trace_k_msgq_get_blocking
#define sys_port_trace_k_msgq_get_blocking(msgq, timeout) sys_trace_k_msgq_get_blocking(msgq, timeout)
#undef  sys_port_trace_k_msgq_get_exit
#define sys_port_trace_k_msgq_get_exit(msgq, timeout, ret) sys_trace_k_msgq_get_exit(msgq, timeout, ret)

/* k_fifo / k_lifo ride the queue events: put = give, successful get = take.
   They have no fixed capacity; pairing order is FIFO or LIFO per the name.
   The underlying k_queue trace points are NOT hooked, so nothing doubles. */
#undef  sys_port_trace_k_fifo_init_exit
#define sys_port_trace_k_fifo_init_exit(fifo) sys_trace_k_fifo_init(fifo)
#undef  sys_port_trace_k_fifo_put_exit
#define sys_port_trace_k_fifo_put_exit(fifo, data) sys_trace_k_fifo_put(fifo)
#undef  sys_port_trace_k_fifo_alloc_put_exit
#define sys_port_trace_k_fifo_alloc_put_exit(fifo, data, ret) sys_trace_k_fifo_alloc_put(fifo, ret)
#undef  sys_port_trace_k_fifo_put_list_exit
#define sys_port_trace_k_fifo_put_list_exit(fifo, head, tail) sys_trace_k_fifo_put(fifo)
#undef  sys_port_trace_k_fifo_put_slist_exit
#define sys_port_trace_k_fifo_put_slist_exit(fifo, list) sys_trace_k_fifo_put(fifo)
#undef  sys_port_trace_k_fifo_get_exit
#define sys_port_trace_k_fifo_get_exit(fifo, timeout, ret) sys_trace_k_fifo_get(fifo, ret)

#undef  sys_port_trace_k_lifo_init_exit
#define sys_port_trace_k_lifo_init_exit(lifo) sys_trace_k_lifo_init(lifo)
#undef  sys_port_trace_k_lifo_put_exit
#define sys_port_trace_k_lifo_put_exit(lifo, data) sys_trace_k_lifo_put(lifo)
#undef  sys_port_trace_k_lifo_alloc_put_exit
#define sys_port_trace_k_lifo_alloc_put_exit(lifo, data, ret) sys_trace_k_lifo_alloc_put(lifo, ret)
#undef  sys_port_trace_k_lifo_get_exit
#define sys_port_trace_k_lifo_get_exit(lifo, timeout, ret) sys_trace_k_lifo_get(lifo, ret)
#endif

#if VA_TRACE_EVENT_FLAGS
#undef  sys_port_trace_k_event_init
#define sys_port_trace_k_event_init(event) sys_trace_k_event_init(event)
/* The post ENTER point carries the raw posted bits (the exit's `events`
   local has already been merged with the previous state). */
#undef  sys_port_trace_k_event_post_enter
#define sys_port_trace_k_event_post_enter(event, events, events_mask) sys_trace_k_event_post(event, events)
#undef  sys_port_trace_k_event_wait_exit
#define sys_port_trace_k_event_wait_exit(event, events, ret) sys_trace_k_event_wait_exit(event, events, ret)
#endif

#if VA_TRACE_WORK
/* Only the *_to_queue / *_for_queue exits are hooked: the plain
   k_work_submit/schedule/reschedule wrappers call them internally and
   trace BOTH pairs, so hooking both would double-report. All four cancel
   entry points are independent. */
#undef  sys_port_trace_k_work_submit_to_queue_exit
#define sys_port_trace_k_work_submit_to_queue_exit(queue, work, ret) sys_trace_k_work_submit(work, ret)
#undef  sys_port_trace_k_work_schedule_for_queue_exit
#define sys_port_trace_k_work_schedule_for_queue_exit(queue, dwork, delay, ret) sys_trace_k_work_schedule(dwork, delay, ret)
#undef  sys_port_trace_k_work_reschedule_for_queue_exit
#define sys_port_trace_k_work_reschedule_for_queue_exit(queue, dwork, delay, ret) sys_trace_k_work_schedule(dwork, delay, ret)
#undef  sys_port_trace_k_work_cancel_exit
#define sys_port_trace_k_work_cancel_exit(work, ret) sys_trace_k_work_cancel(work)
#undef  sys_port_trace_k_work_cancel_sync_exit
#define sys_port_trace_k_work_cancel_sync_exit(work, sync, ret) sys_trace_k_work_cancel(work)
#undef  sys_port_trace_k_work_cancel_delayable_exit
#define sys_port_trace_k_work_cancel_delayable_exit(dwork, ret) sys_trace_k_work_cancel_delayable(dwork)
#undef  sys_port_trace_k_work_cancel_delayable_sync_exit
#define sys_port_trace_k_work_cancel_delayable_sync_exit(dwork, sync, ret) sys_trace_k_work_cancel_delayable(dwork)
#endif

#if VA_TRACE_SLEEP
/* Suspend/resume ride the sleep events: the suspend-to-resume window shows
   as a sleep period on the suspended thread, same as the FreeRTOS adapter. */
#undef  sys_port_trace_k_thread_suspend_enter
#define sys_port_trace_k_thread_suspend_enter(thread) sys_trace_va_thread_suspend(thread)
#undef  sys_port_trace_k_thread_resume_enter
#define sys_port_trace_k_thread_resume_enter(thread) sys_trace_va_thread_resume(thread)
#undef  sys_port_trace_k_thread_sleep_enter
#define sys_port_trace_k_thread_sleep_enter(timeout) sys_trace_k_thread_sleep_enter(timeout)
#undef  sys_port_trace_k_thread_sleep_exit
#define sys_port_trace_k_thread_sleep_exit(timeout, ret) sys_trace_k_thread_sleep_exit(timeout, ret)
#undef  sys_port_trace_k_thread_msleep_enter
#define sys_port_trace_k_thread_msleep_enter(ms) sys_trace_k_thread_msleep_enter(ms)
#undef  sys_port_trace_k_thread_msleep_exit
#define sys_port_trace_k_thread_msleep_exit(ms, ret) sys_trace_k_thread_msleep_exit(ms, ret)
#undef  sys_port_trace_k_thread_usleep_enter
#define sys_port_trace_k_thread_usleep_enter(us) sys_trace_k_thread_usleep_enter(us)
#undef  sys_port_trace_k_thread_usleep_exit
#define sys_port_trace_k_thread_usleep_exit(us, ret) sys_trace_k_thread_usleep_exit(us, ret)
#endif

#if VA_TRACE_TIMERS
#undef  sys_port_trace_k_timer_init
#define sys_port_trace_k_timer_init(timer) sys_trace_k_timer_init(timer)
#undef  sys_port_trace_k_timer_start
#define sys_port_trace_k_timer_start(timer, duration, period) sys_trace_k_timer_start(timer, duration, period)
#undef  sys_port_trace_k_timer_stop
#define sys_port_trace_k_timer_stop(timer) sys_trace_k_timer_stop(timer)
#undef  sys_port_trace_k_timer_status_sync_blocking
#define sys_port_trace_k_timer_status_sync_blocking(timer, timeout) sys_trace_k_timer_status_sync_blocking(timer, timeout)
#endif

#if VA_TRACE_RTOS_HEAPS
/* `bytes` is in scope at the kernel trace point. */
#undef  sys_port_trace_k_heap_init
#define sys_port_trace_k_heap_init(heap) sys_trace_k_heap_init(heap)

/* The blocking point was renamed in Zephyr 4.2 (aligned_alloc_blocking ->
   alloc_helper_blocking); hook both names so every version emits it. */
#undef  sys_port_trace_k_heap_aligned_alloc_blocking
#define sys_port_trace_k_heap_aligned_alloc_blocking(heap, timeout) sys_trace_k_heap_alloc_blocking(heap)
#undef  sys_port_trace_k_heap_alloc_helper_blocking
#define sys_port_trace_k_heap_alloc_helper_blocking(heap, timeout) sys_trace_k_heap_alloc_blocking(heap)

#undef  sys_port_trace_k_heap_aligned_alloc_exit
#define sys_port_trace_k_heap_aligned_alloc_exit(heap, timeout, ret) sys_trace_k_heap_alloc_exit_impl(heap, (uint32_t)(bytes), ret)
/* Zephyr 4.2 refactored k_heap_alloc and k_heap_aligned_alloc onto a shared
   helper, so their exit points are independent and both must be hooked. On
   older kernels k_heap_alloc calls k_heap_aligned_alloc internally - there
   the inner aligned exit already covers both, and hooking the outer alloc
   exit too would double-report. k_heap_calloc is NOT hooked on any version:
   it allocates through the hooked paths and would double-report.
   The gate is 4.1.99, not 4.2.0: post-4.1 development snapshots carry the
   refactor while reporting 4.1.99, and every release resolves correctly
   either way. */
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 1, 99)
#undef  sys_port_trace_k_heap_alloc_exit
#define sys_port_trace_k_heap_alloc_exit(heap, timeout, ret) sys_trace_k_heap_alloc_exit_impl(heap, (uint32_t)(bytes), ret)
#endif
#undef  sys_port_trace_k_heap_free
#define sys_port_trace_k_heap_free(heap) sys_trace_k_heap_free(heap)
#undef  sys_port_trace_k_heap_realloc_exit
#define sys_port_trace_k_heap_realloc_exit(h, ptr, bytes, timeout, ret) sys_trace_k_heap_alloc_exit_impl(h, (uint32_t)(bytes), ret)
#endif

#if VA_TRACE_PM
#undef  sys_port_trace_pm_system_suspend_enter
#define sys_port_trace_pm_system_suspend_enter(ticks) sys_trace_va_pm_system_suspend_enter(ticks)
#undef  sys_port_trace_pm_system_suspend_exit
#define sys_port_trace_pm_system_suspend_exit(ticks, state) sys_trace_va_pm_system_suspend_exit(ticks, state)
#endif

#ifdef __cplusplus
}
#endif

#endif /* VIEWALYZER_TRACING_USER_WRAP_H */
