/**
 * @file VA_ConfigDerived.h
 * @brief Validation and derived flags computed from the ViewAlyzerConfig.h knobs.
 *
 * NOTHING in this file is a user knob. It is included at the end of
 * ViewAlyzerConfig.h, after the project override header and all defaults have
 * been applied, so every translation unit that sees the configuration also
 * sees the same validation and the same derived values. Do not include it
 * directly and do not override anything in it.
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

#ifndef VA_CONFIG_DERIVED_H
#define VA_CONFIG_DERIVED_H

#ifndef VIEWALYZER_CONFIG_H
#error "VA_ConfigDerived.h is internal - include ViewAlyzerConfig.h instead"
#endif

/* Convenience flag - true when any RTOS adapter is active. */
#define VA_HAS_RTOS (VA_RTOS_SELECT != VA_RTOS_NONE)

/* ── Validation ─────────────────────────────────────────────────────
   A typo in the VA_TRACE_* namespace would otherwise fail silently as
   "category off", which looks exactly like "that never happened". */
#define VA_CHECK_BOOL(sym)                                          \
    ((sym) == 0 || (sym) == 1)

#if !VA_CHECK_BOOL(VA_TRACE_DEFAULT)
#error "ViewAlyzer: VA_TRACE_DEFAULT must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_TASKS)
#error "ViewAlyzer: VA_TRACE_TASKS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_TASK_NOTIFICATIONS)
#error "ViewAlyzer: VA_TRACE_TASK_NOTIFICATIONS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_STACK_USAGE)
#error "ViewAlyzer: VA_TRACE_STACK_USAGE must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_ISRS)
#error "ViewAlyzer: VA_TRACE_ISRS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_MUTEXES)
#error "ViewAlyzer: VA_TRACE_MUTEXES must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_MUTEX_CONTENTION)
#error "ViewAlyzer: VA_TRACE_MUTEX_CONTENTION must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_SEMAPHORES)
#error "ViewAlyzer: VA_TRACE_SEMAPHORES must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_QUEUES)
#error "ViewAlyzer: VA_TRACE_QUEUES must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_EVENT_FLAGS)
#error "ViewAlyzer: VA_TRACE_EVENT_FLAGS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_WORK)
#error "ViewAlyzer: VA_TRACE_WORK must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_SLEEP)
#error "ViewAlyzer: VA_TRACE_SLEEP must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_TIMERS)
#error "ViewAlyzer: VA_TRACE_TIMERS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_RTOS_HEAPS)
#error "ViewAlyzer: VA_TRACE_RTOS_HEAPS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_PM)
#error "ViewAlyzer: VA_TRACE_PM must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_USER_VALUES)
#error "ViewAlyzer: VA_TRACE_USER_VALUES must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_USER_EVENTS)
#error "ViewAlyzer: VA_TRACE_USER_EVENTS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_STRINGS)
#error "ViewAlyzer: VA_TRACE_STRINGS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_GPIO)
#error "ViewAlyzer: VA_TRACE_GPIO must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_COUNTERS)
#error "ViewAlyzer: VA_TRACE_COUNTERS must be 0 or 1"
#endif
#if !VA_CHECK_BOOL(VA_TRACE_HEAP_METRICS)
#error "ViewAlyzer: VA_TRACE_HEAP_METRICS must be 0 or 1"
#endif

#if VA_TIMESTAMP_BITS != 32
#error "ViewAlyzer: VA_TIMESTAMP_BITS must be 32 (the wire format has exactly one timestamp width)"
#endif
#define VA_TIMESTAMP_BYTES (VA_TIMESTAMP_BITS / 8)

/* The per-packet sequence byte is part of the wire format, not a knob: it is
   the host's ground truth for loss detection (any gap in the rolling 8-bit
   sequence is exactly that many packets lost, which timestamps alone can
   never prove). Costs 1 byte per packet. */
#if defined(VA_SEQ_COUNTER) && (VA_SEQ_COUNTER != 1)
#error "ViewAlyzer: VA_SEQ_COUNTER is not configurable; sequence bytes are part of the wire format"
#endif
#ifndef VA_SEQ_COUNTER
#define VA_SEQ_COUNTER 1
#endif
#define VA_SEQ_BYTES 1

/* ── Derived transport flags ────────────────────────────────────────── */
#define VA_TRANSPORT_IS_ITM      ((VA_TRANSPORT) == ARM_ITM)
#define VA_TRANSPORT_IS_JLINK    ((VA_TRANSPORT) == JLINK_RTT)
#define VA_TRANSPORT_IS_CUSTOM   ((VA_TRANSPORT) == CUSTOM_TRANSPORT)
#define VA_TRANSPORT_IS_RAMBUF   ((VA_TRANSPORT) == RAM_BUFFER)

/* ── Derived timestamp-source flags ─────────────────────────────────── */
#define VA_TS_IS_DWT    ((VA_TIMESTAMP_SOURCE) == DWT_CYCCNT)
#define VA_TS_IS_CUSTOM ((VA_TIMESTAMP_SOURCE) == CUSTOM_TIMER)

#if !VA_TS_IS_DWT && !VA_TS_IS_CUSTOM
#error "ViewAlyzer: VA_TIMESTAMP_SOURCE must be DWT_CYCCNT or CUSTOM_TIMER"
#endif

#if VA_TS_IS_DWT && (VA_TIMER_BITS != 32)
#error "ViewAlyzer: VA_TIMER_BITS applies to CUSTOM_TIMER only - the DWT cycle counter is always 32-bit"
#endif
#if VA_TS_IS_CUSTOM && (VA_TIMER_BITS != 16) && (VA_TIMER_BITS != 32)
#error "ViewAlyzer: VA_TIMER_BITS must be 16 or 32"
#endif

/* Width/mask of the raw tick source (NOT the wire timestamp field - that
   is VA_TIMESTAMP_BITS). The 64-bit software extension shifts by this. */
#if VA_TS_IS_CUSTOM
#define VA_TS_SOURCE_BITS VA_TIMER_BITS
#else
#define VA_TS_SOURCE_BITS 32
#endif
#if VA_TS_SOURCE_BITS == 16
#define VA_TS_SOURCE_MASK 0xFFFFu
#else
#define VA_TS_SOURCE_MASK 0xFFFFFFFFu
#endif

/* ── Architecture capability (feeds the cross-checks below only) ─────
   Whether this ARCHITECTURE can have the optional CoreSight hardware.
   These do not gate code - the selects do. ARMv8-M baseline (Cortex-M23)
   reports __ARM_ARCH == 8 but has neither ITM nor a DWT cycle counter,
   same as ARMv6-M. */
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) \
 || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define VA_ARCH_HAS_CYCCNT 1
#define VA_ARCH_HAS_ITM    1
#else
#define VA_ARCH_HAS_CYCCNT 0
#define VA_ARCH_HAS_ITM    0
#endif

/* Guarded by VA_ENABLED so the VA_ENABLED=0 escape hatch compiles on any
   core. */
#if VA_ENABLED && VA_TS_IS_DWT && !VA_ARCH_HAS_CYCCNT
#error "ViewAlyzer: this core has no DWT cycle counter (Cortex-M0/M0+/M23). Set VA_TIMESTAMP_SOURCE=CUSTOM_TIMER (Zephyr: CONFIG_VIEWALYZER_TS_CUSTOM_TIMER=y) and pass a free-running timer to VA_Init() - see va_config_template.h."
#endif
#if VA_ENABLED && VA_TRANSPORT_IS_ITM && !VA_ARCH_HAS_ITM
#error "ViewAlyzer: this core has no ITM (Cortex-M0/M0+/M23). Use VA_TRANSPORT=RAM_BUFFER (any probe, including on-board ST-LINK) or JLINK_RTT."
#endif

/* ── Derived snapshot (post-mortem) ring flags ──────────────────────
   The post-mortem ring exists in exactly one of two shapes:
   - the RAM_BUFFER transport ring itself in WRAP mode (untethered builds), or
   - the VA_SNAPSHOT tee ring alongside any live transport.  */
#define VA_PM_VIA_TRANSPORT (VA_TRANSPORT_IS_RAMBUF && ((VA_RAMBUF_MODE) == VA_RAMBUF_MODE_WRAP))
#define VA_PM_RING          (VA_PM_VIA_TRANSPORT || VA_SNAPSHOT)

#if VA_PM_VIA_TRANSPORT && VA_SNAPSHOT
#error "ViewAlyzer: VA_SNAPSHOT with VA_RAMBUF_MODE_WRAP would create two identical snapshot rings - use one or the other"
#endif

#if VA_PM_RING && (VA_MAX_LOG_STRING_LEN > 242)
/* Snapshot ring frames carry a 1-byte length prefix, so a packet must fit in
   255 bytes: 12 bytes of header + seq + VA_MAX_LOG_STRING_LEN payload. */
#error "ViewAlyzer: snapshot ring requires VA_MAX_LOG_STRING_LEN <= 242 (frame length is one byte)"
#endif

#if VA_PM_RING && (VA_SNAPSHOT_SETUP_SIZE > 0) && (VA_SNAPSHOT_SETUP_SIZE < 64)
/* The names area must at least hold the sync marker plus one setup packet. */
#error "ViewAlyzer: VA_SNAPSHOT_SETUP_SIZE must be 0 (disabled) or at least 64 bytes"
#endif

#if VA_TRANSPORT_BUFFERED && (((VA_BUFFER_SIZE) & ((VA_BUFFER_SIZE) - 1)) != 0)
/* The buffered ring uses free-running indices; a non-power-of-two capacity
   corrupts the ring when the 32-bit indices wrap. */
#error "ViewAlyzer: VA_BUFFER_SIZE must be a power of two"
#endif

#if VA_TRANSPORT_BUFFERED && VA_TRANSPORT_IS_RAMBUF
/* VA_Drain flushes in packet-agnostic chunks; the RAM-buffer sink drops on
   whole-write granularity, fragmenting packets on a full ring (and
   chunk-framing the WRAP-mode snapshot). Buffered mode needs a
   self-framing or lossless sink. */
#error "ViewAlyzer: VA_TRANSPORT_BUFFERED cannot be combined with the RAM_BUFFER transport"
#endif

#if VA_TRANSPORT_IS_CUSTOM && ((VA_MAX_LOG_STRING_LEN) + 6 < (VA_MAX_TASK_NAME_LEN))
/* The custom-transport COBS scratch buffer is sized from
   VA_MAX_PACKET_SIZE (driven by VA_MAX_LOG_STRING_LEN), but the largest
   setup packet scales with VA_MAX_TASK_NAME_LEN. */
#error "ViewAlyzer: with CUSTOM_TRANSPORT, VA_MAX_LOG_STRING_LEN must be at least VA_MAX_TASK_NAME_LEN - 6"
#endif

#if VA_TRANSPORT_IS_ITM && ((VA_ITM_PORT) >= 32)
/* ITM has 32 stimulus ports; (1UL << VA_ITM_PORT) and ITM->PORT[VA_ITM_PORT]
   are undefined beyond them. */
#error "ViewAlyzer: VA_ITM_PORT must be 0..31"
#endif

/* ── Derived category flags ─────────────────────────────────────
   A category is not simply on or off: some data has to be TRACKED so an
   enabled category can reference it, without emitting events of its own.
   VA_TRACE_MUTEX_CONTENTION=1 with VA_TRACE_MUTEXES=0 is the case that
   forces this - the contention packet carries the mutex's object id and both
   task ids, so the object and the tasks must still be registered even though
   no MUTEX events are ever emitted. */

/* Task ids and names are needed by anything that names a task. */
#define VA_NEEDS_TASK_REGISTRY (VA_HAS_RTOS && (VA_TRACE_TASKS               \
                                             || VA_TRACE_TASK_NOTIFICATIONS  \
                                             || VA_TRACE_STACK_USAGE         \
                                             || VA_TRACE_MUTEX_CONTENTION    \
                                             || VA_TRACE_SLEEP))

/* Only VA_TRACE_TASKS puts switch events on the wire ... */
#define VA_NEEDS_TASK_SWITCH_EVENTS (VA_HAS_RTOS && VA_TRACE_TASKS)

/* ... but stack usage is sampled on switch-OUT, so the switch hook itself
   must still be installed when only stack usage is on. FreeRTOS sleep also
   needs it: a delayed task's sleep EXIT is detected at its next switch-in. */
#define VA_NEEDS_SWITCH_HOOK (VA_HAS_RTOS && (VA_TRACE_TASKS || VA_TRACE_STACK_USAGE \
                              || ((VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_SLEEP)))

/* Sync-object ids and names. */
#define VA_NEEDS_OBJECT_REGISTRY (VA_HAS_RTOS && (VA_TRACE_MUTEXES           \
                                               || VA_TRACE_MUTEX_CONTENTION  \
                                               || VA_TRACE_SEMAPHORES        \
                                               || VA_TRACE_QUEUES            \
                                               || VA_TRACE_EVENT_FLAGS       \
                                               || VA_TRACE_TIMERS            \
                                               || VA_TRACE_RTOS_HEAPS        \
                                               || VA_TRACE_PM))

/* FreeRTOS routes ALL non-recursive mutex and semaphore traffic through the
   shared traceQUEUE_SEND / traceQUEUE_RECEIVE hooks (traceGIVE_MUTEX and
   traceTAKE_MUTEX are defined by no FreeRTOS version and never invoked), so
   these four categories share one pair of hooks and are filtered at run time
   by object type. When all four are off the hooks are not installed at all
   and the cost is genuinely zero. */
#define VA_NEEDS_SYNC_HOOKS (VA_HAS_RTOS && (VA_TRACE_MUTEXES          \
                                          || VA_TRACE_MUTEX_CONTENTION \
                                          || VA_TRACE_SEMAPHORES       \
                                          || VA_TRACE_QUEUES))

/* traceBLOCKING_ON_QUEUE_RECEIVE exists solely to sample the mutex holder. */
#define VA_NEEDS_BLOCKING_HOOK (VA_HAS_RTOS && VA_TRACE_MUTEX_CONTENTION)

/* True when the adapter can classify a sync object from its native handle
   alone - cheaply, and without touching the recorder's registry. FreeRTOS
   can (a null check on pcHead plus one ucQueueType load); Zephyr cannot,
   because k_mutex / k_sem / k_msgq are distinct types and its trace points
   are already separate per category. */
#define VA_ADAPTER_CLASSIFIES_OBJECTS (VA_RTOS_SELECT == VA_RTOS_FREERTOS)

/* Reject disabled categories at the very TOP of the shared queue hooks,
   before the pending-bundle service and the PRIMASK save/disable that are
   the expensive part of those hooks. Only worth compiling in when the
   adapter can classify and at least one shared category is off. (Timers,
   heaps, and PM never reach these hooks on FreeRTOS, so they are not part
   of the test.) */
#define VA_NEEDS_SYNC_PREFILTER (VA_NEEDS_SYNC_HOOKS                    \
                                 && VA_ADAPTER_CLASSIFIES_OBJECTS       \
                                 && !(VA_TRACE_MUTEXES                  \
                                      && VA_TRACE_SEMAPHORES            \
                                      && VA_TRACE_QUEUES))

/* The user-trace registry stores ISR name registrations too (they go through
   VA_RegisterUserTrace with VA_USER_TYPE_ISR). */
#define VA_NEEDS_USER_TRACE_REGISTRY (VA_TRACE_USER_VALUES || VA_TRACE_ISRS)

/* ── Enabled-category bitmask (reported to the host) ─────────────
   Sent in VA_SETUP_CONFIG_FLAGS group VA_FLAG_GROUP_CATEGORIES with every
   setup bundle, so a late-attaching viewer can tell "disabled in this
   firmware build" apart from "never happened". Bit positions are a host
   protocol contract - append only, never renumber. */
#define VA_CAT_BIT_TASKS              0u
#define VA_CAT_BIT_TASK_NOTIFICATIONS 1u
#define VA_CAT_BIT_STACK_USAGE        2u
#define VA_CAT_BIT_ISRS               3u
#define VA_CAT_BIT_MUTEXES            4u
#define VA_CAT_BIT_MUTEX_CONTENTION   5u
#define VA_CAT_BIT_SEMAPHORES         6u
#define VA_CAT_BIT_QUEUES             7u
#define VA_CAT_BIT_SLEEP              8u
#define VA_CAT_BIT_TIMERS             9u
#define VA_CAT_BIT_RTOS_HEAPS         10u
#define VA_CAT_BIT_PM                 11u
#define VA_CAT_BIT_USER_VALUES        12u
#define VA_CAT_BIT_USER_EVENTS        13u
#define VA_CAT_BIT_STRINGS            14u
#define VA_CAT_BIT_GPIO               15u
#define VA_CAT_BIT_COUNTERS           16u
#define VA_CAT_BIT_HEAP_METRICS       17u
#define VA_CAT_BIT_EVENT_FLAGS        18u
#define VA_CAT_BIT_WORK               19u

/* The mask reports what this build can actually EMIT, not the raw knobs:
   RTOS categories are off on bare metal, task notifications exist only on
   FreeRTOS, k_work only on Zephyr. */
#define VA_CAT_RTOS_(sym)     (VA_HAS_RTOS && (sym) != 0)
#define VA_CAT_FREERTOS_(sym) ((VA_RTOS_SELECT == VA_RTOS_FREERTOS) && (sym) != 0)
#define VA_CAT_ZEPHYR_(sym)   ((VA_RTOS_SELECT == VA_RTOS_ZEPHYR) && (sym) != 0)

#define VA_TRACE_CATEGORY_MASK                                                   \
    (((uint32_t)VA_CAT_RTOS_(VA_TRACE_TASKS)              << VA_CAT_BIT_TASKS)              \
   | ((uint32_t)VA_CAT_FREERTOS_(VA_TRACE_TASK_NOTIFICATIONS) << VA_CAT_BIT_TASK_NOTIFICATIONS) \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_STACK_USAGE)        << VA_CAT_BIT_STACK_USAGE)        \
   | ((uint32_t)(VA_TRACE_ISRS               != 0) << VA_CAT_BIT_ISRS)               \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_MUTEXES)            << VA_CAT_BIT_MUTEXES)            \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_MUTEX_CONTENTION)   << VA_CAT_BIT_MUTEX_CONTENTION)   \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_SEMAPHORES)         << VA_CAT_BIT_SEMAPHORES)         \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_QUEUES)             << VA_CAT_BIT_QUEUES)             \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_SLEEP)              << VA_CAT_BIT_SLEEP)              \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_TIMERS)             << VA_CAT_BIT_TIMERS)             \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_RTOS_HEAPS)         << VA_CAT_BIT_RTOS_HEAPS)         \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_PM)                 << VA_CAT_BIT_PM)                 \
   | ((uint32_t)(VA_TRACE_USER_VALUES        != 0) << VA_CAT_BIT_USER_VALUES)        \
   | ((uint32_t)(VA_TRACE_USER_EVENTS        != 0) << VA_CAT_BIT_USER_EVENTS)        \
   | ((uint32_t)(VA_TRACE_STRINGS            != 0) << VA_CAT_BIT_STRINGS)            \
   | ((uint32_t)(VA_TRACE_GPIO               != 0) << VA_CAT_BIT_GPIO)               \
   | ((uint32_t)(VA_TRACE_COUNTERS           != 0) << VA_CAT_BIT_COUNTERS)           \
   | ((uint32_t)(VA_TRACE_HEAP_METRICS       != 0) << VA_CAT_BIT_HEAP_METRICS)       \
   | ((uint32_t)VA_CAT_RTOS_(VA_TRACE_EVENT_FLAGS)        << VA_CAT_BIT_EVENT_FLAGS)     \
   | ((uint32_t)VA_CAT_ZEPHYR_(VA_TRACE_WORK)             << VA_CAT_BIT_WORK))

/* Flag groups carried by VA_SETUP_CONFIG_FLAGS (0x77). The packet is
   [code][seq][group(1)][value(4, little-endian)]. Hosts must skip groups
   they do not know; group ids are append-only. */
#define VA_FLAG_GROUP_CATEGORIES 0x01u   /* value = VA_TRACE_CATEGORY_MASK */
#define VA_FLAG_GROUP_BUILD      0x02u   /* value = VA_BUILD_FLAGS below   */
#define VA_FLAG_GROUP_VERSION    0x03u   /* value = VA_RECORDER_VERSION_PACKED */

#define VA_BUILD_BIT_NO_RTOS     0u      /* bare-metal build */
#define VA_BUILD_BIT_BUFFERED    1u      /* VA_TRANSPORT_BUFFERED */
#define VA_BUILD_BIT_SEQ_COUNTER 2u      /* sequence bytes present (always set
                                            by firmware; kept so hosts can
                                            trust the bit, not assume it) */
/* Timestamps were stamped by the HOST, not by a target cycle counter. Only
   host-side senders (the python viewalyzer package, test harnesses) ever set
   this; the firmware cannot, which is why it has no VA_TRACE_* switch. */
#define VA_BUILD_BIT_HOST_TS     3u
/* Timestamps come from a user-supplied timer (VA_TIMESTAMP_SOURCE =
   CUSTOM_TIMER), not the DWT cycle counter. The CLK: rate is that timer's
   tick rate, which need not equal the CPU clock - hosts that want an HCLK
   (ETM correlation) must not assume CLK: is one when this bit is set. */
#define VA_BUILD_BIT_TIMER_TS    4u
/* Built with VA_ALLOWED_TO_DISABLE_INTERRUPTS=0 (single-execution-context
   contract): lets a host label such captures. */
#define VA_BUILD_BIT_NO_CS       5u

#define VA_BUILD_FLAGS                                                        \
    (((uint32_t)(VA_HAS_RTOS == 0)          << VA_BUILD_BIT_NO_RTOS)          \
   | ((uint32_t)(VA_TRANSPORT_BUFFERED != 0) << VA_BUILD_BIT_BUFFERED)        \
   | ((uint32_t)(VA_SEQ_COUNTER != 0)        << VA_BUILD_BIT_SEQ_COUNTER)     \
   | ((uint32_t)(VA_TS_IS_CUSTOM != 0)       << VA_BUILD_BIT_TIMER_TS)        \
   | ((uint32_t)(VA_ALLOWED_TO_DISABLE_INTERRUPTS == 0) << VA_BUILD_BIT_NO_CS))

#endif /* VA_CONFIG_DERIVED_H */
