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
#include "ViewAlyzer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Adapter dispatch declarations (defined in VA_Adapter_Zephyr.c) ─── */
/* These are recorder-owned names. Keep them outside Zephyr's sys_trace_*
   namespace, which upstream tracing backends also define. Only the required
   weak user callbacks in the adapter retain Zephyr's callback names. */

#if VA_NEEDS_TASK_REGISTRY
void viewalyzer_zephyr_thread_abort(struct k_thread *thread);
#endif

#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION
void viewalyzer_zephyr_mutex_init(struct k_mutex *mutex, int ret);
void viewalyzer_zephyr_mutex_lock_enter(struct k_mutex *mutex, k_timeout_t timeout);
void viewalyzer_zephyr_mutex_lock_blocking(struct k_mutex *mutex, k_timeout_t timeout);
void viewalyzer_zephyr_mutex_lock_exit(struct k_mutex *mutex, k_timeout_t timeout, int ret);
void viewalyzer_zephyr_mutex_unlock_enter(struct k_mutex *mutex);
void viewalyzer_zephyr_mutex_unlock_exit(struct k_mutex *mutex, int ret);
#endif

#if VA_TRACE_SEMAPHORES
void viewalyzer_zephyr_sem_init(struct k_sem *sem, int ret);
void viewalyzer_zephyr_sem_give_enter(struct k_sem *sem);
void viewalyzer_zephyr_sem_take_enter(struct k_sem *sem, k_timeout_t timeout);
void viewalyzer_zephyr_sem_take_blocking(struct k_sem *sem, k_timeout_t timeout);
void viewalyzer_zephyr_sem_take_exit(struct k_sem *sem, k_timeout_t timeout, int ret);
#endif

#if VA_TRACE_QUEUES
void viewalyzer_zephyr_msgq_init(struct k_msgq *msgq);
void viewalyzer_zephyr_msgq_put_enter(struct k_msgq *msgq, k_timeout_t timeout);
void viewalyzer_zephyr_msgq_put_blocking(struct k_msgq *msgq, k_timeout_t timeout);
void viewalyzer_zephyr_msgq_put_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret);
void viewalyzer_zephyr_msgq_get_enter(struct k_msgq *msgq, k_timeout_t timeout);
void viewalyzer_zephyr_msgq_get_blocking(struct k_msgq *msgq, k_timeout_t timeout);
void viewalyzer_zephyr_msgq_get_exit(struct k_msgq *msgq, k_timeout_t timeout, int ret);
void viewalyzer_zephyr_fifo_init(struct k_fifo *fifo);
void viewalyzer_zephyr_fifo_put(struct k_fifo *fifo);
void viewalyzer_zephyr_fifo_alloc_put(struct k_fifo *fifo, int ret);
void viewalyzer_zephyr_fifo_get(struct k_fifo *fifo, void *ret);
void viewalyzer_zephyr_lifo_init(struct k_lifo *lifo);
void viewalyzer_zephyr_lifo_put(struct k_lifo *lifo);
void viewalyzer_zephyr_lifo_alloc_put(struct k_lifo *lifo, int ret);
void viewalyzer_zephyr_lifo_get(struct k_lifo *lifo, void *ret);
#endif

#if VA_TRACE_EVENT_FLAGS
void viewalyzer_zephyr_event_init(struct k_event *event);
void viewalyzer_zephyr_event_post(struct k_event *event, uint32_t events);
void viewalyzer_zephyr_event_wait_exit(struct k_event *event, uint32_t events, uint32_t ret);
#endif

#if VA_TRACE_WORK
void viewalyzer_zephyr_work_submit(struct k_work *work, int ret);
void viewalyzer_zephyr_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay, int ret);
void viewalyzer_zephyr_work_cancel(struct k_work *work);
void viewalyzer_zephyr_work_cancel_delayable(struct k_work_delayable *dwork);
#endif

#if VA_TRACE_SLEEP || VA_TRACE_TASK_STATES
void viewalyzer_zephyr_thread_suspend(struct k_thread *thread);
void viewalyzer_zephyr_thread_resume(struct k_thread *thread);
void viewalyzer_zephyr_thread_sleep_enter(k_timeout_t timeout);
void viewalyzer_zephyr_thread_sleep_exit(k_timeout_t timeout, int ret);
void viewalyzer_zephyr_thread_msleep_enter(int32_t ms);
void viewalyzer_zephyr_thread_msleep_exit(int32_t ms, int ret);
void viewalyzer_zephyr_thread_usleep_enter(int32_t us);
void viewalyzer_zephyr_thread_usleep_exit(int32_t us, int ret);
#endif

#if VA_TRACE_TIMERS
void viewalyzer_zephyr_timer_init(struct k_timer *timer);
void viewalyzer_zephyr_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period);
void viewalyzer_zephyr_timer_stop(struct k_timer *timer);
void viewalyzer_zephyr_timer_status_sync_blocking(struct k_timer *timer, k_timeout_t timeout);
#endif

#if VA_TRACE_RTOS_HEAPS
void viewalyzer_zephyr_heap_init(struct k_heap *heap);
void viewalyzer_zephyr_heap_alloc_exit_impl(struct k_heap *heap, uint32_t alloc_bytes, void *ret);
void viewalyzer_zephyr_heap_free(struct k_heap *heap);
void viewalyzer_zephyr_heap_alloc_blocking(struct k_heap *heap);
#endif

#if VA_TRACE_PM
void viewalyzer_zephyr_pm_system_suspend_enter(uint32_t ticks);
void viewalyzer_zephyr_pm_system_suspend_exit(uint32_t ticks, uint8_t state);
#endif

/* ── Overridden trace points ─────────────────────────────────────── */
/* #undef the stock no-op, re-point at the adapter. Disabled categories keep
   the stock no-op. */

#if VA_NEEDS_TASK_REGISTRY
/* The kernel signals abort through the enter/exit macro pair, not through
   sys_trace_thread_abort (self-aborting threads never reach exit, so the
   enter side is the reliable one). */
#undef  sys_port_trace_k_thread_abort_enter
#define sys_port_trace_k_thread_abort_enter(thread) viewalyzer_zephyr_thread_abort(thread)
#endif

#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION
#undef  sys_port_trace_k_mutex_init
#define sys_port_trace_k_mutex_init(mutex, ret) viewalyzer_zephyr_mutex_init(mutex, ret)
#undef  sys_port_trace_k_mutex_lock_enter
#define sys_port_trace_k_mutex_lock_enter(mutex, timeout) viewalyzer_zephyr_mutex_lock_enter(mutex, timeout)
#undef  sys_port_trace_k_mutex_lock_blocking
#define sys_port_trace_k_mutex_lock_blocking(mutex, timeout) viewalyzer_zephyr_mutex_lock_blocking(mutex, timeout)
#undef  sys_port_trace_k_mutex_lock_exit
#define sys_port_trace_k_mutex_lock_exit(mutex, timeout, ret) viewalyzer_zephyr_mutex_lock_exit(mutex, timeout, ret)
#undef  sys_port_trace_k_mutex_unlock_enter
#define sys_port_trace_k_mutex_unlock_enter(mutex) viewalyzer_zephyr_mutex_unlock_enter(mutex)
#undef  sys_port_trace_k_mutex_unlock_exit
#define sys_port_trace_k_mutex_unlock_exit(mutex, ret) viewalyzer_zephyr_mutex_unlock_exit(mutex, ret)
#endif

#if VA_TRACE_SEMAPHORES
#undef  sys_port_trace_k_sem_init
#define sys_port_trace_k_sem_init(sem, ret) viewalyzer_zephyr_sem_init(sem, ret)
#undef  sys_port_trace_k_sem_give_enter
#define sys_port_trace_k_sem_give_enter(sem) viewalyzer_zephyr_sem_give_enter(sem)
#undef  sys_port_trace_k_sem_take_enter
#define sys_port_trace_k_sem_take_enter(sem, timeout) viewalyzer_zephyr_sem_take_enter(sem, timeout)
#undef  sys_port_trace_k_sem_take_blocking
#define sys_port_trace_k_sem_take_blocking(sem, timeout) viewalyzer_zephyr_sem_take_blocking(sem, timeout)
#undef  sys_port_trace_k_sem_take_exit
#define sys_port_trace_k_sem_take_exit(sem, timeout, ret) viewalyzer_zephyr_sem_take_exit(sem, timeout, ret)
#endif

#if VA_TRACE_QUEUES
#undef  sys_port_trace_k_msgq_init
#define sys_port_trace_k_msgq_init(msgq) viewalyzer_zephyr_msgq_init(msgq)
#undef  sys_port_trace_k_msgq_put_enter
#define sys_port_trace_k_msgq_put_enter(msgq, timeout) viewalyzer_zephyr_msgq_put_enter(msgq, timeout)
#undef  sys_port_trace_k_msgq_put_blocking
#define sys_port_trace_k_msgq_put_blocking(msgq, timeout) viewalyzer_zephyr_msgq_put_blocking(msgq, timeout)
#undef  sys_port_trace_k_msgq_put_exit
#define sys_port_trace_k_msgq_put_exit(msgq, timeout, ret) viewalyzer_zephyr_msgq_put_exit(msgq, timeout, ret)
#undef  sys_port_trace_k_msgq_get_enter
#define sys_port_trace_k_msgq_get_enter(msgq, timeout) viewalyzer_zephyr_msgq_get_enter(msgq, timeout)
#undef  sys_port_trace_k_msgq_get_blocking
#define sys_port_trace_k_msgq_get_blocking(msgq, timeout) viewalyzer_zephyr_msgq_get_blocking(msgq, timeout)
#undef  sys_port_trace_k_msgq_get_exit
#define sys_port_trace_k_msgq_get_exit(msgq, timeout, ret) viewalyzer_zephyr_msgq_get_exit(msgq, timeout, ret)

/* k_fifo / k_lifo ride the queue events: put = give, successful get = take.
   They have no fixed capacity; pairing order is FIFO or LIFO per the name.
   The underlying k_queue trace points are NOT hooked, so nothing doubles. */
#undef  sys_port_trace_k_fifo_init_exit
#define sys_port_trace_k_fifo_init_exit(fifo) viewalyzer_zephyr_fifo_init(fifo)
#undef  sys_port_trace_k_fifo_put_exit
#define sys_port_trace_k_fifo_put_exit(fifo, data) viewalyzer_zephyr_fifo_put(fifo)
#undef  sys_port_trace_k_fifo_alloc_put_exit
#define sys_port_trace_k_fifo_alloc_put_exit(fifo, data, ret) viewalyzer_zephyr_fifo_alloc_put(fifo, ret)
#undef  sys_port_trace_k_fifo_put_list_exit
#define sys_port_trace_k_fifo_put_list_exit(fifo, head, tail) viewalyzer_zephyr_fifo_put(fifo)
#undef  sys_port_trace_k_fifo_put_slist_exit
#define sys_port_trace_k_fifo_put_slist_exit(fifo, list) viewalyzer_zephyr_fifo_put(fifo)
#undef  sys_port_trace_k_fifo_get_exit
#define sys_port_trace_k_fifo_get_exit(fifo, timeout, ret) viewalyzer_zephyr_fifo_get(fifo, ret)

#undef  sys_port_trace_k_lifo_init_exit
#define sys_port_trace_k_lifo_init_exit(lifo) viewalyzer_zephyr_lifo_init(lifo)
#undef  sys_port_trace_k_lifo_put_exit
#define sys_port_trace_k_lifo_put_exit(lifo, data) viewalyzer_zephyr_lifo_put(lifo)
#undef  sys_port_trace_k_lifo_alloc_put_exit
#define sys_port_trace_k_lifo_alloc_put_exit(lifo, data, ret) viewalyzer_zephyr_lifo_alloc_put(lifo, ret)
#undef  sys_port_trace_k_lifo_get_exit
#define sys_port_trace_k_lifo_get_exit(lifo, timeout, ret) viewalyzer_zephyr_lifo_get(lifo, ret)
#endif

#if VA_TRACE_EVENT_FLAGS
#undef  sys_port_trace_k_event_init
#define sys_port_trace_k_event_init(event) viewalyzer_zephyr_event_init(event)
/* The post ENTER point carries the raw posted bits (the exit's `events`
   local has already been merged with the previous state). */
#undef  sys_port_trace_k_event_post_enter
#define sys_port_trace_k_event_post_enter(event, events, events_mask) viewalyzer_zephyr_event_post(event, events)
#undef  sys_port_trace_k_event_wait_exit
#define sys_port_trace_k_event_wait_exit(event, events, ret) viewalyzer_zephyr_event_wait_exit(event, events, ret)
#endif

#if VA_TRACE_WORK
/* Only the *_to_queue / *_for_queue exits are hooked: the plain
   k_work_submit/schedule/reschedule wrappers call them internally and
   trace BOTH pairs, so hooking both would double-report. All four cancel
   entry points are independent. */
#undef  sys_port_trace_k_work_submit_to_queue_exit
#define sys_port_trace_k_work_submit_to_queue_exit(queue, work, ret) viewalyzer_zephyr_work_submit(work, ret)
#undef  sys_port_trace_k_work_schedule_for_queue_exit
#define sys_port_trace_k_work_schedule_for_queue_exit(queue, dwork, delay, ret) viewalyzer_zephyr_work_schedule(dwork, delay, ret)
#undef  sys_port_trace_k_work_reschedule_for_queue_exit
#define sys_port_trace_k_work_reschedule_for_queue_exit(queue, dwork, delay, ret) viewalyzer_zephyr_work_schedule(dwork, delay, ret)
#undef  sys_port_trace_k_work_cancel_exit
#define sys_port_trace_k_work_cancel_exit(work, ret) viewalyzer_zephyr_work_cancel(work)
#undef  sys_port_trace_k_work_cancel_sync_exit
#define sys_port_trace_k_work_cancel_sync_exit(work, sync, ret) viewalyzer_zephyr_work_cancel(work)
#undef  sys_port_trace_k_work_cancel_delayable_exit
#define sys_port_trace_k_work_cancel_delayable_exit(dwork, ret) viewalyzer_zephyr_work_cancel_delayable(dwork)
#undef  sys_port_trace_k_work_cancel_delayable_sync_exit
#define sys_port_trace_k_work_cancel_delayable_sync_exit(dwork, sync, ret) viewalyzer_zephyr_work_cancel_delayable(dwork)
#endif

#if VA_TRACE_SLEEP || VA_TRACE_TASK_STATES
/* Suspend/resume ride the sleep events: the suspend-to-resume window shows
   as a sleep period on the suspended thread, same as the FreeRTOS adapter. */
#undef  sys_port_trace_k_thread_suspend_enter
#define sys_port_trace_k_thread_suspend_enter(thread) viewalyzer_zephyr_thread_suspend(thread)
#undef  sys_port_trace_k_thread_resume_enter
#define sys_port_trace_k_thread_resume_enter(thread) viewalyzer_zephyr_thread_resume(thread)

/* Newer kernels funnel all three public sleep APIs through sleep_ticks.
   Select the hook interface exposed by the stock header, rather than a
   development version number that can span both implementations. */
#if defined(sys_port_trace_k_thread_sleep_ticks_enter)
#undef  sys_port_trace_k_thread_sleep_ticks_enter
#define sys_port_trace_k_thread_sleep_ticks_enter(timeout) viewalyzer_zephyr_thread_sleep_enter(timeout)
#undef  sys_port_trace_k_thread_sleep_ticks_exit
#define sys_port_trace_k_thread_sleep_ticks_exit(timeout, ret) viewalyzer_zephyr_thread_sleep_exit(timeout, ret)
#else
#undef  sys_port_trace_k_thread_sleep_enter
#define sys_port_trace_k_thread_sleep_enter(timeout) viewalyzer_zephyr_thread_sleep_enter(timeout)
#undef  sys_port_trace_k_thread_sleep_exit
#define sys_port_trace_k_thread_sleep_exit(timeout, ret) viewalyzer_zephyr_thread_sleep_exit(timeout, ret)
#undef  sys_port_trace_k_thread_msleep_enter
#define sys_port_trace_k_thread_msleep_enter(ms) viewalyzer_zephyr_thread_msleep_enter(ms)
#undef  sys_port_trace_k_thread_msleep_exit
#define sys_port_trace_k_thread_msleep_exit(ms, ret) viewalyzer_zephyr_thread_msleep_exit(ms, ret)
#undef  sys_port_trace_k_thread_usleep_enter
#define sys_port_trace_k_thread_usleep_enter(us) viewalyzer_zephyr_thread_usleep_enter(us)
#undef  sys_port_trace_k_thread_usleep_exit
#define sys_port_trace_k_thread_usleep_exit(us, ret) viewalyzer_zephyr_thread_usleep_exit(us, ret)
#endif /* sleep hook interface */
#endif

#if VA_TRACE_TIMERS
#undef  sys_port_trace_k_timer_init
#define sys_port_trace_k_timer_init(timer) viewalyzer_zephyr_timer_init(timer)
#undef  sys_port_trace_k_timer_start
#define sys_port_trace_k_timer_start(timer, duration, period) viewalyzer_zephyr_timer_start(timer, duration, period)
#undef  sys_port_trace_k_timer_stop
#define sys_port_trace_k_timer_stop(timer) viewalyzer_zephyr_timer_stop(timer)
#undef  sys_port_trace_k_timer_status_sync_blocking
#define sys_port_trace_k_timer_status_sync_blocking(timer, timeout) viewalyzer_zephyr_timer_status_sync_blocking(timer, timeout)
#endif

#if VA_TRACE_RTOS_HEAPS
/* `bytes` is in scope at the kernel trace point. */
#undef  sys_port_trace_k_heap_init
#define sys_port_trace_k_heap_init(heap) viewalyzer_zephyr_heap_init(heap)

/* The blocking point was renamed in Zephyr 4.2 (aligned_alloc_blocking ->
   alloc_helper_blocking); hook both names so every version emits it. */
#undef  sys_port_trace_k_heap_aligned_alloc_blocking
#define sys_port_trace_k_heap_aligned_alloc_blocking(heap, timeout) viewalyzer_zephyr_heap_alloc_blocking(heap)
#undef  sys_port_trace_k_heap_alloc_helper_blocking
#define sys_port_trace_k_heap_alloc_helper_blocking(heap, timeout) viewalyzer_zephyr_heap_alloc_blocking(heap)

#undef  sys_port_trace_k_heap_aligned_alloc_exit
#define sys_port_trace_k_heap_aligned_alloc_exit(heap, timeout, ret) viewalyzer_zephyr_heap_alloc_exit_impl(heap, (uint32_t)(bytes), ret)
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
#define sys_port_trace_k_heap_alloc_exit(heap, timeout, ret) viewalyzer_zephyr_heap_alloc_exit_impl(heap, (uint32_t)(bytes), ret)
#endif
#undef  sys_port_trace_k_heap_free
#define sys_port_trace_k_heap_free(heap) viewalyzer_zephyr_heap_free(heap)
#undef  sys_port_trace_k_heap_realloc_exit
#define sys_port_trace_k_heap_realloc_exit(h, ptr, bytes, timeout, ret) viewalyzer_zephyr_heap_alloc_exit_impl(h, (uint32_t)(bytes), ret)
#endif

#if VA_TRACE_PM
#undef  sys_port_trace_pm_system_suspend_enter
#define sys_port_trace_pm_system_suspend_enter(ticks) viewalyzer_zephyr_pm_system_suspend_enter(ticks)
#undef  sys_port_trace_pm_system_suspend_exit
#define sys_port_trace_pm_system_suspend_exit(ticks, state) viewalyzer_zephyr_pm_system_suspend_exit(ticks, state)
#endif

#ifdef __cplusplus
}
#endif


/* Only stock trace points are overridden here. No kernel patch is needed. */
#ifdef __cplusplus
extern "C" {
#endif
#if VA_TRACE_TASK_STATES
void viewalyzer_zephyr_sched_state(struct k_thread *thread, VA_TaskState_t state);
void viewalyzer_zephyr_priority(struct k_thread *thread, int prio);
void viewalyzer_zephyr_wait(void *object, VA_QueueObjectType_t type, VA_WaitReason_t reason, uint32_t detail);
void viewalyzer_zephyr_wait_end(void);
#undef sys_port_trace_k_thread_sched_ready
#define sys_port_trace_k_thread_sched_ready(thread) viewalyzer_zephyr_sched_state(thread, VA_TASK_READY)
#undef sys_port_trace_k_thread_sched_pend
#define sys_port_trace_k_thread_sched_pend(thread) viewalyzer_zephyr_sched_state(thread, VA_TASK_BLOCKED)
#undef sys_port_trace_k_thread_sched_priority_set
#define sys_port_trace_k_thread_sched_priority_set(thread, prio) viewalyzer_zephyr_priority(thread, prio)
#undef sys_port_trace_k_thread_join_blocking
#define sys_port_trace_k_thread_join_blocking(thread, timeout) viewalyzer_zephyr_wait(NULL, VA_OBJECT_TYPE_QUEUE, VA_WAIT_JOIN, (uint32_t)(uintptr_t)(thread))
#undef sys_port_trace_k_thread_join_exit
#define sys_port_trace_k_thread_join_exit(thread, timeout, ret) viewalyzer_zephyr_wait_end()
#endif
#if VA_TRACE_TIMERS && VA_TRACE_TIMER_CALLBACKS && defined(sys_port_trace_k_timer_expiry_enter)
#define VA_ZEPHYR_HAS_TIMER_CALLBACKS 1
void viewalyzer_zephyr_timer_callback(struct k_timer *timer, bool enter, bool stop);
#undef sys_port_trace_k_timer_expiry_enter
#define sys_port_trace_k_timer_expiry_enter(timer) viewalyzer_zephyr_timer_callback(timer, true, false)
#undef sys_port_trace_k_timer_expiry_exit
#define sys_port_trace_k_timer_expiry_exit(timer) viewalyzer_zephyr_timer_callback(timer, false, false)
#undef sys_port_trace_k_timer_stop_fn_expiry_enter
#define sys_port_trace_k_timer_stop_fn_expiry_enter(timer) viewalyzer_zephyr_timer_callback(timer, true, true)
#undef sys_port_trace_k_timer_stop_fn_expiry_exit
#define sys_port_trace_k_timer_stop_fn_expiry_exit(timer) viewalyzer_zephyr_timer_callback(timer, false, true)
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
void viewalyzer_zephyr_slab(struct k_mem_slab *slab, uint8_t operation, int ret);
#undef sys_port_trace_k_mem_slab_init
#define sys_port_trace_k_mem_slab_init(slab, rc) do { if ((rc) == 0) viewalyzer_zephyr_slab(slab, 0, 0); } while (0)
#undef sys_port_trace_k_mem_slab_alloc_blocking
#define sys_port_trace_k_mem_slab_alloc_blocking(slab, timeout) viewalyzer_zephyr_slab(slab, VA_OP_WAIT_BEGIN, 0)
#undef sys_port_trace_k_mem_slab_alloc_exit
#define sys_port_trace_k_mem_slab_alloc_exit(slab, timeout, ret) viewalyzer_zephyr_slab(slab, VA_OP_ALLOC, ret)
#undef sys_port_trace_k_mem_slab_free_exit
#define sys_port_trace_k_mem_slab_free_exit(slab) viewalyzer_zephyr_slab(slab, VA_OP_FREE, 0)
#endif
#if VA_TRACE_CONDVARS || VA_TRACE_TASK_STATES
void viewalyzer_zephyr_condvar(struct k_condvar *condvar, uint8_t operation, int ret, struct k_mutex *mutex, k_timeout_t timeout);
#undef sys_port_trace_k_condvar_init
#define sys_port_trace_k_condvar_init(condvar, ret) do { if ((ret) == 0) viewalyzer_zephyr_condvar(condvar, 0, 0, NULL, K_NO_WAIT); } while (0)
#undef sys_port_trace_k_condvar_wait_enter
#undef sys_port_trace_k_condvar_wait_exit
/* 4.3 added timeout to these hook signatures. The older call sites have
   the same mutex/timeout function arguments in scope. */
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 3, 0)
#define sys_port_trace_k_condvar_wait_enter(condvar, timeout) viewalyzer_zephyr_condvar(condvar, VA_OP_WAIT_BEGIN, 0, mutex, timeout)
#define sys_port_trace_k_condvar_wait_exit(condvar, timeout, ret) viewalyzer_zephyr_condvar(condvar, VA_OP_WAIT_END, ret, mutex, timeout)
#else
#define sys_port_trace_k_condvar_wait_enter(condvar) viewalyzer_zephyr_condvar(condvar, VA_OP_WAIT_BEGIN, 0, mutex, timeout)
#define sys_port_trace_k_condvar_wait_exit(condvar, ret) viewalyzer_zephyr_condvar(condvar, VA_OP_WAIT_END, ret, mutex, timeout)
#endif
#undef sys_port_trace_k_condvar_signal_exit
#define sys_port_trace_k_condvar_signal_exit(condvar, ret) viewalyzer_zephyr_condvar(condvar, VA_OP_SIGNAL, ret, NULL, K_NO_WAIT)
#undef sys_port_trace_k_condvar_broadcast_exit
#define sys_port_trace_k_condvar_broadcast_exit(condvar, ret) viewalyzer_zephyr_condvar(condvar, VA_OP_BROADCAST, ret, NULL, K_NO_WAIT)
#endif
#if VA_TRACE_POLL || VA_TRACE_TASK_STATES
void viewalyzer_zephyr_poll(struct k_poll_event *events, int count, bool enter, int ret);
void viewalyzer_zephyr_poll_signal(struct k_poll_signal *sig, uint8_t operation, int ret);
#undef sys_port_trace_k_poll_api_poll_enter
/* num_events is a kernel function argument on all supported anchors. */
#define sys_port_trace_k_poll_api_poll_enter(events) viewalyzer_zephyr_poll(events, num_events, true, 0)
#undef sys_port_trace_k_poll_api_poll_exit
#define sys_port_trace_k_poll_api_poll_exit(events, ret) viewalyzer_zephyr_poll(events, num_events, false, ret)
#undef sys_port_trace_k_poll_api_signal_init
#define sys_port_trace_k_poll_api_signal_init(sig) viewalyzer_zephyr_poll_signal(sig, 0, 0)
#undef sys_port_trace_k_poll_api_signal_reset
#define sys_port_trace_k_poll_api_signal_reset(sig) viewalyzer_zephyr_poll_signal(sig, VA_OP_RESET, 0)
#undef sys_port_trace_k_poll_api_signal_raise
#define sys_port_trace_k_poll_api_signal_raise(sig, ret) viewalyzer_zephyr_poll_signal(sig, VA_OP_SIGNAL, ret)
#endif

#if VA_TRACE_TASK_STATES
#undef sys_port_trace_k_mutex_lock_blocking
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION
#define sys_port_trace_k_mutex_lock_blocking(mutex, timeout) do { viewalyzer_zephyr_wait(mutex, VA_OBJECT_TYPE_MUTEX, VA_WAIT_MUTEX, 0); viewalyzer_zephyr_mutex_lock_blocking(mutex, timeout); } while (0)
#else
#define sys_port_trace_k_mutex_lock_blocking(mutex, timeout) viewalyzer_zephyr_wait(mutex, VA_OBJECT_TYPE_MUTEX, VA_WAIT_MUTEX, 0)
#endif
#undef sys_port_trace_k_sem_take_blocking
#if VA_TRACE_SEMAPHORES
#define sys_port_trace_k_sem_take_blocking(sem, timeout) do { viewalyzer_zephyr_wait(sem, VA_OBJECT_TYPE_COUNTING_SEM, VA_WAIT_SEMAPHORE, 0); viewalyzer_zephyr_sem_take_blocking(sem, timeout); } while (0)
#else
#define sys_port_trace_k_sem_take_blocking(sem, timeout) viewalyzer_zephyr_wait(sem, VA_OBJECT_TYPE_COUNTING_SEM, VA_WAIT_SEMAPHORE, 0)
#endif
#undef sys_port_trace_k_msgq_put_blocking
#if VA_TRACE_QUEUES
#define sys_port_trace_k_msgq_put_blocking(msgq, timeout) do { viewalyzer_zephyr_wait(msgq, VA_OBJECT_TYPE_QUEUE, VA_WAIT_QUEUE_SEND, 0); viewalyzer_zephyr_msgq_put_blocking(msgq, timeout); } while (0)
#else
#define sys_port_trace_k_msgq_put_blocking(msgq, timeout) viewalyzer_zephyr_wait(msgq, VA_OBJECT_TYPE_QUEUE, VA_WAIT_QUEUE_SEND, 0)
#endif
#undef sys_port_trace_k_msgq_get_blocking
#if VA_TRACE_QUEUES
#define sys_port_trace_k_msgq_get_blocking(msgq, timeout) do { viewalyzer_zephyr_wait(msgq, VA_OBJECT_TYPE_QUEUE, VA_WAIT_QUEUE_RECEIVE, 0); viewalyzer_zephyr_msgq_get_blocking(msgq, timeout); } while (0)
#else
#define sys_port_trace_k_msgq_get_blocking(msgq, timeout) viewalyzer_zephyr_wait(msgq, VA_OBJECT_TYPE_QUEUE, VA_WAIT_QUEUE_RECEIVE, 0)
#endif
#undef sys_port_trace_k_event_wait_blocking
#define sys_port_trace_k_event_wait_blocking(event, events, options, timeout) viewalyzer_zephyr_wait(event, VA_OBJECT_TYPE_EVENTFLAG, VA_WAIT_EVENT_FLAGS, events)
#endif

/* Optional IPC detail layer. Base hooks retain their existing stream when
   these switches are off. No application wrappers or kernel patches needed. */
#if VA_HAS_QUEUE_DETAILS
void viewalyzer_zephyr_msgq_detail(struct k_msgq *msgq, uint8_t op, int ret);
void viewalyzer_zephyr_unbounded(void *queue, uint8_t op, uint32_t count);
void viewalyzer_zephyr_fifo_batch(struct k_fifo *fifo, void *head, void *tail);
#undef sys_port_trace_k_msgq_peek
#define sys_port_trace_k_msgq_peek(msgq, ret) viewalyzer_zephyr_msgq_detail(msgq, VA_QUEUE_PEEK, ret)
#undef sys_port_trace_k_msgq_purge
#define sys_port_trace_k_msgq_purge(msgq) viewalyzer_zephyr_msgq_detail(msgq, VA_QUEUE_RESET, 0)
#if defined(sys_port_trace_k_msgq_put_front_exit)
#undef sys_port_trace_k_msgq_put_front_exit
#define sys_port_trace_k_msgq_put_front_exit(msgq, timeout, ret) viewalyzer_zephyr_msgq_detail(msgq, VA_QUEUE_FRONT, ret)
#endif
#undef sys_port_trace_k_fifo_put_list_enter
#define sys_port_trace_k_fifo_put_list_enter(fifo, head, tail) viewalyzer_zephyr_fifo_batch(fifo, head, tail)
#undef sys_port_trace_k_fifo_put_slist_enter
#define sys_port_trace_k_fifo_put_slist_enter(fifo, list) viewalyzer_zephyr_fifo_batch(fifo, sys_slist_peek_head(list), sys_slist_peek_tail(list))
#undef sys_port_trace_k_fifo_put_list_exit
#define sys_port_trace_k_fifo_put_list_exit(fifo, head, tail) ((void)0)
#undef sys_port_trace_k_fifo_put_slist_exit
#define sys_port_trace_k_fifo_put_slist_exit(fifo, list) ((void)0)
#endif

#if VA_HAS_EVENT_FLAG_DETAILS
void viewalyzer_zephyr_flags(struct k_event *event, uint8_t op, uint32_t bits, uint32_t mask);
/* Stock options are stable across the supported kernels: bit 0 all, bit 1
   reset; 4.4 adds bit 2 clear-on-receive. Older kernels never set bit 2. */
#define VA_ZEPHYR_EVENT_OPTIONS(o) (((o) & 1 ? VA_FLAGS_ALL : 0) | \
    ((o) & 2 ? VA_FLAGS_RESET_ON_ENTRY : 0) | ((o) & 4 ? VA_FLAGS_CLEAR_ON_EXIT : 0))
#undef sys_port_trace_k_event_init
#define sys_port_trace_k_event_init(event) \
    do { viewalyzer_zephyr_event_init(event); viewalyzer_zephyr_flags(event, VA_FLAGS_SNAPSHOT, 0, 0); } while (0)
#undef sys_port_trace_k_event_post_enter
#define sys_port_trace_k_event_post_enter(event, bits, mask) \
    viewalyzer_zephyr_flags(event, (bits) == 0 ? VA_FLAGS_CLEAR : VA_FLAGS_SET, bits, mask)
#undef sys_port_trace_k_event_post_exit
#define sys_port_trace_k_event_post_exit(event, bits, mask) \
    viewalyzer_zephyr_flags(event, VA_FLAGS_SNAPSHOT, (event)->events, 0)
#undef sys_port_trace_k_event_wait_enter
#define sys_port_trace_k_event_wait_enter(event, bits, opts, timeout) \
    viewalyzer_zephyr_flags(event, VA_FLAGS_WAIT_BEGIN | VA_ZEPHYR_EVENT_OPTIONS(opts), (event)->events, bits)
#undef sys_port_trace_k_event_wait_exit
#define sys_port_trace_k_event_wait_exit(event, bits, ret) \
    do { viewalyzer_zephyr_flags(event, ((ret) != 0 ? VA_FLAGS_WAIT_OK : VA_FLAGS_WAIT_TIMEOUT) | \
             VA_ZEPHYR_EVENT_OPTIONS(options), ret, bits); \
         viewalyzer_zephyr_flags(event, VA_FLAGS_SNAPSHOT, (event)->events, 0); } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* VIEWALYZER_TRACING_USER_WRAP_H */
