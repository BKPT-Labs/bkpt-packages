/**
 * @file ViewAlyzerConfig.h
 * @brief ViewAlyzer Recorder - build-time configuration defaults
 *
 * Every user knob lives here: transport, sizes, buffering, and which
 * categories of events get compiled in at all. This header deliberately
 * includes NOTHING (no CMSIS, no FreeRTOS, no Zephyr), so it is safe to pull
 * in from FreeRTOSConfig.h as well as from the recorder core.
 *
 * How to configure, best option first:
 *
 *   1. A header of your own (RECOMMENDED). Copy va_config_template.h into
 *      your project (e.g. as va_config.h), set your knobs there, and build
 *      with -DVA_CONFIG_HEADER=va_config.h (a bare token: no quotes, no
 *      angle brackets). Your header is included first, so its defines win
 *      over every default below, and it lives outside the vendored recorder
 *      tree so package updates leave it alone.
 *   2. -D on the compiler command line. Every knob below is #ifndef-guarded,
 *      so -D always wins. This is how the Zephyr CMake layer maps Kconfig.
 *   3. Edit this file. Fine for a quick experiment, but the recorder gets
 *      vendored into your project by the package flow, so your edits are
 *      lost on the next package update.
 *
 * WHICHEVER you use, the configuration must reach EVERY translation unit
 * that sees ViewAlyzer - including your RTOS kernel, which compiles the
 * trace hooks. Defines made locally inside FreeRTOSConfig.h are invisible to
 * ViewAlyzer.c and will silently disagree.
 *
 * Validation and the recorder-internal flags derived from these knobs live
 * in VA_ConfigDerived.h, included at the end of this file. Nothing there is
 * a user knob.
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

#ifndef VIEWALYZER_CONFIG_H
#define VIEWALYZER_CONFIG_H

/* Stringizer for the header-name knobs. VA_CONFIG_HEADER and
   VA_DEVICE_HEADER take a BARE token (-DVA_DEVICE_HEADER=stm32g474xx.h),
   never a quoted string: quoted forms survive a POSIX shell but get mangled
   by CMD, Ninja response files, Makefiles, and the define boxes in Keil/IAR. */
#define VA_STR2(x) #x
#define VA_STR(x)  VA_STR2(x)

/* Project-owned configuration header, included before every default below. */
#if defined(VA_CONFIG_HEADER)
#include VA_STR(VA_CONFIG_HEADER)
#endif

/* ── Master switch ─────────────────────────────────────────────────── */

#ifndef VA_ENABLED
#define VA_ENABLED 1                  /* 0 compiles the recorder out entirely */
#endif

/* ── RTOS selection ────────────────────────────────────────────────── */

#define VA_RTOS_NONE     0            /* Bare metal: user traces / ISR logging only */
#define VA_RTOS_FREERTOS 1
#define VA_RTOS_ZEPHYR   2
#define VA_RTOS_THREADX  3            /* Reserved for future */

#ifndef VA_RTOS_SELECT
#define VA_RTOS_SELECT VA_RTOS_NONE
#endif

/* ── Trace categories ──────────────────────────────────────────────── */

/* Every category is independent, and each one you turn off costs nothing at
 * run time: its events, its packet builders, its registry storage, and (for
 * FreeRTOS) its kernel trace hooks are all compiled away.
 *
 * VA_TRACE_DEFAULT is the value every category falls back to, so the default
 * build traces everything. To opt in instead, turn everything off and name
 * the categories you want:
 *
 *     #define VA_TRACE_DEFAULT  0
 *     #define VA_TRACE_TASKS    1
 *     #define VA_TRACE_ISRS     1
 *
 * Categories with no implementation for your RTOS compile away silently -
 * VA_TRACE_TASK_NOTIFICATIONS has no Zephyr path, for example.
 *
 * The set of enabled categories is reported to the host in every setup
 * bundle (VA_SETUP_CONFIG_FLAGS), so a viewer can say "mutex tracing is
 * disabled in this firmware build" instead of showing an empty view. */

#ifndef VA_TRACE_DEFAULT
#define VA_TRACE_DEFAULT 1
#endif

/* RTOS scheduling */
#ifndef VA_TRACE_TASKS
#define VA_TRACE_TASKS              VA_TRACE_DEFAULT /* Task switch + create, task name map */
#endif
#ifndef VA_TRACE_TASK_NOTIFICATIONS
#define VA_TRACE_TASK_NOTIFICATIONS VA_TRACE_DEFAULT /* FreeRTOS task notifications */
#endif
#ifndef VA_TRACE_STACK_USAGE
#define VA_TRACE_STACK_USAGE        VA_TRACE_DEFAULT /* Per-task stack high-water packets */
#endif
#ifndef VA_TRACE_ISRS
#define VA_TRACE_ISRS               VA_TRACE_DEFAULT /* VA_LogISRStart/End, ISR name map */
#endif

/* RTOS synchronisation objects */
#ifndef VA_TRACE_MUTEXES
#define VA_TRACE_MUTEXES            VA_TRACE_DEFAULT /* Mutex give/take */
#endif
#ifndef VA_TRACE_MUTEX_CONTENTION
#define VA_TRACE_MUTEX_CONTENTION   VA_TRACE_DEFAULT /* Who blocked on whom */
#endif
#ifndef VA_TRACE_SEMAPHORES
#define VA_TRACE_SEMAPHORES         VA_TRACE_DEFAULT /* Binary + counting semaphores */
#endif
#ifndef VA_TRACE_QUEUES
#define VA_TRACE_QUEUES             VA_TRACE_DEFAULT /* Queues / message queues */
#endif
#ifndef VA_TRACE_EVENT_FLAGS
#define VA_TRACE_EVENT_FLAGS        VA_TRACE_DEFAULT /* Event groups / k_event */
#endif
#ifndef VA_TRACE_WORK
#define VA_TRACE_WORK               VA_TRACE_DEFAULT /* Deferred work (Zephyr k_work) */
#endif
#ifndef VA_TRACE_SLEEP
#define VA_TRACE_SLEEP              VA_TRACE_DEFAULT /* Explicit sleep (k_sleep family, vTaskDelay/suspend) */
#endif
#ifndef VA_TRACE_TIMERS
#define VA_TRACE_TIMERS             VA_TRACE_DEFAULT /* Kernel timers */
#endif
#ifndef VA_TRACE_RTOS_HEAPS
#define VA_TRACE_RTOS_HEAPS         VA_TRACE_DEFAULT /* Kernel allocator alloc/free/fail */
#endif
#ifndef VA_TRACE_PM
#define VA_TRACE_PM                 VA_TRACE_DEFAULT /* Power-management suspend/resume */
#endif

/* User instrumentation (works on bare metal too) */
#ifndef VA_TRACE_USER_VALUES
#define VA_TRACE_USER_VALUES        VA_TRACE_DEFAULT /* VA_LogTrace / VA_LogTraceFloat / VA_LogToggle */
#endif
#ifndef VA_TRACE_USER_EVENTS
#define VA_TRACE_USER_EVENTS        VA_TRACE_DEFAULT /* VA_LogEvent spans (VA_EVENT_START/END) */
#endif
#ifndef VA_TRACE_STRINGS
#define VA_TRACE_STRINGS            VA_TRACE_DEFAULT /* VA_LogString */
#endif
#ifndef VA_TRACE_GPIO
#define VA_TRACE_GPIO               VA_TRACE_DEFAULT /* VA_LogGPIO */
#endif
#ifndef VA_TRACE_COUNTERS
#define VA_TRACE_COUNTERS           VA_TRACE_DEFAULT /* VA_LogCounter */
#endif
#ifndef VA_TRACE_HEAP_METRICS
#define VA_TRACE_HEAP_METRICS       VA_TRACE_DEFAULT /* VA_LogHeap (manual heap gauge) */
#endif

/* ── Transport selection ───────────────────────────────────────────── */

#define ARM_ITM          1u           /* ITM stimulus port (SWO pin, e.g. ST-LINK) */
#define JLINK_RTT        2u           /* SEGGER RTT over a J-Link probe */
#define CUSTOM_TRANSPORT 3u           /* You provide the output function */
#define RAM_BUFFER       4u           /* RAM ring drained by any debug probe */

#ifndef VA_TRANSPORT
#define VA_TRANSPORT RAM_BUFFER
#endif

/* ── ITM transport options ─────────────────────────────────────────── */

#ifndef VA_ITM_PORT
#define VA_ITM_PORT 1                 /* ITM stimulus port recorder packets are sent on */
#endif

/* ── RTT transport options ─────────────────────────────────────────── */

#ifndef VA_RTT_CHANNEL
#define VA_RTT_CHANNEL 0              /* RTT up-channel used for recorder packets */
#endif
#ifndef VA_CONFIGURE_RTT
#define VA_CONFIGURE_RTT 1            /* 1 = recorder configures the channel; 0 = you init RTT yourself */
#endif
#ifndef VA_RTT_BUFFER_SIZE
#define VA_RTT_BUFFER_SIZE 4096u      /* Bytes reserved for the RTT up-buffer */
#endif
/* RTT buffering mode. The default, NO_BLOCK_SKIP, never stalls the
   firmware: when the RTT buffer is full a packet is skipped WHOLE (the
   stream stays parseable) and the host detects and quantifies the loss
   from the per-packet sequence numbers. Set
   VA_RTT_MODE=SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL for guaranteed-lossless
   captures instead - but know that a blocking build freezes inside a
   trace call once the buffer fills if no host is draining the channel,
   so it must only boot with a J-Link attached and reading. */
#ifndef VA_RTT_MODE
#define VA_RTT_MODE SEGGER_RTT_MODE_NO_BLOCK_SKIP
#endif

/* ── RAM buffer transport options ──────────────────────────────────── */

/* The recorder owns a ring buffer + control block in target RAM and the host
   drains it through the debug probe with non-intrusive memory reads -
   RTT-style streaming that works with ST-Link and any other probe, no SEGGER
   code required. The host finds the control block by scanning RAM for its
   magic tag, or directly via the _VA_RAMBUF ELF symbol. */

#ifndef VA_RAMBUF_SIZE
#define VA_RAMBUF_SIZE 8192u          /* Bytes reserved for the trace ring buffer */
#endif

#define VA_RAMBUF_MODE_DROP  0        /* Buffer full: drop whole packets, count them (default) */
#define VA_RAMBUF_MODE_BLOCK 1        /* Buffer full: wait for host to drain (lossless). The wait
                                         spins inside the trace call's critical section, i.e. with
                                         interrupts MASKED: with no host draining, this is a hard
                                         hang (no ticks, no watchdog kicks), not a recoverable
                                         stall. Only boot BLOCK builds with a host attached. */
#define VA_RAMBUF_MODE_WRAP  2        /* Post-mortem snapshot: overwrite the oldest packets, keep
                                         the newest window. No live streaming in this mode - the
                                         ring is read out after the fact with
                                         "ViewAlyzer --headless --snapshot" (or the GUI
                                         Take Snapshot button). */
#ifndef VA_RAMBUF_MODE
#define VA_RAMBUF_MODE VA_RAMBUF_MODE_DROP
#endif

/* Optional placement attributes for the buffer + control block, e.g.
   __attribute__((section(".va_rambuf"))) to pin them to a non-cacheable RAM
   region on cached parts (Cortex-M7 with D-cache enabled). */
#ifndef VA_RAMBUF_ATTRIBUTES
#define VA_RAMBUF_ATTRIBUTES
#endif

/* Keep the core out of WFI while the RAM-buffer transport is active: some
   probe/MCU combos return garbage for debug memory reads while the core
   sleeps, which shows up as stream corruption. 1 = the idle path spins
   (interrupts still serviced) instead of sleeping; 0 = normal sleep.
   Consumed by the Zephyr adapter (Kconfig: VIEWALYZER_RAMBUF_BUSY_IDLE);
   other adapters/bare metal must keep the core awake themselves if their
   probe misreads during sleep. */
#ifndef VA_RAMBUF_BUSY_IDLE
#define VA_RAMBUF_BUSY_IDLE 1
#endif

/* ── Snapshot (post-mortem) ring ───────────────────────────────────── */

/* Optional second RAM ring that keeps the most recent trace window for
   post-mortem inspection while the normal transport streams live. Every
   emitted packet is teed into a wrapping ring (oldest packets overwritten),
   which the host reads out after a crash or hang with
   "ViewAlyzer --headless --snapshot" - no host connection is needed while
   the system runs. Works with every transport (ITM, RTT, RAM_BUFFER,
   custom). Off by default: it costs VA_SNAPSHOT_SIZE bytes of RAM plus one
   length byte per packet in the ring.

   If the whole build is untethered (no live streaming wanted), prefer
   VA_TRANSPORT = RAM_BUFFER with VA_RAMBUF_MODE = VA_RAMBUF_MODE_WRAP
   instead: same snapshot behaviour, no second buffer. Combining that mode
   with VA_SNAPSHOT is rejected (two identical snapshot rings).

   From a fault or assert handler, call VA_SnapshotFreeze() to stop further
   snapshot writes so the window at the moment of the fault is preserved.

   Reading the ring from a RUNNING target has no torn-read detection: the
   host can land mid-frame if the target keeps writing while it walks the
   ring. Snapshot dumps should halt the target first (the app's snapshot
   flows do) or follow a VA_SnapshotFreeze(). */

#ifndef VA_SNAPSHOT
#define VA_SNAPSHOT 0                 /* 1 = enable the snapshot tee ring */
#endif
#ifndef VA_SNAPSHOT_SIZE
#define VA_SNAPSHOT_SIZE 4096u        /* Bytes reserved for the snapshot ring */
#endif

/* Optional placement attributes for the snapshot ring + control block,
   e.g. __attribute__((section(".va_snapshot"))) to pin them to a RAM region
   that survives a warm reset, or a non-cacheable region on cached parts. */
#ifndef VA_SNAPSHOT_ATTRIBUTES
#define VA_SNAPSHOT_ATTRIBUTES
#endif

/* Names area beside the snapshot ring (applies to VA_SNAPSHOT and to
   VA_RAMBUF_MODE_WRAP). A small snapshot ring usually holds well under one
   setup-bundle period (VA_AUTO_SETUP_INTERVAL_MS) of events, so the wrapped
   window would carry no task/trace names at all. This area always holds the
   LATEST setup bundle (task, ISR, trace and sync-object names; CLK; build
   flags) - rewritten in place on every periodic bundle, never overwritten
   by events - and the snapshot dump prepends it to the window, so snapshots
   are fully named no matter how small the ring is. Size it to hold your
   registries (roughly 20 bytes per named entity); overflow drops whole
   packets off the end (some names missing). 0 = disable (saves the RAM,
   loses the names). */
#ifndef VA_SNAPSHOT_SETUP_SIZE
#define VA_SNAPSHOT_SETUP_SIZE 1024u
#endif

/* ── Buffered transport mode ───────────────────────────────────────── */

/* When 1, trace hooks copy encoded packets into a RAM ring buffer (bounded,
   non-blocking) instead of pushing them straight to the wire, and
   VA_Drain() - called from your idle thread / main loop - flushes the ring
   to the transport. This bounds per-hook latency regardless of wire
   backpressure, at the cost of the on-wire byte position lagging the event
   (so it is INCOMPATIBLE with fused ETM+ITM time-correlation captures).
   0 = unbuffered (default): each hook writes directly to the wire. */

#ifndef VA_TRANSPORT_BUFFERED
#define VA_TRANSPORT_BUFFERED 0
#endif
#ifndef VA_BUFFER_SIZE
#define VA_BUFFER_SIZE 4096           /* Ring capacity (bytes) when VA_TRANSPORT_BUFFERED */
#endif

/* ── Registry sizes ────────────────────────────────────────────────── */

#ifndef VA_MAX_TASKS
#define VA_MAX_TASKS          16      /* RTOS task/thread slots */
#endif
#ifndef VA_MAX_SYNC_OBJECTS
#define VA_MAX_SYNC_OBJECTS   64      /* Mutexes, semaphores, queues, timers, heaps */
#endif
#ifndef VA_MAX_USER_EVENTS
#define VA_MAX_USER_EVENTS    16      /* VA_RegisterUserEvent() spans/events */
#endif

/* VA_RegisterUserTrace() registrations: graphs, counters, toggles, bars and
   ISR names. A SEPARATE budget from VA_MAX_USER_EVENTS. Raise it if
   VA_RegisterUserTrace() is called more times than there are slots (19 bytes
   of .bss per slot); overflow is reported on the wire and the viewer names
   the knob to raise. */
#ifndef VA_MAX_USER_TRACES
#define VA_MAX_USER_TRACES    16
#endif

#ifndef VA_MAX_GPIOS
#define VA_MAX_GPIOS          16      /* VA_RegisterGPIO() channels */
#endif
#ifndef VA_MAX_HEAPS
#define VA_MAX_HEAPS          8       /* VA_RegisterHeap() gauges */
#endif

#ifndef VA_MAX_TASK_NAME_LEN
#define VA_MAX_TASK_NAME_LEN  16      /* Includes the NUL: 15 usable characters */
#endif
#ifndef VA_MAX_LOG_STRING_LEN
#define VA_MAX_LOG_STRING_LEN 100     /* Max bytes per VA_LogString() message (protocol max 1024) */
#endif

/* ── Timestamp source ──────────────────────────────────────────────── */

/* Where event timestamps come from.
 *
 * DWT_CYCCNT (default): the CPU cycle counter (ARMv7-M / ARMv8-M
 * mainline). An architecture without one (Cortex-M0/M0+/M23) is a
 * compile error; a vendor-omitted DWT on a capable core is caught at
 * VA_Init, which probes the counter and refuses to start (ERR:TS_DEAD).
 *
 * CUSTOM_TIMER: a free-running hardware timer read by a function handed
 * to VA_Init(), whose signature changes with this knob:
 *
 *     VA_Init(cpu_freq)                     with DWT_CYCCNT
 *     VA_Init(cpu_freq, ts_fn, tick_hz)     with CUSTOM_TIMER
 *
 * The function returns the raw counter value; the recorder does the
 * 64-bit extension and reports tick_hz to the host, so any tick rate
 * works (timestamp resolution = one tick). The source must:
 *   - be monotonic modulo 2^VA_TIMER_BITS and never pause while the CPU
 *     runs (beware timers that stop in sleep modes you trace through);
 *   - be callable from ANY context, including with interrupts masked;
 *   - take no locks, block on nothing, and be cheap - it runs inside
 *     every trace hook.
 * VA_Init() probes this source too (ERR:TS_NULL / ERR:TS_HZ_ZERO /
 * ERR:TS_DEAD). */

#define DWT_CYCCNT   1u               /* DWT cycle counter (mainline ARMv7-M/ARMv8-M) */
#define CUSTOM_TIMER 2u               /* your free-running timer, via VA_Init() */

#ifndef VA_TIMESTAMP_SOURCE
#define VA_TIMESTAMP_SOURCE DWT_CYCCNT
#endif

/* Width of the CUSTOM_TIMER counter: 32 or 16. With a 16-bit source the
   recorder must observe at least one tick read per 2^16-tick wrap or time
   is silently lost. Every emitted event reads the clock, but across quiet
   gaps VA_TickOverflowCheck() must be called more often than
   65536 / tick_hz seconds - 65 ms at 1 MHz! Prefer a 32-bit timer, or a
   prescaled/slow 16-bit one (32.768 kHz wraps every 2 s). Must stay 32
   for DWT_CYCCNT. */
#ifndef VA_TIMER_BITS
#define VA_TIMER_BITS 32
#endif

/* ── Timing / wire format ──────────────────────────────────────────── */

/* 1 (default): the recorder guards shared state with short PRIMASK-masked
 * critical sections.
 * 0: for systems that forbid the recorder from masking interrupts. HARD
 * CONTRACT: every trace call must come from a single execution context
 * (no tracing from ISRs, no preemption between tracing contexts) -
 * concurrent trace calls corrupt the stream silently. Advertised to hosts
 * via build-flags bit VA_BUILD_BIT_NO_CS. */
#ifndef VA_ALLOWED_TO_DISABLE_INTERRUPTS
#define VA_ALLOWED_TO_DISABLE_INTERRUPTS 1
#endif

/* Stack usage is sampled and emitted on task switch-out, at most once per
   this many ms per task. The high-water scan runs with interrupts masked,
   so this interval also bounds its cost per context switch. 0 = sample on
   every switch-out (much higher overhead and bandwidth). */
#ifndef VA_STACK_USAGE_HEARTBEAT_MS
#define VA_STACK_USAGE_HEARTBEAT_MS 500
#endif

#ifndef VA_AUTO_SETUP_INTERVAL_MS
#define VA_AUTO_SETUP_INTERVAL_MS 2000 /* Re-emit sync + setup packets at this interval (ms). 0 = off. */
#endif

/* Width of the timestamp field on the wire (NOT the tick-source width -
   that is VA_TIMER_BITS above). Always 32: the low 32 bits of the extended
   tick count wrap periodically (2^32 / tick rate: ~25 s at a 170 MHz
   CYCCNT; slower custom timers only widen it) and the host reconstructs
   the full 64-bit value from packet ordering.
   VA_AUTO_SETUP_INTERVAL_MS must stay well below the wrap period so the host
   never misses a wrap (a sync/bundle guarantees at least one packet per
   interval); 2000 ms gives a >12x margin at 170 MHz. Not a knob: any other
   value is a compile error. */
#ifndef VA_TIMESTAMP_BITS
#define VA_TIMESTAMP_BITS 32
#endif

/* ── End of user configuration ─────────────────────────────────────── */

/* Validation and recorder-internal derived flags. Nothing below is a knob. */
#include "VA_ConfigDerived.h"

#endif /* VIEWALYZER_CONFIG_H */
