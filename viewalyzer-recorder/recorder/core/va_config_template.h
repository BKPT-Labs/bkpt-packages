/**
 * @file va_config_template.h
 * @brief Template for a project-owned ViewAlyzer configuration header.
 *
 * Copy this file into YOUR project (e.g. as va_config.h), uncomment the
 * knobs you want to change, and build with
 *
 *     -DVA_CONFIG_HEADER=va_config.h
 *
 * (a bare token: no quotes, no angle brackets). It is included before every
 * default in ViewAlyzerConfig.h, so anything set here wins. Because it lives
 * outside the vendored recorder tree, package updates leave it alone -
 * unlike edits made to ViewAlyzerConfig.h itself.
 *
 * Every line below shows the default. Leave a line commented and you get
 * that default; this file changes nothing until you uncomment something.
 * The full knob list with detailed comments is in ViewAlyzerConfig.h.
 *
 * Copyright 2025-2026 BKPT, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VA_CONFIG_H
#define VA_CONFIG_H

/* Master switch: 0 compiles the recorder out entirely. */
/* #define VA_ENABLED 1 */

/* RTOS: VA_RTOS_NONE, VA_RTOS_FREERTOS, or VA_RTOS_ZEPHYR.
   (Usually set by your build system; Zephyr sets it from Kconfig.) */
/* #define VA_RTOS_SELECT VA_RTOS_NONE */

/* ── What gets traced ──────────────────────────────────────────────
   Every category defaults to VA_TRACE_DEFAULT. Two ways to use this:
   - opt OUT: leave VA_TRACE_DEFAULT at 1 and set the noisy ones to 0, or
   - opt IN:  set VA_TRACE_DEFAULT to 0 and name what you want at 1.
   A disabled category costs nothing: events, packet builders, registries,
   and kernel hooks are all compiled away. */

/* #define VA_TRACE_DEFAULT 1 */

/* RTOS scheduling */
/* #define VA_TRACE_TASKS              1 */
/* #define VA_TRACE_TASK_NOTIFICATIONS 1 */
/* #define VA_TRACE_STACK_USAGE        1 */
/* #define VA_TRACE_ISRS               1 */

/* RTOS synchronisation objects */
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

/* User instrumentation (works on bare metal too) */
/* #define VA_TRACE_USER_VALUES        1 */
/* #define VA_TRACE_USER_EVENTS        1 */
/* #define VA_TRACE_STRINGS            1 */
/* #define VA_TRACE_GPIO               1 */
/* #define VA_TRACE_COUNTERS           1 */
/* #define VA_TRACE_HEAP_METRICS       1 */

/* ── Timestamp source ──────────────────────────────────────────────
   DWT_CYCCNT (default) or CUSTOM_TIMER.

   Pick CUSTOM_TIMER when the core has no DWT cycle counter (Cortex-M0/
   M0+/M23) or the vendor omitted it. VA_Init() then takes your tick
   source alongside the CPU clock:

       VA_Init(cpu_freq, my_read_ticks, tick_hz);

   where my_read_ticks is `uint32_t fn(void)` returning a FREE-RUNNING
   counter (e.g. `return TIM2->CNT;`). The timer must be running before
   VA_Init(), must never pause while the CPU runs, and the function must
   be safe to call from any context (including interrupts masked), take
   no locks, and be cheap - it runs inside every trace hook. Any tick
   rate works; timestamp resolution = one tick.

   Wrap budget: the recorder needs one clock read per counter wrap.
   Events do this for free; across QUIET gaps call VA_TickOverflowCheck()
   more often than the wrap period (2^bits / tick_hz):
       32-bit @ 1 MHz      wraps every ~71 min  - trivial
       16-bit @ 32.768 kHz wraps every 2 s      - check every <= 1 s
       16-bit @ 1 MHz      wraps every 65 ms    - prefer a 32-bit timer!
*/

/* #define VA_TIMESTAMP_SOURCE DWT_CYCCNT */
/* #define VA_TIMER_BITS       32 */

/* ── Transport ─────────────────────────────────────────────────────
   ARM_ITM, JLINK_RTT, CUSTOM_TRANSPORT, or RAM_BUFFER.
   (No ITM on Cortex-M0/M0+/M23 - use RAM_BUFFER or JLINK_RTT there.) */

/* #define VA_TRANSPORT ARM_ITM */

/* ITM */
/* #define VA_ITM_PORT 1 */

/* RTT */
/* #define VA_RTT_CHANNEL     0 */
/* #define VA_CONFIGURE_RTT   1 */
/* #define VA_RTT_BUFFER_SIZE 4096u */

/* RAM buffer */
/* #define VA_RAMBUF_SIZE 8192u */
/* #define VA_RAMBUF_MODE VA_RAMBUF_MODE_DROP */

/* Snapshot (post-mortem) ring */
/* #define VA_SNAPSHOT      0 */
/* #define VA_SNAPSHOT_SIZE 4096u */

/* ── Sizes ─────────────────────────────────────────────────────────── */

/* #define VA_MAX_TASKS          16 */
/* #define VA_MAX_SYNC_OBJECTS   64 */
/* #define VA_MAX_USER_EVENTS    16 */
/* #define VA_MAX_USER_TRACES    16 */
/* #define VA_MAX_TASK_NAME_LEN  16 */
/* #define VA_MAX_LOG_STRING_LEN 100 */

#endif /* VA_CONFIG_H */
