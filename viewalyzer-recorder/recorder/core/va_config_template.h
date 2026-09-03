/**
 * @file va_config_template.h
 * @brief Project configuration template for ViewAlyzer.
 *
 * QUICK START
 *   1. Copy this file into your project as va_config.h.
 *   2. Set VA_DEVICE_HEADER and uncomment only the overrides you need.
 *   3. Build with -DVA_CONFIG_HEADER=va_config.h.
 *
 * Optional settings retain their defaults while commented. See
 * ViewAlyzerConfig.h for the complete option reference.
 *
 * Copyright 2025-2026 BKPT, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VA_CONFIG_H
#define VA_CONFIG_H

/* --------------------------------------------------------------------------
 * CORE
 * -------------------------------------------------------------------------- */

/* Required outside Zephyr: your CMSIS device header, as a bare token. */
/* #define VA_DEVICE_HEADER stm32g4xx.h */

/* Set to 0 to compile out the recorder. */
/* #define VA_ENABLED 1 */

/* Usually supplied by the build system; Zephyr uses Kconfig. */
/* Options: VA_RTOS_NONE | VA_RTOS_FREERTOS | VA_RTOS_ZEPHYR */
/* #define VA_RTOS_SELECT VA_RTOS_NONE */

/* --------------------------------------------------------------------------
 * TRACE FILTERS
 *
 * Categories inherit VA_TRACE_DEFAULT. Keep 1 and disable noisy categories,
 * or set 0 and enable only what you need. Disabled categories compile out.
 * -------------------------------------------------------------------------- */

/* #define VA_TRACE_DEFAULT 1 */

/* Scheduling */
/* #define VA_TRACE_TASKS              1 */
/* #define VA_TRACE_TASK_NOTIFICATIONS 1 */
/* #define VA_TRACE_STACK_USAGE        1 */
/* #define VA_TRACE_ISRS               1 */

/* RTOS objects */
/* #define VA_TRACE_MUTEXES            1 */
/* #define VA_TRACE_MUTEX_CONTENTION   1 */
/* #define VA_TRACE_SEMAPHORES         1 */
/* #define VA_TRACE_QUEUES             1 */
/* #define VA_TRACE_EVENT_FLAGS        1 */
/* #define VA_TRACE_WORK               1 */
/* #define VA_TRACE_SLEEP              1 */
/* #define VA_TRACE_TIMERS             1 */
/* #define VA_TRACE_RTOS_HEAPS         1 */
/* #define VA_TRACE_PM                 1 */

/* User instrumentation (also available on bare metal) */
/* #define VA_TRACE_USER_VALUES        1 */
/* #define VA_TRACE_USER_EVENTS        1 */
/* #define VA_TRACE_STRINGS            1 */
/* #define VA_TRACE_GPIO               1 */
/* #define VA_TRACE_COUNTERS           1 */
/* #define VA_TRACE_HEAP_METRICS       1 */

/* --------------------------------------------------------------------------
 * TIMEBASE
 *
 * Default: DWT_CYCCNT. Use CUSTOM_TIMER when DWT is unavailable (such as on
 * Cortex-M0/M0+/M23), then initialize with:
 *
 *   VA_Init(cpu_freq, read_ticks, tick_hz);
 *
 * read_ticks must return a free-running uint32_t counter and be fast,
 * lock-free, and safe from any context. During quiet periods, call
 * VA_TickOverflowCheck() at least once per counter wrap. Prefer 32-bit timers.
 * -------------------------------------------------------------------------- */

/* #define VA_TIMESTAMP_SOURCE DWT_CYCCNT */

/* Custom timer width: typically 16 or 32 bits. */
/* #define VA_TIMER_BITS       32 */

/* --------------------------------------------------------------------------
 * TRANSPORT
 *
 * Options: ARM_ITM | JLINK_RTT | CUSTOM_TRANSPORT | RAM_BUFFER
 * Cortex-M0/M0+/M23: use JLINK_RTT or RAM_BUFFER (ITM is unavailable).
 * -------------------------------------------------------------------------- */

/* #define VA_TRANSPORT RAM_BUFFER */

/* ITM */
/* #define VA_ITM_PORT 1 */

/* RTT */
/* #define VA_RTT_CHANNEL     0 */
/* #define VA_CONFIGURE_RTT   1 */
/* #define VA_RTT_BUFFER_SIZE 4096u */

/* RAM buffer */
/* #define VA_RAMBUF_SIZE 8192u */
/* #define VA_RAMBUF_MODE VA_RAMBUF_MODE_DROP */

/* Optional linker placement for the RAM ring and control block. */
/* #define VA_RAMBUF_ATTRIBUTES __attribute__((section(".va_rambuf"))) */

/* Snapshot (post-mortem) ring */
/* #define VA_SNAPSHOT      0 */
/* #define VA_SNAPSHOT_SIZE 4096u */

/* Optional linker placement; use retained RAM to survive warm resets. */
/* #define VA_SNAPSHOT_ATTRIBUTES __attribute__((section(".va_snapshot"))) */

/* --------------------------------------------------------------------------
 * CAPACITY
 * -------------------------------------------------------------------------- */

/* #define VA_MAX_TASKS          16 */
/* #define VA_MAX_SYNC_OBJECTS   64 */
/* #define VA_MAX_USER_EVENTS    16 */
/* #define VA_MAX_USER_TRACES    16 */
/* #define VA_MAX_TASK_NAME_LEN  16 */
/* #define VA_MAX_LOG_STRING_LEN 100 */

#endif /* VA_CONFIG_H */
