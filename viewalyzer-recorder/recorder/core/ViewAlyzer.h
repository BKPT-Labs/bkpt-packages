/**
 * @file ViewAlyzer.h
 * @brief ViewAlyzer Recorder Firmware - Public API
 *
 * Configuration lives in ViewAlyzerConfig.h - this header is the API surface
 * only. It includes no device, CMSIS, or RTOS headers, so it is safe to
 * include from anywhere, including FreeRTOSConfig.h.
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

#ifndef VIEWALYZER_H
#define VIEWALYZER_H

/* Recorder version. Semver, bumped only at tagged releases: patch = fixes,
   minor = additive (new event codes, category bits, flag groups), major =
   breaking wire/API change. The three numeric parts are the source of truth;
   the string and packed forms derive from them. */
#define VA_RECORDER_VERSION_MAJOR 1
#define VA_RECORDER_VERSION_MINOR 1
#define VA_RECORDER_VERSION_PATCH 0

#define VA_VERSION_STR2_(x) #x
#define VA_VERSION_STR_(x)  VA_VERSION_STR2_(x)
#define VA_RECORDER_VERSION                    \
    VA_VERSION_STR_(VA_RECORDER_VERSION_MAJOR) \
    "." VA_VERSION_STR_(VA_RECORDER_VERSION_MINOR) \
    "." VA_VERSION_STR_(VA_RECORDER_VERSION_PATCH)

/* On-wire form (VA_SETUP_CONFIG_FLAGS group VA_FLAG_GROUP_VERSION). */
#define VA_RECORDER_VERSION_PACKED                     \
    (((uint32_t)VA_RECORDER_VERSION_MAJOR << 16)       \
   | ((uint32_t)VA_RECORDER_VERSION_MINOR << 8)        \
   | ((uint32_t)VA_RECORDER_VERSION_PATCH))

/* Wire protocol id, decoupled from the recorder version above. Encoded in
   the sync marker (byte 3 binary, bytes 8-9 ASCII) and the ring control
   blocks. Format 1 = 32-bit timestamps + per-packet sequence bytes. Bumps
   only on a breaking framing change; additive event/setup codes do NOT bump
   it (hosts gate on the category mask and packet presence instead). */
#define VA_WIRE_VERSION 1

#include "ViewAlyzerConfig.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Maximum raw packet size (before COBS encoding).
   Largest packet is VA_LogString: 12-byte header + message payload. */
#define VA_MAX_PACKET_SIZE (12 + VA_SEQ_BYTES + VA_MAX_LOG_STRING_LEN)

/* User-provided send function signature for custom transport */
typedef void (*VA_TransportSendFn)(const uint8_t *data, uint32_t length);

/* User-provided tick source for CUSTOM_TIMER timestamps: returns the
   current value of a free-running counter (low VA_TIMER_BITS bits used).
   Full contract at VA_TIMESTAMP_SOURCE in ViewAlyzerConfig.h. */
typedef uint32_t (*VA_TimestampFn)(void);

/* Discard the arguments of a compiled-out API without evaluating them
   (no side effects, no -Wunused warnings). */
#define VA_DISCARD_1(a)       ((void)sizeof(a))
#define VA_DISCARD_2(a, b)    (VA_DISCARD_1(a), VA_DISCARD_1(b))
#define VA_DISCARD_3(a, b, c) (VA_DISCARD_2(a, b), VA_DISCARD_1(c))
#define VA_DISCARD_PICK(_1, _2, _3, NAME, ...) NAME
#define VA_DISCARD_ARGS(...) \
    VA_DISCARD_PICK(__VA_ARGS__, VA_DISCARD_3, VA_DISCARD_2, VA_DISCARD_1)(__VA_ARGS__)

/* Throughput-test counters (compile-gated with -DVA_TP_TEST=1, off by
   default). Little-endian layout is a host contract - do not reorder. */
#if defined(VA_TP_TEST) && (VA_TP_TEST == 1)
    typedef struct
    {
        char              magic[8];          /* "VATPCNT1", written last        */
        volatile uint32_t offeredPackets;    /* packets handed to the transport */
        volatile uint32_t offeredBytes;      /* their total protocol bytes      */
        volatile uint32_t droppedPackets;    /* packets the transport dropped   */
        volatile uint32_t droppedBytes;      /* their total protocol bytes      */
    } VA_TpCounters_t;
    extern VA_TpCounters_t _VA_TP;
#endif

/* --- Binary Event Type Codes --- */
#define VA_EVENT_TYPE_MASK        0x7F
#define VA_EVENT_FLAG_START_END   0x80
#define VA_EVENT_TASK_SWITCH      0x01
#define VA_EVENT_ISR              0x02
#define VA_EVENT_TASK_CREATE      0x03
#define VA_EVENT_USER_TRACE       0x04
#define VA_EVENT_TASK_NOTIFY      0x05
#define VA_EVENT_SEMAPHORE        0x06
#define VA_EVENT_MUTEX            0x07
#define VA_EVENT_QUEUE            0x08
#define VA_EVENT_TASK_STACK_USAGE 0x09
#define VA_EVENT_USER_TOGGLE      0x0A
#define VA_EVENT_USER_EVENT       0x0B
#define VA_EVENT_MUTEX_CONTENTION 0x0C
#define VA_EVENT_STRING_EVENT     0x0D
#define VA_EVENT_FLOAT_TRACE      0x0E
#define VA_EVENT_GPIO             0x0F
#define VA_EVENT_COUNTER          0x10
#define VA_EVENT_HEAP             0x11
#define VA_EVENT_SLEEP            0x12
#define VA_EVENT_TIMER            0x13
#define VA_EVENT_HEAP_SYNC        0x14
#define VA_EVENT_PM_SUSPEND       0x15
/* Payload = 32-bit absolute sequence number of this packet. */
#define VA_EVENT_SEQ_CHECKPOINT   0x16
/* id = heap object, value = requested bytes of the failed alloc. */
#define VA_EVENT_HEAP_FAIL        0x17
/* Event-flag group (FreeRTOS event group / Zephyr k_event).
   START flag = bits set/posted (value = the bits); no flag = a wait was
   satisfied (value = the bits waited for / matched). */
#define VA_EVENT_EVENTFLAG        0x18
/* An operation on a sync object FAILED (timeout / no space / no data).
   id = object id, START flag = give/send/set side, no flag = take/receive/
   wait side; value = operation detail (waited-for bits for event flags,
   0 otherwise). Distinct from VA_EVENT_HEAP_FAIL. */
#define VA_EVENT_OP_FAILED        0x19
/* Deferred work (Zephyr k_work family). Payload after the timestamp
   is [handler(4)][delay_ms(4)], both little-endian; the handler address is
   symbolicated by the host from the ELF, and work items consume no object
   ids (the id byte is 0). START flag = armed (delay_ms 0 = submitted to
   run now, >0 = scheduled that far ahead); no flag = canceled. There is no
   execution trace point in the kernel, so the host derives the expected
   fire time from arm + delay and must not fabricate start/end events. */
#define VA_EVENT_WORK             0x1A
/* A kernel timer was armed. Emitted alongside (not instead of) the
   Timer give event; id = the timer's sync-object id, payload after the
   timestamp is [duration_ms(4)][period_ms(4)]. period 0 = one-shot; the
   first expected fire is arm + duration, later ones repeat at period. */
#define VA_EVENT_TIMER_ARM        0x1B


/* --- Setup Message Codes --- */
#define VA_SETUP_TASK_MAP          0x70
#define VA_SETUP_ISR_MAP           0x71
#define VA_SETUP_USER_TRACE        0x72
#define VA_SETUP_SEMAPHORE_MAP     0x73
#define VA_SETUP_MUTEX_MAP         0x74
#define VA_SETUP_QUEUE_MAP         0x75
#define VA_SETUP_USER_EVENT_MAP    0x76
/* Typed flags packet: [0x77][seq?][group(1)][value(4, little-endian)].
   Groups are VA_FLAG_GROUP_* in ViewAlyzerConfig.h. Unlike every other setup
   code this one carries no name string. */
#define VA_SETUP_CONFIG_FLAGS      0x77
#define VA_SETUP_GPIO_MAP          0x78
#define VA_SETUP_HEAP_INFO         0x79
#define VA_SETUP_OS_INFO           0x7A
#define VA_SETUP_TIMER_MAP         0x7B
#define VA_SETUP_HEAP_MAP          0x7C
#define VA_SETUP_PM_MAP            0x7D
/* Typed per-object info: [0x7E][seq?][id(1)][kind(1)][value(4, LE)].
   id is in the SYNC-OBJECT id space (unlike VA_SETUP_HEAP_INFO, whose id
   space is the manual heap gauges). Fixed length, no name string. */
#define VA_SETUP_OBJECT_INFO       0x7E
#define VA_SETUP_INFO              0x7F

/* VA_SETUP_OBJECT_INFO kinds. */
#define VA_OBJINFO_HEAP_CAPACITY   0x01   /* value = heap capacity in bytes */
#define VA_OBJINFO_OBJECT_ADDR     0x02   /* value = the object's native handle
                                             address, so hosts can name
                                             statically-defined objects from
                                             the ELF's data symbols */

    typedef enum
    {
        VA_USER_TYPE_GRAPH     = 0,
        VA_USER_TYPE_BAR       = 1,
        VA_USER_TYPE_GAUGE     = 2,
        VA_USER_TYPE_COUNTER   = 3,
        VA_USER_TYPE_TABLE     = 4,
        VA_USER_TYPE_HISTOGRAM = 5,
        VA_USER_TYPE_TOGGLE    = 6,
        VA_USER_TYPE_TASK      = 7,
        VA_USER_TYPE_ISR       = 8
    } VA_UserTraceType_t;

    typedef enum
    {
        TOGGLE_LOW,
        TOGGLE_HIGH
    } VA_UserToggleState_t;

    typedef enum
    {
        USER_EVENT_START,
        USER_EVENT_END
    } VA_UserEventState_t;

    typedef enum
    {
        VA_OBJECT_TYPE_QUEUE           = 0,
        VA_OBJECT_TYPE_MUTEX           = 1,
        VA_OBJECT_TYPE_COUNTING_SEM    = 2,
        VA_OBJECT_TYPE_BINARY_SEM      = 3,
        VA_OBJECT_TYPE_RECURSIVE_MUTEX = 4,
        VA_OBJECT_TYPE_TIMER           = 5,
        VA_OBJECT_TYPE_HEAP            = 6,
        VA_OBJECT_TYPE_POWER_MGMT      = 7,
        VA_OBJECT_TYPE_EVENTFLAG       = 8
    } VA_QueueObjectType_t;

/* --- Static ISR IDs --- */
#define VA_ISR_ID_SYSTICK 1
#define VA_ISR_ID_PENDSV 2
/* ... Add more ISR IDs ... */

/* ── Public API ──────────────────────────────────────────────────── */
/* Session infrastructure (init, drain, tick, transport registration) is
   always present. Everything else follows its VA_TRACE_* category: when a
   category is off, its functions become argument-discarding no-op macros,
   so calls in your code keep compiling and cost nothing. */
#if (VA_ENABLED == 1)

#if VA_TRANSPORT_IS_CUSTOM
    void VA_RegisterTransportSend(VA_TransportSendFn sendFn);
#endif
/* VA_Init resets every registry: call it BEFORE any VA_Register* call.
   User traces/events/GPIOs/heap gauges registered earlier are wiped and
   never re-registered. VA_Init also probes the timestamp source for
   movement and refuses to start (reporting ERR:TS_* to the host) when it
   is not counting - including a vendor-omitted DWT. */
#if VA_TS_IS_CUSTOM
    /* CUSTOM_TIMER build: the tick source rides in VA_Init itself, so a
       missing source is a compile error. The timer must be RUNNING before
       the call. Tick math and the CLK: rate use tick_hz. */
    void VA_Init(uint32_t cpu_freq, VA_TimestampFn ts_fn, uint32_t tick_hz);
#else
    void VA_Init(uint32_t cpu_freq);
#endif
    void VA_EmitSetupBundle(void);    /* re-emit sync marker + all setup packets (call periodically, e.g. every 2-5 s) */
    void VA_TickOverflowCheck(void);  /* call more often than the tick counter wraps: DWT every 1-10 s; 16-bit timers wrap in ms (see VA_TIMER_BITS) */
    void VA_Drain(void);              /* buffered mode only: flush the RAM ring to the wire. Call from idle/main loop. No-op when VA_TRANSPORT_BUFFERED == 0. */
    bool VA_IsInit(void);

#if VA_PM_RING
    /** Stop snapshot-ring writes so the current post-mortem window survives
     *  whatever the system does next. Call from your fault or assert handler
     *  (any context is safe); everything emitted before the call is already
     *  in the ring. Only meaningful with VA_RAMBUF_MODE_WRAP or VA_SNAPSHOT;
     *  a no-op macro otherwise, so the call always compiles. */
    void VA_SnapshotFreeze(void);
#else
#define VA_SnapshotFreeze() ((void)0)
#endif

/* ── ISR tracing ─────────────────────────────────────────────── */
#if VA_TRACE_ISRS
    void VA_LogISRStart(uint8_t isrId);
    void VA_LogISREnd(uint8_t isrId);
#else
#define VA_LogISRStart(isrId) VA_DISCARD_ARGS(isrId)
#define VA_LogISREnd(isrId)   VA_DISCARD_ARGS(isrId)
#endif

/* Compile-time check that a string-literal name fits the fixed
   VA_MAX_TASK_NAME_LEN wire field (pointers pass; truncated at runtime). */
#define VA_ASSERT_NAME_FITS(name) \
    ((void) sizeof (char[(sizeof (name) <= VA_MAX_TASK_NAME_LEN) ? 1 : -1]))

/* ── User values: VA_LogTrace / VA_LogTraceFloat / VA_LogToggle ── */
#if VA_NEEDS_USER_TRACE_REGISTRY
    /* Also the registration path for ISR names (VA_USER_TYPE_ISR). */
    void VA_RegisterUserTrace(uint8_t id, const char *name, VA_UserTraceType_t type);
    /* Parenthesised callee => the macro does not recurse into itself. */
#define VA_RegisterUserTrace(id, name, type) \
    (VA_ASSERT_NAME_FITS (name), (VA_RegisterUserTrace) ((id), (name), (type)))
#else
#define VA_RegisterUserTrace(id, name, type) \
    (VA_ASSERT_NAME_FITS (name), VA_DISCARD_ARGS(id, name, type))
#endif

#if VA_TRACE_USER_VALUES
    void VA_LogTrace(uint8_t id, int32_t value);
    void VA_LogTraceFloat(uint8_t id, float value);
    void VA_LogToggle(uint8_t id, bool state);
#else
#define VA_LogTrace(id, value)      VA_DISCARD_ARGS(id, value)
#define VA_LogTraceFloat(id, value) VA_DISCARD_ARGS(id, value)
#define VA_LogToggle(id, state)     VA_DISCARD_ARGS(id, state)
#endif

/* ── User event spans ────────────────────────────────────────── */
#if VA_TRACE_USER_EVENTS
    void VA_RegisterUserEvent(uint8_t id, const char *name);
    void VA_LogEvent(uint8_t id, bool state);
#define VA_RegisterUserEvent(id, name) \
    (VA_ASSERT_NAME_FITS (name), (VA_RegisterUserEvent) ((id), (name)))
#define VA_EVENT_START(id) VA_LogEvent(id, USER_EVENT_START)
#define VA_EVENT_END(id)   VA_LogEvent(id, USER_EVENT_END)
#else
#define VA_RegisterUserEvent(id, name) \
    (VA_ASSERT_NAME_FITS (name), VA_DISCARD_ARGS(id, name))
#define VA_LogEvent(id, state)         VA_DISCARD_ARGS(id, state)
#define VA_EVENT_START(id)             VA_DISCARD_ARGS(id)
#define VA_EVENT_END(id)               VA_DISCARD_ARGS(id)
#endif

/* ── Strings ─────────────────────────────────────────────────── */
#if VA_TRACE_STRINGS
    void VA_LogString(uint8_t id, const char *msg);
#else
#define VA_LogString(id, msg) VA_DISCARD_ARGS(id, msg)
#endif

/* ── GPIO ────────────────────────────────────────────────────── */
#if VA_TRACE_GPIO
    void VA_RegisterGPIO(uint8_t id, const char *name);
    void VA_LogGPIO(uint8_t id, bool state);
#else
#define VA_RegisterGPIO(id, name) VA_DISCARD_ARGS(id, name)
#define VA_LogGPIO(id, state)     VA_DISCARD_ARGS(id, state)
#endif

/* ── Counters ────────────────────────────────────────────────── */
#if VA_TRACE_COUNTERS
    void VA_LogCounter(uint8_t id, uint32_t value);
#else
#define VA_LogCounter(id, value) VA_DISCARD_ARGS(id, value)
#endif

/* ── Manual heap gauge (bare metal included) ─────────────────── */
#if VA_TRACE_HEAP_METRICS
    void VA_RegisterHeap(uint8_t id, const char *name, uint32_t totalSize);
    void VA_LogHeap(uint8_t id, uint32_t usedBytes);
#else
#define VA_RegisterHeap(id, name, totalSize) VA_DISCARD_ARGS(id, name, totalSize)
#define VA_LogHeap(id, usedBytes)            VA_DISCARD_ARGS(id, usedBytes)
#endif

    /* ── RTOS task/object hooks (generic void* handles) ──────────── */
    /* Called by the RTOS adapter. The handle is opaque to the core - the
       adapter passes the correct RTOS-native pointer. */

/* ── Task registry + switches ────────────────────────────────── */
#if VA_NEEDS_TASK_REGISTRY
    void va_taskcreated(void *taskHandle, const char *name);
    /* Frees the registry slot so a recycled TCB/thread address cannot
       inherit the dead task's identity. The id is never reused. */
    void va_taskdeleted(void *taskHandle);
    /* Updates the stored name and re-emits the task's name map. */
    void va_taskrenamed(void *taskHandle, const char *name);

    /* Task-creation scratch globals, filled by the adapter just before
       va_taskcreated(). Declared here so the kernel-compiled hook header
       sees them without the device header. */
    extern volatile void     *g_task_pxStack;
    extern volatile void     *g_task_pxEndOfStack;
    extern volatile uint32_t  g_task_uxPriority;
    extern volatile uint32_t  g_task_uxBasePriority;
    extern volatile uint32_t  g_task_ulStackDepth;
#else
#define va_taskcreated(h, n) VA_DISCARD_ARGS(h, n)
#define va_taskdeleted(h)    VA_DISCARD_ARGS(h)
#define va_taskrenamed(h, n) VA_DISCARD_ARGS(h, n)
#endif

#if VA_NEEDS_SWITCH_HOOK
    void va_taskswitchedin(void *taskHandle);
    void va_taskswitchedout(void *taskHandle);
#else
#define va_taskswitchedin(h)  VA_DISCARD_ARGS(h)
#define va_taskswitchedout(h) VA_DISCARD_ARGS(h)
#endif

/* ── Task notifications ──────────────────────────────────────── */
#if VA_HAS_RTOS && VA_TRACE_TASK_NOTIFICATIONS
    void va_logtasknotifygive(void *srcHandle, void *destHandle, uint32_t value);
    void va_logtasknotifytake(void *taskHandle, uint32_t value);
#else
#define va_logtasknotifygive(s, d, v) VA_DISCARD_ARGS(s, d, v)
#define va_logtasknotifytake(h, v)    VA_DISCARD_ARGS(h, v)
#endif

/* ── Sleep (Zephyr k_sleep / k_msleep / k_usleep) ────────────── */
#if VA_HAS_RTOS && VA_TRACE_SLEEP
    void va_logSleepEnter(void *taskHandle);
    void va_logSleepExit(void *taskHandle);
#else
#define va_logSleepEnter(h) VA_DISCARD_ARGS(h)
#define va_logSleepExit(h)  VA_DISCARD_ARGS(h)
#endif

/* ── PM (Zephyr pm_system_suspend enter/exit) ────────────────── */
#if VA_HAS_RTOS && VA_TRACE_PM
    void va_logPMSuspendEnter(void);
    void va_logPMSuspendExit(uint8_t state);
#else
#define va_logPMSuspendEnter()      ((void)0)
#define va_logPMSuspendExit(state)  VA_DISCARD_ARGS(state)
#endif

/* ── Sync objects (mutex / semaphore / queue / timer / heap / PM) ──
 *  On FreeRTOS these four categories share one pair of kernel trace hooks,
 *  so the give/take entry points exist whenever ANY of them is on and reject
 *  disabled object types at run time. */
#if VA_NEEDS_OBJECT_REGISTRY
    void va_logQueueObjectCreate(void *queueObject, const char *name);
    void va_logQueueObjectCreateWithType(void *queueObject, const char *typeHint);
    /* Typed entry points for objects the adapter cannot classify from the
       handle alone (FreeRTOS software timers, event groups, heap sentinels -
       anything that is not a Queue_t). The caller states the object type;
       the adapter's handle inspection is never consulted. */
    void va_logQueueObjectCreateTyped(void *queueObject, const char *name, VA_QueueObjectType_t type);
    void va_logQueueObjectGiveTyped(void *queueObject, VA_QueueObjectType_t type);
    void va_logQueueObjectTakeTyped(void *queueObject, VA_QueueObjectType_t type);
    void va_updateQueueObjectType(void *queueObject, const char *typeHint);
    /* Frees the registry slot on object deletion (same rule as tasks: the
       slot is reusable, the id is not). */
    void va_logQueueObjectDelete(void *queueObject);
    /* User-assigned name (FreeRTOS vQueueAddToRegistry); replaces the
       auto-generated name and re-emits the name map. */
    void va_logQueueObjectSetName(void *queueObject, const char *name);
    void va_logQueueObjectGive(void *queueObject, uint32_t timeout);
    void va_logQueueObjectTake(void *queueObject, uint32_t timeout);
    /* A failed give/send (giveSide true) or take/receive (false) on a sync
       object: timeout, no space, or no data. The typed variant is for
       handles the adapter cannot classify (event flags). detail rides the
       value field (waited-for bits for event flags, 0 otherwise). */
    void va_logQueueObjectOpFailed(void *queueObject, bool giveSide, uint32_t detail);
    void va_logObjectOpFailedTyped(void *queueObject, VA_QueueObjectType_t type, bool giveSide, uint32_t detail);
#else
#define va_logQueueObjectCreate(queueObject, name)             VA_DISCARD_ARGS(queueObject, name)
#define va_logQueueObjectCreateWithType(queueObject, typeHint) VA_DISCARD_ARGS(queueObject, typeHint)
#define va_logQueueObjectCreateTyped(queueObject, name, type)  VA_DISCARD_ARGS(queueObject, name, type)
#define va_logQueueObjectGiveTyped(queueObject, type)          VA_DISCARD_ARGS(queueObject, type)
#define va_logQueueObjectTakeTyped(queueObject, type)          VA_DISCARD_ARGS(queueObject, type)
#define va_updateQueueObjectType(queueObject, typeHint)        VA_DISCARD_ARGS(queueObject, typeHint)
#define va_logQueueObjectDelete(queueObject)                   VA_DISCARD_ARGS(queueObject)
#define va_logQueueObjectSetName(queueObject, name)            VA_DISCARD_ARGS(queueObject, name)
#define va_logQueueObjectGive(queueObject, timeout)            VA_DISCARD_ARGS(queueObject, timeout)
#define va_logQueueObjectTake(queueObject, timeout)            VA_DISCARD_ARGS(queueObject, timeout)
#define va_logQueueObjectOpFailed(queueObject, giveSide, detail) VA_DISCARD_ARGS(queueObject, giveSide, detail)
#define va_logObjectOpFailedTyped(queueObject, type, giveSide, detail) \
    (VA_DISCARD_2(queueObject, type), VA_DISCARD_2(giveSide, detail))
#endif

#if VA_NEEDS_BLOCKING_HOOK
    void va_logQueueObjectBlocking(void *queueObject);
#else
#define va_logQueueObjectBlocking(queueObject) VA_DISCARD_ARGS(queueObject)
#endif

/* ── Event flags (FreeRTOS event groups / Zephyr k_event) ────────── */
#if VA_HAS_RTOS && VA_TRACE_EVENT_FLAGS
    void va_logEventFlagSet(void *flagObject, uint32_t bits);
    void va_logEventFlagWaitEnd(void *flagObject, uint32_t bits);
#else
#define va_logEventFlagSet(flagObject, bits)     VA_DISCARD_ARGS(flagObject, bits)
#define va_logEventFlagWaitEnd(flagObject, bits) VA_DISCARD_ARGS(flagObject, bits)
#endif

/* ── Timer arm (duration/period at k_timer_start / xTimerStart) ──── */
#if VA_HAS_RTOS && VA_TRACE_TIMERS
    /** Emitted by the adapters when a timer is armed, alongside the Timer
     *  give event. periodMs 0 = one-shot. */
    void va_logTimerArm(void *timerObject, uint32_t durationMs, uint32_t periodMs);
#else
#define va_logTimerArm(timerObject, durationMs, periodMs) \
    VA_DISCARD_ARGS(timerObject, durationMs, periodMs)
#endif

/* ── Deferred work (Zephyr k_work family) ────────────────────────── */
#if VA_HAS_RTOS && VA_TRACE_WORK
    /** Work armed: delayMs 0 = submitted to run now, >0 = scheduled that
     *  far ahead (rescheduling arms again with the new delay). The handler
     *  address identifies the work item; no object id is consumed. */
    void va_logWorkArm(void *handler, uint32_t delayMs);
    void va_logWorkCancel(void *handler);
#else
#define va_logWorkArm(handler, delayMs) VA_DISCARD_ARGS(handler, delayMs)
#define va_logWorkCancel(handler)       VA_DISCARD_ARGS(handler)
#endif

/* ── Kernel heap allocator tracing ───────────────────────────── */
#if VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS
    void va_logHeapAlloc(void *heapObject, uint32_t allocBytes);
    void va_logHeapFree(void *heapObject, uint32_t allocatedBytes);
    void va_logHeapAllocFailed(void *heapObject, uint32_t requestedBytes);
    void va_logHeapCapacity(void *heapObject, const char *name, uint32_t totalSize);
#else
#define va_logHeapAlloc(heapObject, allocBytes)             VA_DISCARD_ARGS(heapObject, allocBytes)
#define va_logHeapFree(heapObject, allocatedBytes)          VA_DISCARD_ARGS(heapObject, allocatedBytes)
#define va_logHeapAllocFailed(heapObject, requestedBytes)   VA_DISCARD_ARGS(heapObject, requestedBytes)
#define va_logHeapCapacity(heapObject, name, totalSize)     VA_DISCARD_ARGS(heapObject, name, totalSize)
#endif

    /* ── RTOS Adapter interface ──────────────────────────────────
     * Each RTOS adapter implements the ones its enabled categories need.
     */
#if VA_NEEDS_OBJECT_REGISTRY
    /** Determine the sync-object type from a native RTOS handle.
     *  FreeRTOS: inspects pcHead / ucQueueType in the Queue_t mirror.
     *  Zephyr:   uses the caller-provided type hint (always returns QUEUE).
     */
    VA_QueueObjectType_t va_adapter_get_queue_object_type(void *handle);
#endif

#if VA_HAS_RTOS && VA_TRACE_STACK_USAGE
    /** Return stack usage in BYTES for the given task handle.
     *  FreeRTOS: uxTaskGetStackHighWaterMark, converted from words.
     *  Zephyr:   k_thread_stack_space_get (bytes natively).
     */
    uint32_t va_adapter_calculate_stack_usage(void *taskHandle);

    /** Return total stack size in BYTES for the given task handle. */
    uint32_t va_adapter_get_total_stack_size(void *taskHandle);
#endif

#if VA_NEEDS_BLOCKING_HOOK
    /** Detect mutex contention and emit a contention packet if applicable.
     *  Called from va_logQueueObjectBlocking().
     */
    void va_adapter_check_mutex_contention(void *queueObject, uint8_t queue_va_id);
#endif

#else /* VA_ENABLED == 0 - the whole recorder compiles away */

/* Same contract as a disabled category: the call still compiles, its
   arguments are NOT evaluated, and they still count as used so a build with
   tracing off does not fill up with -Wunused warnings. */
#define VA_RegisterTransportSend(fn) VA_DISCARD_ARGS(fn)
/* Variadic: matches both the 1-arg (DWT_CYCCNT) and 3-arg (CUSTOM_TIMER)
   VA_Init shapes. */
#define VA_Init(...) VA_DISCARD_ARGS(__VA_ARGS__)
#define VA_EmitSetupBundle() ((void)0)
#define VA_TickOverflowCheck() ((void)0)
#define VA_Drain() ((void)0)
#define VA_SnapshotFreeze() ((void)0)
#define VA_RegisterUserEvent(id, name) VA_DISCARD_ARGS(id, name)
#define VA_RegisterUserTrace(id, name, type) VA_DISCARD_ARGS(id, name, type)
#define VA_LogISRStart(isrId) VA_DISCARD_ARGS(isrId)
#define VA_LogISREnd(isrId) VA_DISCARD_ARGS(isrId)
#define VA_LogTrace(id, value) VA_DISCARD_ARGS(id, value)
#define VA_LogTraceFloat(id, value) VA_DISCARD_ARGS(id, value)
#define VA_LogString(id, msg) VA_DISCARD_ARGS(id, msg)
#define VA_LogToggle(id, state) VA_DISCARD_ARGS(id, state)
#define VA_LogEvent(id, state) VA_DISCARD_ARGS(id, state)
#define VA_EVENT_START(id) VA_DISCARD_ARGS(id)
#define VA_EVENT_END(id) VA_DISCARD_ARGS(id)
#define VA_LogGPIO(id, state) VA_DISCARD_ARGS(id, state)
#define VA_LogCounter(id, value) VA_DISCARD_ARGS(id, value)
#define VA_LogHeap(id, usedBytes) VA_DISCARD_ARGS(id, usedBytes)
#define VA_RegisterGPIO(id, name) VA_DISCARD_ARGS(id, name)
#define VA_RegisterHeap(id, name, totalSize) VA_DISCARD_ARGS(id, name, totalSize)

#define va_taskswitchedin(h) VA_DISCARD_ARGS(h)
#define va_taskswitchedout(h) VA_DISCARD_ARGS(h)
#define va_taskcreated(h, n) VA_DISCARD_ARGS(h, n)
#define va_taskdeleted(h) VA_DISCARD_ARGS(h)
#define va_taskrenamed(h, n) VA_DISCARD_ARGS(h, n)
#define va_logtasknotifygive(s, d, v) VA_DISCARD_ARGS(s, d, v)
#define va_logtasknotifytake(h, v) VA_DISCARD_ARGS(h, v)
#define va_logQueueObjectCreate(queueObject, name) VA_DISCARD_ARGS(queueObject, name)
#define va_logQueueObjectCreateWithType(queueObject, typeHint) VA_DISCARD_ARGS(queueObject, typeHint)
#define va_logQueueObjectCreateTyped(queueObject, name, type) VA_DISCARD_ARGS(queueObject, name, type)
#define va_logQueueObjectGiveTyped(queueObject, type) VA_DISCARD_ARGS(queueObject, type)
#define va_logQueueObjectTakeTyped(queueObject, type) VA_DISCARD_ARGS(queueObject, type)
#define va_updateQueueObjectType(queueObject, typeHint) VA_DISCARD_ARGS(queueObject, typeHint)
#define va_logQueueObjectDelete(queueObject) VA_DISCARD_ARGS(queueObject)
#define va_logQueueObjectSetName(queueObject, name) VA_DISCARD_ARGS(queueObject, name)
#define va_logQueueObjectGive(queueObject, timeout) VA_DISCARD_ARGS(queueObject, timeout)
#define va_logQueueObjectTake(queueObject, timeout) VA_DISCARD_ARGS(queueObject, timeout)
#define va_logQueueObjectOpFailed(queueObject, giveSide, detail) VA_DISCARD_ARGS(queueObject, giveSide, detail)
#define va_logObjectOpFailedTyped(queueObject, type, giveSide, detail) \
    (VA_DISCARD_2(queueObject, type), VA_DISCARD_2(giveSide, detail))
#define va_logEventFlagSet(flagObject, bits) VA_DISCARD_ARGS(flagObject, bits)
#define va_logEventFlagWaitEnd(flagObject, bits) VA_DISCARD_ARGS(flagObject, bits)
#define va_logWorkArm(handler, delayMs) VA_DISCARD_ARGS(handler, delayMs)
#define va_logWorkCancel(handler) VA_DISCARD_ARGS(handler)
#define va_logTimerArm(timerObject, durationMs, periodMs) VA_DISCARD_ARGS(timerObject, durationMs, periodMs)
#define va_logQueueObjectBlocking(queueObject) VA_DISCARD_ARGS(queueObject)
#define va_logHeapAlloc(heapObject, allocBytes) VA_DISCARD_ARGS(heapObject, allocBytes)
#define va_logHeapFree(heapObject, allocatedBytes) VA_DISCARD_ARGS(heapObject, allocatedBytes)
#define va_logHeapAllocFailed(heapObject, requestedBytes) VA_DISCARD_ARGS(heapObject, requestedBytes)
#define va_logHeapCapacity(heapObject, name, totalSize) VA_DISCARD_ARGS(heapObject, name, totalSize)
#define va_logSleepEnter(h) VA_DISCARD_ARGS(h)
#define va_logSleepExit(h) VA_DISCARD_ARGS(h)
#define va_logPMSuspendEnter() ((void)0)
#define va_logPMSuspendExit(state) VA_DISCARD_ARGS(state)

#define VA_IsInit() false

#endif /* VA_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* VIEWALYZER_H */
