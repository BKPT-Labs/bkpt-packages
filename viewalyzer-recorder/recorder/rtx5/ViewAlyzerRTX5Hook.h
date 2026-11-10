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
/* Force-include in every kernel C translation unit and the recorder.
 * Build the kernel from source; omit upstream Source/rtx_evr.c. */
#ifndef VIEWALYZER_RTX5_HOOK_H
#define VIEWALYZER_RTX5_HOOK_H
#include "VA_RTX5Config.h"
#if VA_ENABLED && (VA_RTOS_SELECT == VA_RTOS_RTX5)
#if !VA_ALLOWED_TO_DISABLE_INTERRUPTS
#error "ViewAlyzer RTX5 requires interrupt masking for native hook serialization"
#endif
#include "RTX_Config.h"
#ifdef EVR_RTX_DISABLE
#error "RTX5 ViewAlyzer hooks require EVR_RTX_DISABLE to be unset"
#endif
#ifdef RTE_CMSIS_View_EventRecorder
#error "Select one RTX5 hook provider: ViewAlyzer or Arm EventRecorder"
#endif
/* A source-built kernel's initialization entry point encodes the same
 * recorder configuration as the application/core. A hookless prebuilt
 * kernel cannot silently satisfy the recorder's validation dependency. */
#define osKernelInitialize VA_RTX5_SYMBOL(osKernelInitialize)
#undef OS_EVR_MEMORY
#define OS_EVR_MEMORY 1
#undef OS_EVR_KERNEL
#define OS_EVR_KERNEL 1
#undef OS_EVR_THREAD
#define OS_EVR_THREAD 1
#undef OS_EVR_WAIT
#define OS_EVR_WAIT 1
#undef OS_EVR_THFLAGS
#define OS_EVR_THFLAGS 1
#undef OS_EVR_EVFLAGS
#define OS_EVR_EVFLAGS 1
#undef OS_EVR_TIMER
#define OS_EVR_TIMER 1
#undef OS_EVR_MUTEX
#define OS_EVR_MUTEX 1
#undef OS_EVR_SEMAPHORE
#define OS_EVR_SEMAPHORE 1
#undef OS_EVR_MEMPOOL
#define OS_EVR_MEMPOOL 1
#undef OS_EVR_MSGQUEUE
#define OS_EVR_MSGQUEUE 1
#if VA_TRACE_RTOS_HEAPS
#ifdef EVR_RTX_MEMORY_INIT_DISABLE
#error "Required RTX5 hook disabled: MemoryInit"
#endif
#define EvrRtxMemoryInit VA_RTX5_SYMBOL(va_rtx5_MemoryInit)
#else
#define EVR_RTX_MEMORY_INIT_DISABLE
#endif
#if VA_TRACE_RTOS_HEAPS
#ifdef EVR_RTX_MEMORY_ALLOC_DISABLE
#error "Required RTX5 hook disabled: MemoryAlloc"
#endif
#define EvrRtxMemoryAlloc VA_RTX5_SYMBOL(va_rtx5_MemoryAlloc)
#else
#define EVR_RTX_MEMORY_ALLOC_DISABLE
#endif
#if VA_TRACE_RTOS_HEAPS
#ifdef EVR_RTX_MEMORY_FREE_DISABLE
#error "Required RTX5 hook disabled: MemoryFree"
#endif
#define EvrRtxMemoryFree VA_RTX5_SYMBOL(va_rtx5_MemoryFree)
#else
#define EVR_RTX_MEMORY_FREE_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_BLOCK_INIT_DISABLE
#define EVR_RTX_MEMORY_BLOCK_INIT_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_BLOCK_ALLOC_DISABLE
#define EVR_RTX_MEMORY_BLOCK_ALLOC_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_BLOCK_FREE_DISABLE
#define EVR_RTX_MEMORY_BLOCK_FREE_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_ERROR_DISABLE
#define EVR_RTX_KERNEL_ERROR_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_INITIALIZE_DISABLE
#define EVR_RTX_KERNEL_INITIALIZE_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_INITIALIZED_DISABLE
#define EVR_RTX_KERNEL_INITIALIZED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_GET_INFO_DISABLE
#define EVR_RTX_KERNEL_GET_INFO_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_INFO_RETRIEVED_DISABLE
#define EVR_RTX_KERNEL_INFO_RETRIEVED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_GET_STATE_DISABLE
#define EVR_RTX_KERNEL_GET_STATE_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_START_DISABLE
#define EVR_RTX_KERNEL_START_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_STARTED_DISABLE
#define EVR_RTX_KERNEL_STARTED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_LOCK_DISABLE
#define EVR_RTX_KERNEL_LOCK_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_LOCKED_DISABLE
#define EVR_RTX_KERNEL_LOCKED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_UNLOCK_DISABLE
#define EVR_RTX_KERNEL_UNLOCK_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_UNLOCKED_DISABLE
#define EVR_RTX_KERNEL_UNLOCKED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_RESTORE_LOCK_DISABLE
#define EVR_RTX_KERNEL_RESTORE_LOCK_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_LOCK_RESTORED_DISABLE
#define EVR_RTX_KERNEL_LOCK_RESTORED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_SUSPEND_DISABLE
#define EVR_RTX_KERNEL_SUSPEND_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_SUSPENDED_DISABLE
#define EVR_RTX_KERNEL_SUSPENDED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_RESUME_DISABLE
#define EVR_RTX_KERNEL_RESUME_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_RESUMED_DISABLE
#define EVR_RTX_KERNEL_RESUMED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_PROTECT_DISABLE
#define EVR_RTX_KERNEL_PROTECT_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_PROTECTED_DISABLE
#define EVR_RTX_KERNEL_PROTECTED_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_GET_TICK_COUNT_DISABLE
#define EVR_RTX_KERNEL_GET_TICK_COUNT_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_GET_TICK_FREQ_DISABLE
#define EVR_RTX_KERNEL_GET_TICK_FREQ_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_GET_SYS_TIMER_COUNT_DISABLE
#define EVR_RTX_KERNEL_GET_SYS_TIMER_COUNT_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_GET_SYS_TIMER_FREQ_DISABLE
#define EVR_RTX_KERNEL_GET_SYS_TIMER_FREQ_DISABLE
#endif
#if VA_TRACE_TIMERS
#ifdef EVR_RTX_KERNEL_ERROR_NOTIFY_DISABLE
#error "Required RTX5 hook disabled: KernelErrorNotify"
#endif
#define EvrRtxKernelErrorNotify VA_RTX5_SYMBOL(va_rtx5_KernelErrorNotify)
#else
#define EVR_RTX_KERNEL_ERROR_NOTIFY_DISABLE
#endif
#ifndef EVR_RTX_KERNEL_DESTROY_CLASS_DISABLE
#define EVR_RTX_KERNEL_DESTROY_CLASS_DISABLE
#endif
#ifndef EVR_RTX_THREAD_ERROR_DISABLE
#define EVR_RTX_THREAD_ERROR_DISABLE
#endif
#ifndef EVR_RTX_THREAD_NEW_DISABLE
#define EVR_RTX_THREAD_NEW_DISABLE
#endif
#if VA_NEEDS_TASK_REGISTRY
#ifdef EVR_RTX_THREAD_CREATED_DISABLE
#error "Required RTX5 hook disabled: ThreadCreated"
#endif
#define EvrRtxThreadCreated VA_RTX5_SYMBOL(va_rtx5_ThreadCreated)
#else
#define EVR_RTX_THREAD_CREATED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_NAME_DISABLE
#define EVR_RTX_THREAD_GET_NAME_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_CLASS_DISABLE
#define EVR_RTX_THREAD_GET_CLASS_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_ZONE_DISABLE
#define EVR_RTX_THREAD_GET_ZONE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_ID_DISABLE
#define EVR_RTX_THREAD_GET_ID_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_STATE_DISABLE
#define EVR_RTX_THREAD_GET_STATE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_STACK_SIZE_DISABLE
#define EVR_RTX_THREAD_GET_STACK_SIZE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_STACK_SPACE_DISABLE
#define EVR_RTX_THREAD_GET_STACK_SPACE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_SET_PRIORITY_DISABLE
#define EVR_RTX_THREAD_SET_PRIORITY_DISABLE
#endif
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_THREAD_PRIORITY_UPDATED_DISABLE
#error "Required RTX5 hook disabled: ThreadPriorityUpdated"
#endif
#define EvrRtxThreadPriorityUpdated VA_RTX5_SYMBOL(va_rtx5_ThreadPriorityUpdated)
#else
#define EVR_RTX_THREAD_PRIORITY_UPDATED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_PRIORITY_DISABLE
#define EVR_RTX_THREAD_GET_PRIORITY_DISABLE
#endif
#ifndef EVR_RTX_THREAD_YIELD_DISABLE
#define EVR_RTX_THREAD_YIELD_DISABLE
#endif
#ifndef EVR_RTX_THREAD_SUSPEND_DISABLE
#define EVR_RTX_THREAD_SUSPEND_DISABLE
#endif
#if VA_NEEDS_TASK_REGISTRY
#ifdef EVR_RTX_THREAD_SUSPENDED_DISABLE
#error "Required RTX5 hook disabled: ThreadSuspended"
#endif
#define EvrRtxThreadSuspended VA_RTX5_SYMBOL(va_rtx5_ThreadSuspended)
#else
#define EVR_RTX_THREAD_SUSPENDED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_RESUME_DISABLE
#define EVR_RTX_THREAD_RESUME_DISABLE
#endif
#if VA_NEEDS_TASK_REGISTRY
#ifdef EVR_RTX_THREAD_RESUMED_DISABLE
#error "Required RTX5 hook disabled: ThreadResumed"
#endif
#define EvrRtxThreadResumed VA_RTX5_SYMBOL(va_rtx5_ThreadResumed)
#else
#define EVR_RTX_THREAD_RESUMED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_DETACH_DISABLE
#define EVR_RTX_THREAD_DETACH_DISABLE
#endif
#ifndef EVR_RTX_THREAD_DETACHED_DISABLE
#define EVR_RTX_THREAD_DETACHED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_JOIN_DISABLE
#define EVR_RTX_THREAD_JOIN_DISABLE
#endif
#if VA_TRACE_TASK_STATES
#ifdef EVR_RTX_THREAD_JOIN_PENDING_DISABLE
#error "Required RTX5 hook disabled: ThreadJoinPending"
#endif
#define EvrRtxThreadJoinPending VA_RTX5_SYMBOL(va_rtx5_ThreadJoinPending)
#else
#define EVR_RTX_THREAD_JOIN_PENDING_DISABLE
#endif
#ifndef EVR_RTX_THREAD_JOINED_DISABLE
#define EVR_RTX_THREAD_JOINED_DISABLE
#endif
#if VA_TRACE_TASK_STATES
#ifdef EVR_RTX_THREAD_BLOCKED_DISABLE
#error "Required RTX5 hook disabled: ThreadBlocked"
#endif
#define EvrRtxThreadBlocked VA_RTX5_SYMBOL(va_rtx5_ThreadBlocked)
#else
#define EVR_RTX_THREAD_BLOCKED_DISABLE
#endif
#if VA_NEEDS_TASK_REGISTRY
#ifdef EVR_RTX_THREAD_UNBLOCKED_DISABLE
#error "Required RTX5 hook disabled: ThreadUnblocked"
#endif
#define EvrRtxThreadUnblocked VA_RTX5_SYMBOL(va_rtx5_ThreadUnblocked)
#else
#define EVR_RTX_THREAD_UNBLOCKED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_PREEMPTED_DISABLE
#define EVR_RTX_THREAD_PREEMPTED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_SWITCHED_DISABLE
#define EVR_RTX_THREAD_SWITCHED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_EXIT_DISABLE
#define EVR_RTX_THREAD_EXIT_DISABLE
#endif
#ifndef EVR_RTX_THREAD_TERMINATE_DISABLE
#define EVR_RTX_THREAD_TERMINATE_DISABLE
#endif
#if VA_NEEDS_TASK_REGISTRY
#ifdef EVR_RTX_THREAD_DESTROYED_DISABLE
#error "Required RTX5 hook disabled: ThreadDestroyed"
#endif
#define EvrRtxThreadDestroyed VA_RTX5_SYMBOL(va_rtx5_ThreadDestroyed)
#else
#define EVR_RTX_THREAD_DESTROYED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_FEED_WATCHDOG_DISABLE
#define EVR_RTX_THREAD_FEED_WATCHDOG_DISABLE
#endif
#ifndef EVR_RTX_THREAD_FEED_WATCHDOG_DONE_DISABLE
#define EVR_RTX_THREAD_FEED_WATCHDOG_DONE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_PROTECT_PRIVILEGED_DISABLE
#define EVR_RTX_THREAD_PROTECT_PRIVILEGED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_PRIVILEGED_PROTECTED_DISABLE
#define EVR_RTX_THREAD_PRIVILEGED_PROTECTED_DISABLE
#endif
#ifndef EVR_RTX_THREAD_GET_COUNT_DISABLE
#define EVR_RTX_THREAD_GET_COUNT_DISABLE
#endif
#ifndef EVR_RTX_THREAD_ENUMERATE_DISABLE
#define EVR_RTX_THREAD_ENUMERATE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_SUSPEND_CLASS_DISABLE
#define EVR_RTX_THREAD_SUSPEND_CLASS_DISABLE
#endif
#ifndef EVR_RTX_THREAD_RESUME_CLASS_DISABLE
#define EVR_RTX_THREAD_RESUME_CLASS_DISABLE
#endif
#ifndef EVR_RTX_THREAD_TERMINATE_ZONE_DISABLE
#define EVR_RTX_THREAD_TERMINATE_ZONE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_WATCHDOG_EXPIRED_DISABLE
#define EVR_RTX_THREAD_WATCHDOG_EXPIRED_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS
#ifdef EVR_RTX_THREAD_FLAGS_ERROR_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsError"
#endif
#define EvrRtxThreadFlagsError VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsError)
#else
#define EVR_RTX_THREAD_FLAGS_ERROR_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS
#ifdef EVR_RTX_THREAD_FLAGS_SET_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsSet"
#endif
#define EvrRtxThreadFlagsSet VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsSet)
#else
#define EVR_RTX_THREAD_FLAGS_SET_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS
#ifdef EVR_RTX_THREAD_FLAGS_SET_DONE_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsSetDone"
#endif
#define EvrRtxThreadFlagsSetDone VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsSetDone)
#else
#define EVR_RTX_THREAD_FLAGS_SET_DONE_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS
#ifdef EVR_RTX_THREAD_FLAGS_CLEAR_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsClear"
#endif
#define EvrRtxThreadFlagsClear VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsClear)
#else
#define EVR_RTX_THREAD_FLAGS_CLEAR_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS
#ifdef EVR_RTX_THREAD_FLAGS_CLEAR_DONE_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsClearDone"
#endif
#define EvrRtxThreadFlagsClearDone VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsClearDone)
#else
#define EVR_RTX_THREAD_FLAGS_CLEAR_DONE_DISABLE
#endif
#ifndef EVR_RTX_THREAD_FLAGS_GET_DISABLE
#define EVR_RTX_THREAD_FLAGS_GET_DISABLE
#endif
#ifndef EVR_RTX_THREAD_FLAGS_WAIT_DISABLE
#define EVR_RTX_THREAD_FLAGS_WAIT_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_THREAD_FLAGS_WAIT_PENDING_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsWaitPending"
#endif
#define EvrRtxThreadFlagsWaitPending VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsWaitPending)
#else
#define EVR_RTX_THREAD_FLAGS_WAIT_PENDING_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_THREAD_FLAGS_WAIT_TIMEOUT_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsWaitTimeout"
#endif
#define EvrRtxThreadFlagsWaitTimeout VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsWaitTimeout)
#else
#define EVR_RTX_THREAD_FLAGS_WAIT_TIMEOUT_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_THREAD_FLAGS_WAIT_COMPLETED_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsWaitCompleted"
#endif
#define EvrRtxThreadFlagsWaitCompleted VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsWaitCompleted)
#else
#define EVR_RTX_THREAD_FLAGS_WAIT_COMPLETED_DISABLE
#endif
#if VA_TRACE_TASK_NOTIFICATIONS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_THREAD_FLAGS_WAIT_NOT_COMPLETED_DISABLE
#error "Required RTX5 hook disabled: ThreadFlagsWaitNotCompleted"
#endif
#define EvrRtxThreadFlagsWaitNotCompleted VA_RTX5_SYMBOL(va_rtx5_ThreadFlagsWaitNotCompleted)
#else
#define EVR_RTX_THREAD_FLAGS_WAIT_NOT_COMPLETED_DISABLE
#endif
#ifndef EVR_RTX_DELAY_ERROR_DISABLE
#define EVR_RTX_DELAY_ERROR_DISABLE
#endif
#ifndef EVR_RTX_DELAY_DISABLE
#define EVR_RTX_DELAY_DISABLE
#endif
#ifndef EVR_RTX_DELAY_UNTIL_DISABLE
#define EVR_RTX_DELAY_UNTIL_DISABLE
#endif
#ifndef EVR_RTX_DELAY_STARTED_DISABLE
#define EVR_RTX_DELAY_STARTED_DISABLE
#endif
#ifndef EVR_RTX_DELAY_UNTIL_STARTED_DISABLE
#define EVR_RTX_DELAY_UNTIL_STARTED_DISABLE
#endif
#if VA_TRACE_SLEEP
#ifdef EVR_RTX_DELAY_COMPLETED_DISABLE
#error "Required RTX5 hook disabled: DelayCompleted"
#endif
#define EvrRtxDelayCompleted VA_RTX5_SYMBOL(va_rtx5_DelayCompleted)
#else
#define EVR_RTX_DELAY_COMPLETED_DISABLE
#endif
#ifndef EVR_RTX_TIMER_ERROR_DISABLE
#define EVR_RTX_TIMER_ERROR_DISABLE
#endif
#ifndef EVR_RTX_TIMER_CALLBACK_DISABLE
#define EVR_RTX_TIMER_CALLBACK_DISABLE
#endif
#ifndef EVR_RTX_TIMER_NEW_DISABLE
#define EVR_RTX_TIMER_NEW_DISABLE
#endif
#if VA_TRACE_TIMERS
#ifdef EVR_RTX_TIMER_CREATED_DISABLE
#error "Required RTX5 hook disabled: TimerCreated"
#endif
#define EvrRtxTimerCreated VA_RTX5_SYMBOL(va_rtx5_TimerCreated)
#else
#define EVR_RTX_TIMER_CREATED_DISABLE
#endif
#ifndef EVR_RTX_TIMER_GET_NAME_DISABLE
#define EVR_RTX_TIMER_GET_NAME_DISABLE
#endif
#ifndef EVR_RTX_TIMER_START_DISABLE
#define EVR_RTX_TIMER_START_DISABLE
#endif
#if VA_TRACE_TIMERS
#ifdef EVR_RTX_TIMER_STARTED_DISABLE
#error "Required RTX5 hook disabled: TimerStarted"
#endif
#define EvrRtxTimerStarted VA_RTX5_SYMBOL(va_rtx5_TimerStarted)
#else
#define EVR_RTX_TIMER_STARTED_DISABLE
#endif
#ifndef EVR_RTX_TIMER_STOP_DISABLE
#define EVR_RTX_TIMER_STOP_DISABLE
#endif
#if VA_TRACE_TIMERS
#ifdef EVR_RTX_TIMER_STOPPED_DISABLE
#error "Required RTX5 hook disabled: TimerStopped"
#endif
#define EvrRtxTimerStopped VA_RTX5_SYMBOL(va_rtx5_TimerStopped)
#else
#define EVR_RTX_TIMER_STOPPED_DISABLE
#endif
#ifndef EVR_RTX_TIMER_IS_RUNNING_DISABLE
#define EVR_RTX_TIMER_IS_RUNNING_DISABLE
#endif
#ifndef EVR_RTX_TIMER_DELETE_DISABLE
#define EVR_RTX_TIMER_DELETE_DISABLE
#endif
#if VA_TRACE_TIMERS
#ifdef EVR_RTX_TIMER_DESTROYED_DISABLE
#error "Required RTX5 hook disabled: TimerDestroyed"
#endif
#define EvrRtxTimerDestroyed VA_RTX5_SYMBOL(va_rtx5_TimerDestroyed)
#else
#define EVR_RTX_TIMER_DESTROYED_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS
#ifdef EVR_RTX_EVENT_FLAGS_ERROR_DISABLE
#error "Required RTX5 hook disabled: EventFlagsError"
#endif
#define EvrRtxEventFlagsError VA_RTX5_SYMBOL(va_rtx5_EventFlagsError)
#else
#define EVR_RTX_EVENT_FLAGS_ERROR_DISABLE
#endif
#ifndef EVR_RTX_EVENT_FLAGS_NEW_DISABLE
#define EVR_RTX_EVENT_FLAGS_NEW_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_EVENT_FLAGS_CREATED_DISABLE
#error "Required RTX5 hook disabled: EventFlagsCreated"
#endif
#define EvrRtxEventFlagsCreated VA_RTX5_SYMBOL(va_rtx5_EventFlagsCreated)
#else
#define EVR_RTX_EVENT_FLAGS_CREATED_DISABLE
#endif
#ifndef EVR_RTX_EVENT_FLAGS_GET_NAME_DISABLE
#define EVR_RTX_EVENT_FLAGS_GET_NAME_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS
#ifdef EVR_RTX_EVENT_FLAGS_SET_DISABLE
#error "Required RTX5 hook disabled: EventFlagsSet"
#endif
#define EvrRtxEventFlagsSet VA_RTX5_SYMBOL(va_rtx5_EventFlagsSet)
#else
#define EVR_RTX_EVENT_FLAGS_SET_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS
#ifdef EVR_RTX_EVENT_FLAGS_SET_DONE_DISABLE
#error "Required RTX5 hook disabled: EventFlagsSetDone"
#endif
#define EvrRtxEventFlagsSetDone VA_RTX5_SYMBOL(va_rtx5_EventFlagsSetDone)
#else
#define EVR_RTX_EVENT_FLAGS_SET_DONE_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS
#ifdef EVR_RTX_EVENT_FLAGS_CLEAR_DISABLE
#error "Required RTX5 hook disabled: EventFlagsClear"
#endif
#define EvrRtxEventFlagsClear VA_RTX5_SYMBOL(va_rtx5_EventFlagsClear)
#else
#define EVR_RTX_EVENT_FLAGS_CLEAR_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS
#ifdef EVR_RTX_EVENT_FLAGS_CLEAR_DONE_DISABLE
#error "Required RTX5 hook disabled: EventFlagsClearDone"
#endif
#define EvrRtxEventFlagsClearDone VA_RTX5_SYMBOL(va_rtx5_EventFlagsClearDone)
#else
#define EVR_RTX_EVENT_FLAGS_CLEAR_DONE_DISABLE
#endif
#ifndef EVR_RTX_EVENT_FLAGS_GET_DISABLE
#define EVR_RTX_EVENT_FLAGS_GET_DISABLE
#endif
#ifndef EVR_RTX_EVENT_FLAGS_WAIT_DISABLE
#define EVR_RTX_EVENT_FLAGS_WAIT_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_EVENT_FLAGS_WAIT_PENDING_DISABLE
#error "Required RTX5 hook disabled: EventFlagsWaitPending"
#endif
#define EvrRtxEventFlagsWaitPending VA_RTX5_SYMBOL(va_rtx5_EventFlagsWaitPending)
#else
#define EVR_RTX_EVENT_FLAGS_WAIT_PENDING_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_EVENT_FLAGS_WAIT_TIMEOUT_DISABLE
#error "Required RTX5 hook disabled: EventFlagsWaitTimeout"
#endif
#define EvrRtxEventFlagsWaitTimeout VA_RTX5_SYMBOL(va_rtx5_EventFlagsWaitTimeout)
#else
#define EVR_RTX_EVENT_FLAGS_WAIT_TIMEOUT_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_EVENT_FLAGS_WAIT_COMPLETED_DISABLE
#error "Required RTX5 hook disabled: EventFlagsWaitCompleted"
#endif
#define EvrRtxEventFlagsWaitCompleted VA_RTX5_SYMBOL(va_rtx5_EventFlagsWaitCompleted)
#else
#define EVR_RTX_EVENT_FLAGS_WAIT_COMPLETED_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_EVENT_FLAGS_WAIT_NOT_COMPLETED_DISABLE
#error "Required RTX5 hook disabled: EventFlagsWaitNotCompleted"
#endif
#define EvrRtxEventFlagsWaitNotCompleted VA_RTX5_SYMBOL(va_rtx5_EventFlagsWaitNotCompleted)
#else
#define EVR_RTX_EVENT_FLAGS_WAIT_NOT_COMPLETED_DISABLE
#endif
#ifndef EVR_RTX_EVENT_FLAGS_DELETE_DISABLE
#define EVR_RTX_EVENT_FLAGS_DELETE_DISABLE
#endif
#if VA_TRACE_EVENT_FLAGS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_EVENT_FLAGS_DESTROYED_DISABLE
#error "Required RTX5 hook disabled: EventFlagsDestroyed"
#endif
#define EvrRtxEventFlagsDestroyed VA_RTX5_SYMBOL(va_rtx5_EventFlagsDestroyed)
#else
#define EVR_RTX_EVENT_FLAGS_DESTROYED_DISABLE
#endif
#ifndef EVR_RTX_MUTEX_ERROR_DISABLE
#define EVR_RTX_MUTEX_ERROR_DISABLE
#endif
#ifndef EVR_RTX_MUTEX_NEW_DISABLE
#define EVR_RTX_MUTEX_NEW_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_CREATED_DISABLE
#error "Required RTX5 hook disabled: MutexCreated"
#endif
#define EvrRtxMutexCreated VA_RTX5_SYMBOL(va_rtx5_MutexCreated)
#else
#define EVR_RTX_MUTEX_CREATED_DISABLE
#endif
#ifndef EVR_RTX_MUTEX_GET_NAME_DISABLE
#define EVR_RTX_MUTEX_GET_NAME_DISABLE
#endif
#ifndef EVR_RTX_MUTEX_ACQUIRE_DISABLE
#define EVR_RTX_MUTEX_ACQUIRE_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_ACQUIRE_PENDING_DISABLE
#error "Required RTX5 hook disabled: MutexAcquirePending"
#endif
#define EvrRtxMutexAcquirePending VA_RTX5_SYMBOL(va_rtx5_MutexAcquirePending)
#else
#define EVR_RTX_MUTEX_ACQUIRE_PENDING_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_ACQUIRE_TIMEOUT_DISABLE
#error "Required RTX5 hook disabled: MutexAcquireTimeout"
#endif
#define EvrRtxMutexAcquireTimeout VA_RTX5_SYMBOL(va_rtx5_MutexAcquireTimeout)
#else
#define EVR_RTX_MUTEX_ACQUIRE_TIMEOUT_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_ACQUIRED_DISABLE
#error "Required RTX5 hook disabled: MutexAcquired"
#endif
#define EvrRtxMutexAcquired VA_RTX5_SYMBOL(va_rtx5_MutexAcquired)
#else
#define EVR_RTX_MUTEX_ACQUIRED_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_NOT_ACQUIRED_DISABLE
#error "Required RTX5 hook disabled: MutexNotAcquired"
#endif
#define EvrRtxMutexNotAcquired VA_RTX5_SYMBOL(va_rtx5_MutexNotAcquired)
#else
#define EVR_RTX_MUTEX_NOT_ACQUIRED_DISABLE
#endif
#ifndef EVR_RTX_MUTEX_RELEASE_DISABLE
#define EVR_RTX_MUTEX_RELEASE_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_RELEASED_DISABLE
#error "Required RTX5 hook disabled: MutexReleased"
#endif
#define EvrRtxMutexReleased VA_RTX5_SYMBOL(va_rtx5_MutexReleased)
#else
#define EVR_RTX_MUTEX_RELEASED_DISABLE
#endif
#ifndef EVR_RTX_MUTEX_GET_OWNER_DISABLE
#define EVR_RTX_MUTEX_GET_OWNER_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_DELETE_DISABLE
#error "Required RTX5 hook disabled: MutexDelete"
#endif
#define EvrRtxMutexDelete VA_RTX5_SYMBOL(va_rtx5_MutexDelete)
#else
#define EVR_RTX_MUTEX_DELETE_DISABLE
#endif
#if VA_TRACE_MUTEXES || VA_TRACE_MUTEX_CONTENTION || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MUTEX_DESTROYED_DISABLE
#error "Required RTX5 hook disabled: MutexDestroyed"
#endif
#define EvrRtxMutexDestroyed VA_RTX5_SYMBOL(va_rtx5_MutexDestroyed)
#else
#define EVR_RTX_MUTEX_DESTROYED_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_ERROR_DISABLE
#error "Required RTX5 hook disabled: SemaphoreError"
#endif
#define EvrRtxSemaphoreError VA_RTX5_SYMBOL(va_rtx5_SemaphoreError)
#else
#define EVR_RTX_SEMAPHORE_ERROR_DISABLE
#endif
#ifndef EVR_RTX_SEMAPHORE_NEW_DISABLE
#define EVR_RTX_SEMAPHORE_NEW_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_CREATED_DISABLE
#error "Required RTX5 hook disabled: SemaphoreCreated"
#endif
#define EvrRtxSemaphoreCreated VA_RTX5_SYMBOL(va_rtx5_SemaphoreCreated)
#else
#define EVR_RTX_SEMAPHORE_CREATED_DISABLE
#endif
#ifndef EVR_RTX_SEMAPHORE_GET_NAME_DISABLE
#define EVR_RTX_SEMAPHORE_GET_NAME_DISABLE
#endif
#ifndef EVR_RTX_SEMAPHORE_ACQUIRE_DISABLE
#define EVR_RTX_SEMAPHORE_ACQUIRE_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_ACQUIRE_PENDING_DISABLE
#error "Required RTX5 hook disabled: SemaphoreAcquirePending"
#endif
#define EvrRtxSemaphoreAcquirePending VA_RTX5_SYMBOL(va_rtx5_SemaphoreAcquirePending)
#else
#define EVR_RTX_SEMAPHORE_ACQUIRE_PENDING_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_ACQUIRE_TIMEOUT_DISABLE
#error "Required RTX5 hook disabled: SemaphoreAcquireTimeout"
#endif
#define EvrRtxSemaphoreAcquireTimeout VA_RTX5_SYMBOL(va_rtx5_SemaphoreAcquireTimeout)
#else
#define EVR_RTX_SEMAPHORE_ACQUIRE_TIMEOUT_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_ACQUIRED_DISABLE
#error "Required RTX5 hook disabled: SemaphoreAcquired"
#endif
#define EvrRtxSemaphoreAcquired VA_RTX5_SYMBOL(va_rtx5_SemaphoreAcquired)
#else
#define EVR_RTX_SEMAPHORE_ACQUIRED_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_NOT_ACQUIRED_DISABLE
#error "Required RTX5 hook disabled: SemaphoreNotAcquired"
#endif
#define EvrRtxSemaphoreNotAcquired VA_RTX5_SYMBOL(va_rtx5_SemaphoreNotAcquired)
#else
#define EVR_RTX_SEMAPHORE_NOT_ACQUIRED_DISABLE
#endif
#ifndef EVR_RTX_SEMAPHORE_RELEASE_DISABLE
#define EVR_RTX_SEMAPHORE_RELEASE_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_RELEASED_DISABLE
#error "Required RTX5 hook disabled: SemaphoreReleased"
#endif
#define EvrRtxSemaphoreReleased VA_RTX5_SYMBOL(va_rtx5_SemaphoreReleased)
#else
#define EVR_RTX_SEMAPHORE_RELEASED_DISABLE
#endif
#ifndef EVR_RTX_SEMAPHORE_GET_COUNT_DISABLE
#define EVR_RTX_SEMAPHORE_GET_COUNT_DISABLE
#endif
#ifndef EVR_RTX_SEMAPHORE_DELETE_DISABLE
#define EVR_RTX_SEMAPHORE_DELETE_DISABLE
#endif
#if VA_TRACE_SEMAPHORES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_SEMAPHORE_DESTROYED_DISABLE
#error "Required RTX5 hook disabled: SemaphoreDestroyed"
#endif
#define EvrRtxSemaphoreDestroyed VA_RTX5_SYMBOL(va_rtx5_SemaphoreDestroyed)
#else
#define EVR_RTX_SEMAPHORE_DESTROYED_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_ERROR_DISABLE
#define EVR_RTX_MEMORY_POOL_ERROR_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_NEW_DISABLE
#define EVR_RTX_MEMORY_POOL_NEW_DISABLE
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MEMORY_POOL_CREATED_DISABLE
#error "Required RTX5 hook disabled: MemoryPoolCreated"
#endif
#define EvrRtxMemoryPoolCreated VA_RTX5_SYMBOL(va_rtx5_MemoryPoolCreated)
#else
#define EVR_RTX_MEMORY_POOL_CREATED_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_GET_NAME_DISABLE
#define EVR_RTX_MEMORY_POOL_GET_NAME_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_ALLOC_DISABLE
#define EVR_RTX_MEMORY_POOL_ALLOC_DISABLE
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MEMORY_POOL_ALLOC_PENDING_DISABLE
#error "Required RTX5 hook disabled: MemoryPoolAllocPending"
#endif
#define EvrRtxMemoryPoolAllocPending VA_RTX5_SYMBOL(va_rtx5_MemoryPoolAllocPending)
#else
#define EVR_RTX_MEMORY_POOL_ALLOC_PENDING_DISABLE
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MEMORY_POOL_ALLOC_TIMEOUT_DISABLE
#error "Required RTX5 hook disabled: MemoryPoolAllocTimeout"
#endif
#define EvrRtxMemoryPoolAllocTimeout VA_RTX5_SYMBOL(va_rtx5_MemoryPoolAllocTimeout)
#else
#define EVR_RTX_MEMORY_POOL_ALLOC_TIMEOUT_DISABLE
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MEMORY_POOL_ALLOCATED_DISABLE
#error "Required RTX5 hook disabled: MemoryPoolAllocated"
#endif
#define EvrRtxMemoryPoolAllocated VA_RTX5_SYMBOL(va_rtx5_MemoryPoolAllocated)
#else
#define EVR_RTX_MEMORY_POOL_ALLOCATED_DISABLE
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MEMORY_POOL_ALLOC_FAILED_DISABLE
#error "Required RTX5 hook disabled: MemoryPoolAllocFailed"
#endif
#define EvrRtxMemoryPoolAllocFailed VA_RTX5_SYMBOL(va_rtx5_MemoryPoolAllocFailed)
#else
#define EVR_RTX_MEMORY_POOL_ALLOC_FAILED_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_FREE_DISABLE
#define EVR_RTX_MEMORY_POOL_FREE_DISABLE
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MEMORY_POOL_DEALLOCATED_DISABLE
#error "Required RTX5 hook disabled: MemoryPoolDeallocated"
#endif
#define EvrRtxMemoryPoolDeallocated VA_RTX5_SYMBOL(va_rtx5_MemoryPoolDeallocated)
#else
#define EVR_RTX_MEMORY_POOL_DEALLOCATED_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_FREE_FAILED_DISABLE
#define EVR_RTX_MEMORY_POOL_FREE_FAILED_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_GET_CAPACITY_DISABLE
#define EVR_RTX_MEMORY_POOL_GET_CAPACITY_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_GET_BLOCK_SZIE_DISABLE
#define EVR_RTX_MEMORY_POOL_GET_BLOCK_SZIE_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_GET_COUNT_DISABLE
#define EVR_RTX_MEMORY_POOL_GET_COUNT_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_GET_SPACE_DISABLE
#define EVR_RTX_MEMORY_POOL_GET_SPACE_DISABLE
#endif
#ifndef EVR_RTX_MEMORY_POOL_DELETE_DISABLE
#define EVR_RTX_MEMORY_POOL_DELETE_DISABLE
#endif
#if VA_TRACE_MEM_SLABS || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MEMORY_POOL_DESTROYED_DISABLE
#error "Required RTX5 hook disabled: MemoryPoolDestroyed"
#endif
#define EvrRtxMemoryPoolDestroyed VA_RTX5_SYMBOL(va_rtx5_MemoryPoolDestroyed)
#else
#define EVR_RTX_MEMORY_POOL_DESTROYED_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_ERROR_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_ERROR_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_NEW_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_NEW_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_CREATED_DISABLE
#error "Required RTX5 hook disabled: MessageQueueCreated"
#endif
#define EvrRtxMessageQueueCreated VA_RTX5_SYMBOL(va_rtx5_MessageQueueCreated)
#else
#define EVR_RTX_MESSAGE_QUEUE_CREATED_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_GET_NAME_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_GET_NAME_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_PUT_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_PUT_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_PUT_PENDING_DISABLE
#error "Required RTX5 hook disabled: MessageQueuePutPending"
#endif
#define EvrRtxMessageQueuePutPending VA_RTX5_SYMBOL(va_rtx5_MessageQueuePutPending)
#else
#define EVR_RTX_MESSAGE_QUEUE_PUT_PENDING_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_PUT_TIMEOUT_DISABLE
#error "Required RTX5 hook disabled: MessageQueuePutTimeout"
#endif
#define EvrRtxMessageQueuePutTimeout VA_RTX5_SYMBOL(va_rtx5_MessageQueuePutTimeout)
#else
#define EVR_RTX_MESSAGE_QUEUE_PUT_TIMEOUT_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_INSERT_PENDING_DISABLE
#error "Required RTX5 hook disabled: MessageQueueInsertPending"
#endif
#define EvrRtxMessageQueueInsertPending VA_RTX5_SYMBOL(va_rtx5_MessageQueueInsertPending)
#else
#define EVR_RTX_MESSAGE_QUEUE_INSERT_PENDING_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_INSERTED_DISABLE
#error "Required RTX5 hook disabled: MessageQueueInserted"
#endif
#define EvrRtxMessageQueueInserted VA_RTX5_SYMBOL(va_rtx5_MessageQueueInserted)
#else
#define EVR_RTX_MESSAGE_QUEUE_INSERTED_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_NOT_INSERTED_DISABLE
#error "Required RTX5 hook disabled: MessageQueueNotInserted"
#endif
#define EvrRtxMessageQueueNotInserted VA_RTX5_SYMBOL(va_rtx5_MessageQueueNotInserted)
#else
#define EVR_RTX_MESSAGE_QUEUE_NOT_INSERTED_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_GET_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_GET_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_GET_PENDING_DISABLE
#error "Required RTX5 hook disabled: MessageQueueGetPending"
#endif
#define EvrRtxMessageQueueGetPending VA_RTX5_SYMBOL(va_rtx5_MessageQueueGetPending)
#else
#define EVR_RTX_MESSAGE_QUEUE_GET_PENDING_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_GET_TIMEOUT_DISABLE
#error "Required RTX5 hook disabled: MessageQueueGetTimeout"
#endif
#define EvrRtxMessageQueueGetTimeout VA_RTX5_SYMBOL(va_rtx5_MessageQueueGetTimeout)
#else
#define EVR_RTX_MESSAGE_QUEUE_GET_TIMEOUT_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_RETRIEVED_DISABLE
#error "Required RTX5 hook disabled: MessageQueueRetrieved"
#endif
#define EvrRtxMessageQueueRetrieved VA_RTX5_SYMBOL(va_rtx5_MessageQueueRetrieved)
#else
#define EVR_RTX_MESSAGE_QUEUE_RETRIEVED_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_NOT_RETRIEVED_DISABLE
#error "Required RTX5 hook disabled: MessageQueueNotRetrieved"
#endif
#define EvrRtxMessageQueueNotRetrieved VA_RTX5_SYMBOL(va_rtx5_MessageQueueNotRetrieved)
#else
#define EVR_RTX_MESSAGE_QUEUE_NOT_RETRIEVED_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_GET_CAPACITY_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_GET_CAPACITY_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_GET_MSG_SIZE_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_GET_MSG_SIZE_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_GET_COUNT_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_GET_COUNT_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_GET_SPACE_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_GET_SPACE_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_RESET_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_RESET_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_RESET_DONE_DISABLE
#error "Required RTX5 hook disabled: MessageQueueResetDone"
#endif
#define EvrRtxMessageQueueResetDone VA_RTX5_SYMBOL(va_rtx5_MessageQueueResetDone)
#else
#define EVR_RTX_MESSAGE_QUEUE_RESET_DONE_DISABLE
#endif
#ifndef EVR_RTX_MESSAGE_QUEUE_DELETE_DISABLE
#define EVR_RTX_MESSAGE_QUEUE_DELETE_DISABLE
#endif
#if VA_TRACE_QUEUES || VA_TRACE_TASK_STATES
#ifdef EVR_RTX_MESSAGE_QUEUE_DESTROYED_DISABLE
#error "Required RTX5 hook disabled: MessageQueueDestroyed"
#endif
#define EvrRtxMessageQueueDestroyed VA_RTX5_SYMBOL(va_rtx5_MessageQueueDestroyed)
#else
#define EVR_RTX_MESSAGE_QUEUE_DESTROYED_DISABLE
#endif
#if OS_EVR_INIT
#error "ViewAlyzer RTX5 does not use Arm EventRecorder initialization; set OS_EVR_INIT=0"
#endif
#ifdef __cplusplus
extern "C" {
#endif
#include "rtx_evr.h"
#if defined(RTX_SAFETY_FEATURES) || defined(RTX_TZ_CONTEXT)
#error "ViewAlyzer RTX5 currently supports the GNU Armv7-M non-TrustZone port without RTX safety extensions"
#endif
#if VA_NEEDS_TASK_REGISTRY
/* The adapter owns this strong override. A second application override must
   fail to link rather than silently bypassing either cleanup path. */
void osRtxThreadBeforeFree(osRtxThread_t *thread);
#endif
#if VA_TRACE_TIMERS && VA_TRACE_TIMER_CALLBACKS
void VA_RTX5_TimerEnter(osTimerId_t timer);
void VA_RTX5_TimerExit(osTimerId_t timer);
#endif
#ifdef __cplusplus
}
#endif
#if osRtxVersionKernel != 50090001
#error "ViewAlyzer RTX5 adapter requires CMSIS-RTX 5.9.1"
#endif
#if VA_TRACE_STACK_USAGE && !OS_STACK_WATERMARK
#error "RTX5 stack usage requires OS_STACK_WATERMARK=1"
#endif
#else
#ifndef EVR_RTX_DISABLE
#define EVR_RTX_DISABLE
#endif
#endif
#endif
