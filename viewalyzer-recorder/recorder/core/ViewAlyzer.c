/**
 * @file ViewAlyzer.c
 * @brief ViewAlyzer Recorder Firmware - RTOS-Agnostic Core Engine
 *
 * This file contains the transport layer, timestamp engine, packet emission,
 * and generic task/object map management.  All RTOS-specific logic (stack
 * introspection, queue-type detection, mutex-holder queries) lives in the
 * corresponding adapter file (VA_Adapter_FreeRTOS.c, VA_Adapter_Zephyr.c, …).
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

#include "ViewAlyzer.h"

#ifdef __cplusplus
extern "C"
{
#endif
#if (VA_ENABLED == 1)

#include "VA_Internal.h"
#include <string.h>

#if VA_TRANSPORT_IS_JLINK
#include "SEGGER_RTT.h"
/* VA_RTT_MODE always arrives defined from ViewAlyzerConfig.h (default
   SEGGER_RTT_MODE_NO_BLOCK_SKIP); no fallback here. */
#if VA_RTT_BUFFER_SIZE > 0
    static uint8_t s_va_rtt_up_buffer[VA_RTT_BUFFER_SIZE];
#endif
#endif

#if VA_TRANSPORT_IS_CUSTOM
#include "viewalyzer_cobs.h"
    static VA_TransportSendFn s_user_send_fn = NULL;
#endif

/* ── Throughput-test counters (VA_TP_TEST=1 builds only) ─────────────── */
/* Little-endian layout is a host contract: do not reorder fields. */
#if defined(VA_TP_TEST) && (VA_TP_TEST == 1)
VA_TpCounters_t _VA_TP __attribute__((aligned(4)));
static const char VA_TP_MAGIC[8] = "VATPCNT1";
#define VA_TP_OFFER(len) do { _VA_TP.offeredPackets++; _VA_TP.offeredBytes += (uint32_t)(len); } while (0)
#define VA_TP_DROP(len)  do { _VA_TP.droppedPackets++; _VA_TP.droppedBytes  += (uint32_t)(len); } while (0)
#else
#define VA_TP_OFFER(len) ((void)0)
#define VA_TP_DROP(len)  ((void)0)
#endif

/* Lightweight uint32-to-decimal into a prefix buffer, e.g. "CLK:170000000" */
static char *_va_u32_to_str(char *buf, size_t buf_size, const char *prefix, uint32_t val)
{
    size_t plen = strlen(prefix);
    if (plen >= buf_size) { buf[0] = '\0'; return buf; }
    memcpy(buf, prefix, plen);

    char tmp[11]; /* max 10 digits for uint32 + NUL */
    int i = (int)sizeof(tmp) - 1;
    tmp[i] = '\0';
    if (val == 0) { tmp[--i] = '0'; }
    else { while (val) { tmp[--i] = '0' + (char)(val % 10); val /= 10; } }
    const char *digits = &tmp[i];
    size_t dlen = sizeof(tmp) - 1 - (size_t)i;

    if (plen + dlen >= buf_size) dlen = buf_size - plen - 1;
    memcpy(buf + plen, digits, dlen);
    buf[plen + dlen] = '\0';
    return buf;
}

/* Bounded copy into a VA_MAX_TASK_NAME_LEN field, always NUL-terminated. */
static inline void _va_copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > (size_t)(VA_MAX_TASK_NAME_LEN - 1))
        n = (size_t)(VA_MAX_TASK_NAME_LEN - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#if VA_NEEDS_OBJECT_REGISTRY
/* Lightweight "name_Suffix" concatenation */
static void _va_strcat_suffix(char *buf, size_t buf_size,
                             const char *name, const char *suffix)
{
    if (buf_size == 0) return;

    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);

    size_t pos = 0;

    /* If enough space, preserve suffix fully and trim name */
    if (buf_size >= slen + 2) {
        size_t max_nlen = buf_size - slen - 2; /* space for '_' + suffix + '\0' */
        if (nlen > max_nlen) nlen = max_nlen;

        if (nlen > 0) {
            memcpy(buf, name, nlen);
            pos = nlen;
        }

        buf[pos++] = '_';
        memcpy(buf + pos, suffix, slen);
        pos += slen;
    }
    else {
        /* Not enough room for full suffix → truncate suffix */
        size_t copy = buf_size - 1; /* leave space for '\0' */
        if (copy > 0) {
            memcpy(buf, suffix, copy);
            pos = copy;
        }
    }

    buf[pos] = '\0';
}
#endif /* VA_NEEDS_OBJECT_REGISTRY */

    /* 64-bit software-extension state for the tick source. Wrap is
       detected by decrease, so the clock must be read at least once per
       wrap period: every emitted event does; VA_TickOverflowCheck()
       covers quiet gaps. */
    static volatile uint32_t _va_ts_ovf  = 0;
    static volatile uint32_t _va_ts_last = 0;

#if VA_TS_IS_CUSTOM
    /* Tick source. Trace calls are legal before VA_Init (boot-time kernel
       hooks), so the pointer must always be callable: it starts on the
       stub and returns to it when VA_Init rejects a source. */
    static uint32_t _va_ts_none(void) { return 0u; }
    static VA_TimestampFn _va_ts_fn = _va_ts_none;
#define _VA_TICK_READ() ((_va_ts_fn()) & VA_TS_SOURCE_MASK)
#else
#define _VA_TICK_READ() (DWT->CYCCNT)
#endif

    /* ── Shared global state (exposed via VA_Internal.h) ────────── */
    volatile bool VA_IS_INIT = false;

    /* Stream sync marker: "VAZ" + wire version (binary), "SYNC" + wire
       version (ASCII), 0xAA 0x55. Carries no sequence byte itself. */
    static const uint8_t VA_SYNC_MARKER[] = {0x56, 0x41, 0x5A, VA_WIRE_VERSION,
                                             0x53, 0x59, 0x4E, 0x43,
                                             '0', '0' + VA_WIRE_VERSION,
                                             0xAA, 0x55};

    /* Absolute packet count since VA_Init; low 8 bits go on the wire. */
    static uint32_t _va_seq = 0;

    /* Tick rate of the timestamp source: the CPU clock for DWT_CYCCNT, the
       registered tick_hz for CUSTOM_TIMER. Reported as CLK: and used for
       every interval-in-ticks computation. */
    static uint32_t _va_tick_hz = 0;

/* Info-packet markers have a separate bound from display names. */
#define VA_INFO_TASKMAP_FULL  "ERR:TASKMAPFULL"
#define VA_INFO_USERMAP_FULL  "ERR:USERMAPFULL"
/* Registry-overflow reports for the remaining registries; same latched,
   re-announced-per-bundle pattern as the task/user-trace pair above. */
#define VA_INFO_OBJMAP_FULL   "ERR:OBJMAPFULL"
#define VA_INFO_EVTMAP_FULL   "ERR:EVTMAPFULL"
#define VA_INFO_GPIOMAP_FULL  "ERR:GPIOMAPFULL"
#define VA_INFO_HEAPMAP_FULL  "ERR:HEAPMAPFULL"
/* Timestamp-source init failures: emitted once by VA_Init, which then
   refuses to start, so a dead or misconfigured tick source shows up on
   the host as a named error instead of a capture with zero-time events. */
#define VA_INFO_TS_NULL       "ERR:TS_NULL"
#define VA_INFO_TS_HZ_ZERO    "ERR:TS_HZ_ZERO"
#define VA_INFO_TS_DEAD       "ERR:TS_DEAD"
typedef char va_assert_info_markers_fit_[
    (sizeof (VA_INFO_TASKMAP_FULL) <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_USERMAP_FULL) <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_OBJMAP_FULL)  <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_EVTMAP_FULL)  <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_GPIOMAP_FULL) <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_HEAPMAP_FULL) <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_TS_NULL)      <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_TS_HZ_ZERO)   <= VA_CONTROL_TEXT_CAPACITY &&
     sizeof (VA_INFO_TS_DEAD)      <= VA_CONTROL_TEXT_CAPACITY) ? 1 : -1];

#if VA_NEEDS_TASK_REGISTRY
    /* --- Task ID Mapping (RTOS-agnostic) --- */
    VA_TaskMapEntry_t taskMap[VA_MAX_TASKS];
    uint8_t next_task_id = 1;
    /* Latched registry-full condition, re-announced by every setup bundle. */
    static bool _va_task_map_overflow = false;

    /* One-entry MRU cache; guarded by the caller's critical section. */
    static void *_va_task_cache_handle = NULL;
    static int   _va_task_cache_idx    = -1;
#endif

#if VA_BUNDLE_SERVICE
    /* Bundle due tracking (extended 64-bit tick timestamps, so it is
       wrap-safe for any tick-source width; ISR-safe). A bundle becomes due
       periodically (VA_AUTO_SETUP_INTERVAL_MS) and/or when a RAM-buffer
       host attaches; it is emitted from thread context only. */
    static struct {
        uint64_t last_ts;    /* extended timestamp at the last bundle */
        uint64_t interval;   /* ticks; 0 = no periodic bundle */
        /* Last thread-context service (0 = never). A due bundle waits for
           VA_TickOverflowCheck() while this is within defer_ticks. */
        uint64_t service_ts;
        uint64_t defer_ticks;
        /* volatile: set inside a CS (possibly in an ISR), polled from
           thread context - without it LTO can hoist the poll. */
        volatile bool due;
        volatile bool emitting;   /* thread context only */
    } _va_bundle;
#endif

#if VA_HAS_RTOS && VA_TRACE_STACK_USAGE
    static uint64_t _va_stack_heartbeat_ticks = 0; /* precomputed in VA_Init */
#endif

#if VA_NEEDS_TASK_REGISTRY
    /* Global variables to store task information during creation */
    volatile void *g_task_pxStack = NULL;
    volatile void *g_task_pxEndOfStack = NULL;
    volatile uint32_t g_task_uxPriority = 0;
    volatile uint32_t g_task_uxBasePriority = 0;
    volatile uint32_t g_task_ulStackDepth = 0;
#endif

#if VA_TRACE_USER_EVENTS
    /* User Event Tracking (Independent of RTOS) */
    typedef struct
    {
        uint8_t id;
        char name[VA_MAX_TASK_NAME_LEN];
        bool active;
    } VA_UserEventMapEntry_t;
    static VA_UserEventMapEntry_t userEventMap[VA_MAX_USER_EVENTS];
    /* Latched registry-full condition, re-announced by every setup bundle. */
    static bool _va_user_event_overflow = false;
#endif

#if VA_NEEDS_USER_TRACE_REGISTRY
    /* User trace registry (holds ISR name registrations too). Stored so the
       periodic bundle can re-emit the maps for late-attaching hosts. */
    typedef struct
    {
        uint8_t id;
        uint8_t type;                       /* VA_UserTraceType_t */
        char name[VA_MAX_TASK_NAME_LEN];
        bool active;
    } VA_UserTraceMapEntry_t;
    static VA_UserTraceMapEntry_t userTraceMap[VA_MAX_USER_TRACES];
    /* Latched registry-full condition, re-announced by every setup bundle. */
    static bool _va_user_trace_overflow = false;
#endif

#if VA_NEEDS_OBJECT_REGISTRY
    /* --- Queue / sync-object map (RTOS-agnostic storage, adapter determines type) --- */
    VA_QueueObjectMapEntry_t queueObjectMap[VA_MAX_SYNC_OBJECTS];
    uint8_t next_queue_object_id = 1;
    /* Latched full/exhausted condition, re-announced by every setup bundle
       (slots are reused after delete, the 255-id space is not). */
    static bool _va_obj_map_overflow = false;
#endif

#if VA_TRACE_GPIO
    /* GPIO channel registry. Stored so the periodic bundle can re-emit the
       name map for late-attaching hosts. */
    typedef struct
    {
        uint8_t id;
        char name[VA_MAX_TASK_NAME_LEN];
        bool active;
    } VA_GpioMapEntry_t;
    static VA_GpioMapEntry_t gpioMap[VA_MAX_GPIOS];
    /* Latched registry-full condition, re-announced by every setup bundle. */
    static bool _va_gpio_map_overflow = false;
#endif

#if VA_TRACE_HEAP_METRICS
    /* Manual heap-gauge registry (same late-attach rationale). */
    typedef struct
    {
        uint8_t id;
        char name[VA_MAX_TASK_NAME_LEN];
        uint32_t totalSize;
        bool active;
    } VA_HeapGaugeMapEntry_t;
    static VA_HeapGaugeMapEntry_t heapGaugeMap[VA_MAX_HEAPS];
    /* Latched registry-full condition, re-announced by every setup bundle. */
    static bool _va_heap_map_overflow = false;
#endif

/* ── Post-mortem snapshot ring (VA_RAMBUF_MODE_WRAP / VA_SNAPSHOT) ─── */
#if VA_PM_RING
/* Wrapping ring holding the most recent trace window, read out through the
   debug probe. Packets are framed [length(1)][bytes]. The control block is
   a host-protocol contract: fixed order, little-endian, magic written last.
   Single writer inside the VA critical section. */
typedef struct
{
    char              magic[16];   /* "ViewAlyzerPM01", written last        */
    uint32_t          bufferAddr;  /* absolute address of the ring bytes    */
    uint32_t          bufferSize;  /* ring capacity in bytes                */
    volatile uint32_t wrOff;       /* next write position                   */
    volatile uint32_t rdOff;       /* oldest valid frame (firmware-owned)   */
    volatile uint32_t discarded;   /* frames overwritten + oversize drops   */
    volatile uint32_t flags;       /* bit0 frozen, bit1 has wrapped         */
    uint32_t          cpuFreqHz;   /* from VA_Init, 0 = unknown             */
    uint8_t           wireVersion; /* VA_WIRE_VERSION                       */
    uint8_t           tsBytes;     /* wire timestamp width (4)              */
    uint16_t          recorderVersion; /* (major << 8) | minor              */
    uint32_t          setupAddr;   /* names area (latest setup bundle), 0 = none */
    volatile uint32_t setupUsed;   /* valid bytes in the names area         */
} VA_PmRingControlBlock_t;

#define VA_PM_FLAG_FROZEN  0x1u
#define VA_PM_FLAG_WRAPPED 0x2u

#if VA_PM_VIA_TRANSPORT
#define VA_PM_SIZE       ((uint32_t)VA_RAMBUF_SIZE)
#define VA_PM_ATTRIBUTES VA_RAMBUF_ATTRIBUTES
#else
#define VA_PM_SIZE       ((uint32_t)VA_SNAPSHOT_SIZE)
#define VA_PM_ATTRIBUTES VA_SNAPSHOT_ATTRIBUTES
#endif

static VA_PM_ATTRIBUTES uint8_t s_va_pm_storage[VA_PM_SIZE];
VA_PM_ATTRIBUTES VA_PmRingControlBlock_t _VA_PMBUF __attribute__((aligned(4)));

#if VA_SNAPSHOT_SETUP_SIZE > 0
/* Names area: always holds the latest setup bundle so a wrapped window
   still dumps with task/trace names. Raw wire bytes, no frame prefixes. */
static VA_PM_ATTRIBUTES uint8_t s_va_pm_setup[VA_SNAPSHOT_SETUP_SIZE];
#endif

static const char VA_PM_MAGIC[16] = "ViewAlyzerPM01";

/* One byte is always kept unused so wrOff == rdOff means exactly "empty". */
static inline uint32_t _va_pm_free(void)
{
    uint32_t wr = _VA_PMBUF.wrOff;
    uint32_t rd = _VA_PMBUF.rdOff;
    return (rd > wr) ? (rd - wr - 1u) : (VA_PM_SIZE - wr + rd - 1u);
}

#if VA_SNAPSHOT_SETUP_SIZE > 0
/* Mirror the setup stream into the names area: a sync marker restarts it,
   setup packets (0x70..0x7F) append while they fit. Caller holds the CS. */
static void _va_pm_capture_setup(const uint8_t *data, uint32_t length)
{
    if (length == sizeof(VA_SYNC_MARKER) && memcmp(data, VA_SYNC_MARKER, sizeof(VA_SYNC_MARKER)) == 0)
    {
        _VA_PMBUF.setupUsed = 0;
        __DMB();
        memcpy(&s_va_pm_setup[0], data, length);
        __DMB();
        _VA_PMBUF.setupUsed = length;
        return;
    }
    if (data[0] < 0x70u || data[0] > 0x7Fu)
        return;
    uint32_t used = _VA_PMBUF.setupUsed;
    if (used == 0u || used + length > (uint32_t)VA_SNAPSHOT_SETUP_SIZE)
        return;   /* no marker yet, or full: drop whole packets off the end */
    memcpy(&s_va_pm_setup[used], data, length);
    __DMB();
    _VA_PMBUF.setupUsed = used + length;
}
#endif

/* Store one packet as a frame, discarding whole frames from the front until
   it fits. Caller must hold a VA critical section (single writer). */
static void _va_pm_write(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT || length == 0u)
        return;
    if (_VA_PMBUF.flags & VA_PM_FLAG_FROZEN)
        return;
#if VA_SNAPSHOT_SETUP_SIZE > 0
    _va_pm_capture_setup(data, length);
#endif
    if (length > 255u || length + 1u > VA_PM_SIZE - 1u)
    {
        _VA_PMBUF.discarded++;
        return;
    }

    while (_va_pm_free() < length + 1u)
    {
        /* Hop the oldest whole frame. */
        uint32_t rd = _VA_PMBUF.rdOff;
        uint32_t hop = 1u + (uint32_t)s_va_pm_storage[rd];
        rd += hop;
        if (rd >= VA_PM_SIZE)
            rd -= VA_PM_SIZE;
        _VA_PMBUF.rdOff = rd;
        _VA_PMBUF.discarded++;
        _VA_PMBUF.flags |= VA_PM_FLAG_WRAPPED;
    }

    uint32_t wr = _VA_PMBUF.wrOff;
    s_va_pm_storage[wr] = (uint8_t)length;
    if (++wr >= VA_PM_SIZE)
        wr = 0;
    uint32_t chunk = VA_PM_SIZE - wr;
    if (chunk > length)
        chunk = length;
    memcpy(&s_va_pm_storage[wr], data, chunk);
    if (chunk < length)
        memcpy(&s_va_pm_storage[0], data + chunk, length - chunk);
    wr += length;
    if (wr >= VA_PM_SIZE)
        wr -= VA_PM_SIZE;
    __DMB();   /* frame bytes must be probe-visible before the offset is */
    _VA_PMBUF.wrOff = wr;
}

/* Stop snapshot writes so the current window survives. Any-context safe;
   call from a fault or assert handler. Nothing needs flushing. */
void VA_SnapshotFreeze(void)
{
    _VA_PMBUF.flags |= VA_PM_FLAG_FROZEN;
    __DMB();
}
#endif /* VA_PM_RING */

/* ── Transport layer ─────────────────────────────────────────────── */

#if VA_TRANSPORT_IS_ITM
/* Spin limit on the stimulus FIFO before declaring the pipe stalled;
   prevents an infinite spin when no host is draining SWO. */
#ifndef VA_ITM_STALL_SPIN_LIMIT
#define VA_ITM_STALL_SPIN_LIMIT 50000u
#endif

static volatile uint8_t _va_itm_stalled = 0;

static inline int ITM_WaitReady(uint8_t port)
{
    if (ITM->PORT[port].u32 != 0)
        return 1;
    for (uint32_t i = 0; i < VA_ITM_STALL_SPIN_LIMIT; ++i)
        if (ITM->PORT[port].u32 != 0)
            return 1;
    _va_itm_stalled = 1;
    return 0;
}

static inline int ITM_SendU32(uint8_t port, uint32_t value)
{
    if (!ITM_WaitReady(port))
        return 0;
    ITM->PORT[port].u32 = value;
    return 1;
}
static inline int ITM_SendU16(uint8_t port, uint16_t value)
{
    if (!ITM_WaitReady(port))
        return 0;
    ITM->PORT[port].u16 = value;
    return 1;
}
static inline int ITM_SendU8(uint8_t port, uint8_t value)
{
    if (!ITM_WaitReady(port))
        return 0;
    ITM->PORT[port].u8 = value;
    return 1;
}
static uint32_t _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT)
        return 0;
#if VA_TRANSPORT_BUFFERED
#if (__ARM_ARCH >= 8)
    if (!(DCB->DEMCR & DCB_DEMCR_TRCENA_Msk))
        return 0;
#else
    if (!(CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk))
        return 0;
#endif
    if (!(ITM->TCR & ITM_TCR_ITMENA_Msk) || !(ITM->TER & (1UL << VA_ITM_PORT)))
        return 0;
#endif
    if (_va_itm_stalled)
    {
#if VA_TRANSPORT_BUFFERED
        if (ITM->PORT[VA_ITM_PORT].u32 == 0)
            return 0;
        _va_itm_stalled = 0;
#else
        VA_TP_DROP(length);
        return 0;
#endif
    }
    uint32_t i = 0;
    while (length >= 4)
    {
        uint32_t word = ((uint32_t)data[i + 3] << 24) |
                        ((uint32_t)data[i + 2] << 16) |
                        ((uint32_t)data[i + 1] << 8) |
                        ((uint32_t)data[i + 0] << 0);
        if (!ITM_SendU32(VA_ITM_PORT, word))
            return i;
        i += 4;
        length -= 4;
    }
    if (length >= 2)
    {
        uint16_t half = (uint16_t)(((uint16_t)data[i + 1] << 8) | (uint16_t)data[i + 0]);
        if (!ITM_SendU16(VA_ITM_PORT, half))
            return i;
        i += 2;
        length -= 2;
    }
    if (length > 0)
    {
        if (!ITM_SendU8(VA_ITM_PORT, data[i]))
            return i;
        i++;
        length--;
    }
    return i;
}

#elif VA_TRANSPORT_IS_JLINK
#if VA_TRANSPORT_BUFFERED
static uint32_t _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT)
        return 0;
    /* Only the drain writes this channel; the host can only free space. */
    uint32_t available = SEGGER_RTT_GetAvailWriteSpace(VA_RTT_CHANNEL);
    if (length > available)
        length = available;
    if (length == 0)
        return 0;
    return SEGGER_RTT_Write(VA_RTT_CHANNEL, data, length);
}
#else
static void _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT)
        return;
    unsigned written = SEGGER_RTT_Write(VA_RTT_CHANNEL, data, length);
    if (written < length)
        VA_TP_DROP(length - written);
}
#endif

#elif VA_TRANSPORT_IS_CUSTOM
static uint32_t _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT || s_user_send_fn == NULL)
        return 0;
    uint32_t written = s_user_send_fn(data, length);
    return written <= length ? written : 0;
}

#elif VA_TRANSPORT_IS_RAMBUF
#if VA_PM_VIA_TRANSPORT
/* WRAP mode: the transport ring IS the post-mortem snapshot ring. */
static void _va_send_bytes(const uint8_t *data, uint32_t length)
{
    _va_pm_write(data, length);
}
#else
/* RAM ring buffer drained by the host through the debug probe. The struct
   layout is a host-protocol contract: fixed order, little-endian, magic at
   offset 0, written last. Do not reorder or insert fields. */
typedef struct
{
    char              magic[16];      /* "ViewAlyzerRB01", written last      */
    uint32_t          bufferAddr;     /* absolute address of the ring bytes  */
    uint32_t          bufferSize;     /* ring capacity in bytes              */
    volatile uint32_t wrOff;          /* target-owned write offset           */
    volatile uint32_t rdOff;          /* host-owned read offset              */
    volatile uint32_t droppedPackets; /* packets dropped since VA_Init       */
    uint32_t          flags;          /* bit0 = VA_RAMBUF_MODE               */
    uint32_t          cpuFreqHz;      /* from VA_Init, 0 = unknown           */
    uint8_t           wireVersion;    /* VA_WIRE_VERSION                     */
    uint8_t           tsBytes;        /* wire timestamp width (4)            */
    uint16_t          recorderVersion;/* (major << 8) | minor                */
} VA_RamBufControlBlock_t;

static VA_RAMBUF_ATTRIBUTES uint8_t s_va_rambuf_storage[VA_RAMBUF_SIZE];
VA_RAMBUF_ATTRIBUTES VA_RamBufControlBlock_t _VA_RAMBUF __attribute__((aligned(4)));

static const char VA_RAMBUF_MAGIC[16] = "ViewAlyzerRB01";

/* One byte is always kept unused so wrOff == rdOff means exactly "empty". */
static inline uint32_t _va_rambuf_free(uint32_t wr)
{
    uint32_t rd = _VA_RAMBUF.rdOff;
    return (rd > wr) ? (rd - wr - 1u) : ((uint32_t)VA_RAMBUF_SIZE - wr + rd - 1u);
}

static void _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT || length == 0)
        return;
    if (length > (uint32_t)VA_RAMBUF_SIZE - 1u)
    {
        _VA_RAMBUF.droppedPackets++;
        VA_TP_DROP(length);
        return;
    }
    uint32_t wr = _VA_RAMBUF.wrOff;
#if (VA_RAMBUF_MODE == VA_RAMBUF_MODE_BLOCK)
    /* Lossless: stalls the firmware when no host is draining. */
    while (_va_rambuf_free(wr) < length) {}
#else
    /* Drop whole packets, never truncate - the stream stays parseable. */
    if (_va_rambuf_free(wr) < length)
    {
        _VA_RAMBUF.droppedPackets++;
        VA_TP_DROP(length);
        return;
    }
#endif
    uint32_t chunk = (uint32_t)VA_RAMBUF_SIZE - wr;
    if (chunk > length)
        chunk = length;
    if (chunk == 7u)
    {
        /* Common event size; byte stores support every ring alignment. */
        volatile uint8_t *dst = &s_va_rambuf_storage[wr];
        dst[0] = data[0];
        dst[1] = data[1];
        dst[2] = data[2];
        dst[3] = data[3];
        dst[4] = data[4];
        dst[5] = data[5];
        dst[6] = data[6];
    }
    else
        memcpy(&s_va_rambuf_storage[wr], data, chunk);
    if (chunk < length)
        memcpy(&s_va_rambuf_storage[0], data + chunk, length - chunk);
    wr += length;
    if (wr >= (uint32_t)VA_RAMBUF_SIZE)
        wr -= (uint32_t)VA_RAMBUF_SIZE;
    __DMB();   /* ring bytes must be probe-visible before the offset is */
    _VA_RAMBUF.wrOff = wr;
}

#if VA_RAMBUF_ATTACH_BUNDLE
/* Host-attach detection. The target never writes rdOff, so any change is
   the host consuming; a change after "no host" is an attach and earns one
   setup bundle (readers that skip the stale backlog move rdOff too, so
   they are detected even when they never read a byte of it). The host is
   considered gone once pending data stays unconsumed for
   VA_RAMBUF_HOST_IDLE_MS; an empty ring is not evidence either way. */
static uint32_t _va_rambuf_seen_rd;      /* rdOff at the last poll          */
static uint64_t _va_rambuf_consume_ts;   /* last observed consumption       */
static uint64_t _va_rambuf_idle_ticks;   /* VA_RAMBUF_HOST_IDLE_MS in ticks */
static bool     _va_rambuf_host_active;

/* Thread-context poll from VA_TickOverflowCheck(); true once per attach. */
static bool _va_rambuf_poll_host(uint64_t now)
{
    uint32_t rd = _VA_RAMBUF.rdOff;
    uint32_t wr = _VA_RAMBUF.wrOff;
    if (rd != _va_rambuf_seen_rd)
    {
        _va_rambuf_seen_rd    = rd;
        _va_rambuf_consume_ts = now;
        if (_va_rambuf_host_active)
            return false;
        _va_rambuf_host_active = true;
        return true;
    }
    if (_va_rambuf_host_active && rd != wr &&
        (now - _va_rambuf_consume_ts) >= _va_rambuf_idle_ticks)
        _va_rambuf_host_active = false;
    return false;
}
#endif /* VA_RAMBUF_ATTACH_BUNDLE */
#endif /* VA_PM_VIA_TRANSPORT */

#else
#error "VA_TRANSPORT must be ARM_ITM, JLINK_RTT, CUSTOM_TRANSPORT, or RAM_BUFFER"
#endif /* VA_TRANSPORT */

/* ── Optional buffered transport - RAM ring drained by VA_Drain() ─── */
#if VA_TRANSPORT_BUFFERED
/* Free-running head/tail counters; all mutation inside a VA critical
   section. */
static uint8_t           _va_ring[VA_BUFFER_SIZE];
static volatile uint32_t _va_ring_head = 0;   /* next write index (free-running) */
static volatile uint32_t _va_ring_tail = 0;   /* next read index  (free-running) */
static volatile uint32_t _va_dropped_packets = 0;
static volatile uint32_t _va_dropped_bytes   = 0;
static uint32_t          _va_reported_drops  = 0;
static bool              _va_drain_active    = false;

static inline uint32_t _va_ring_used(void) { return _va_ring_head - _va_ring_tail; }

/* Push a whole packet or drop it entirely (a partial write would corrupt the
   stream). Caller must hold a VA critical section. */
static void _va_ring_push(const uint8_t *data, uint32_t length)
{
    if (length > (uint32_t)(VA_BUFFER_SIZE) - _va_ring_used())
    {
        if (_va_dropped_packets != UINT32_MAX)
            _va_dropped_packets++;
        if (length > UINT32_MAX - _va_dropped_bytes)
            _va_dropped_bytes = UINT32_MAX;
        else
            _va_dropped_bytes += length;
        VA_TP_DROP(length);
        return;
    }
    for (uint32_t i = 0; i < length; ++i)
        _va_ring[(_va_ring_head + i) % VA_BUFFER_SIZE] = data[i];
    _va_ring_head += length;
}
#endif /* VA_TRANSPORT_BUFFERED */

#if VA_METADATA
#include "VA_Metadata.h"
#endif

/* ── Packet emission layer ───────────────────────────────────────── */
static inline void _va_emit_packet_raw(const uint8_t *data, uint32_t length)
{
    VA_TP_OFFER(length);
#if VA_SNAPSHOT
    /* Tee every packet into the post-mortem ring, pre-COBS. */
    _va_pm_write(data, length);
#endif
#if VA_TRANSPORT_IS_CUSTOM
    /* Packet emission holds the recorder critical section. */
    static uint8_t cobs_buf[VA_MAX_PACKET_SIZE + (VA_MAX_PACKET_SIZE / 254) + 2];
    size_t encoded_len = va_cobs_encode(data, (size_t)length, cobs_buf, sizeof(cobs_buf));
    if (encoded_len == 0)
    {
        VA_TP_DROP(length);
        return;  /* sequence is already consumed; the host sees a loss */
    }
#if VA_TRANSPORT_BUFFERED
    _va_ring_push(cobs_buf, (uint32_t)encoded_len);   /* drained later, already framed */
#else
    _va_send_bytes(cobs_buf, (uint32_t)encoded_len);
#endif
#else
#if VA_TRANSPORT_BUFFERED
    _va_ring_push(data, length);
#else
    _va_send_bytes(data, length);
#endif
#endif
}

void _va_emit_packet(uint8_t *data, uint32_t length)
{
    /* Stamped in the single funnel so sequence order is exactly wire order;
       packets dropped downstream keep their number (host sees gaps). */
    data[1] = (uint8_t)_va_seq++;
#if VA_METADATA
    if ((data[0] >= 0x70u && data[0] <= 0x7Fu) || data[0] == VA_EVENT_TASK_CREATE)
        _va_metadata_store(data, length);
    if ((data[0] & VA_EVENT_TYPE_MASK) == VA_EVENT_TASK_SWITCH)
        _va_metadata_running = (data[0] & VA_EVENT_FLAG_START_END) ? data[2] : 0;
#endif
    /* Triggering packet first, periodic bundle after: time-correlation
       anchors on the packet's wire position. */
    _va_emit_packet_raw(data, length);
#if VA_METADATA
    _va_metadata_poll();
#endif

#if VA_BUNDLE_SERVICE
    /* Only flag the bundle as due - emitting ~1 KB inline here would block
       interrupts; _va_service_pending_bundle() emits from thread context.
       "Now" comes from the extension state, not a fresh clock read: a torn
       ovf/last pair only fires one bundle early or late. */
    {
        const uint64_t interval = _va_bundle.interval;
        const uint64_t now = (((uint64_t)_va_ts_ovf) << VA_TS_SOURCE_BITS)
                             | _va_ts_last;
        if (interval != 0u && !_va_bundle.emitting && !_va_bundle.due &&
            (now - _va_bundle.last_ts) >= interval)
            _va_bundle.due = true;
    }
#endif
}

/* Markers carry no sequence byte, so they bypass _va_emit_packet. */
static inline void _va_emit_sync_marker(void)
{
    _va_emit_packet_raw(VA_SYNC_MARKER, sizeof(VA_SYNC_MARKER));
}

/* True in any exception context, including PendSV (the scheduler path). */
static inline bool _va_in_isr(void)
{
    return (__get_IPSR() & 0x1FFu) != 0u;
}

/* True when the caller already runs with interrupts masked (PRIMASK, or a
   kernel critical section via BASEPRI). The bundle's short per-packet CS
   cannot release a mask it does not own, so emitting here would keep
   interrupts blocked for the whole bundle. */
static inline bool _va_irqs_masked(void)
{
    if (__get_PRIMASK() != 0u)
        return true;
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) \
 || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
    if (__get_BASEPRI() != 0u)
        return true;
#endif
    return false;
}

#if VA_BUNDLE_SERVICE
/* Emit a due bundle. The caller has verified unmasked thread context. */
static void _va_emit_due_bundle(void)
{
    if (!_va_bundle.due || _va_bundle.emitting)
        return;
    _va_bundle.due      = false;
    _va_bundle.last_ts  = _va_get_timestamp();
    _va_bundle.emitting = true;
    VA_EmitSetupBundle();          /* emits each packet under its own short CS */
    _va_bundle.emitting = false;
}
#endif

/* Hook / VA_Log* path: the opportunistic service point. */
void _va_service_pending_bundle(void)
{
#if VA_BUNDLE_SERVICE
    if (!_va_bundle.due || _va_bundle.emitting)
        return;
    if (_va_in_isr() || _va_irqs_masked())
        return;   /* defer to the next unmasked thread-context log call */

    /* A periodic thread-context VA_TickOverflowCheck() caller exists: leave
       the bundle to it. Kernel hooks run in whichever task the kernel is
       serving - FreeRTOS traceTIMER_EXPIRED, for instance, runs in the
       timer service task right before the callback - and the bundle is
       ~1 KB of packets, so paying it here delays that task by tens of
       microseconds. Hooks take over again if the service call stops. */
    if (_va_bundle.service_ts != 0u)
    {
        const uint64_t now = (((uint64_t)_va_ts_ovf) << VA_TS_SOURCE_BITS)
                             | _va_ts_last;
        if ((now - _va_bundle.service_ts) < _va_bundle.defer_ticks)
            return;
    }
    _va_emit_due_bundle();
#endif
}

/* ── Packet construction helpers (non-static - adapters use these) ─── */

/* Little-endian timestamp field; the single point controlling wire width. */
static inline uint32_t _va_put_ts(uint8_t *dst, uint64_t timestamp)
{
    dst[0] = (uint8_t)(timestamp >> 0);
    dst[1] = (uint8_t)(timestamp >> 8);
    dst[2] = (uint8_t)(timestamp >> 16);
    dst[3] = (uint8_t)(timestamp >> 24);
#if VA_TIMESTAMP_BYTES == 8
    dst[4] = (uint8_t)(timestamp >> 32);
    dst[5] = (uint8_t)(timestamp >> 40);
    dst[6] = (uint8_t)(timestamp >> 48);
    dst[7] = (uint8_t)(timestamp >> 56);
#endif
    return VA_TIMESTAMP_BYTES;
}

/* All builders lay packets out as [type][seq][rest...]; the seq slot is
   stamped centrally in _va_emit_packet. */

void _va_send_event_packet(uint8_t type_byte, uint8_t id, uint64_t timestamp)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES];
    packet[0] = type_byte;
    packet[1 + VA_SEQ_BYTES] = id;
    _va_put_ts(&packet[2 + VA_SEQ_BYTES], timestamp);
    _va_emit_packet(packet, sizeof(packet));
}

/* Emit a sequence checkpoint. Must be called inside a VA critical section:
   the payload is read from _va_seq before emission, and because no packet can
   intervene inside the CS it is exactly the absolute sequence number this
   packet itself gets stamped with (low 8 bits match its own seq byte). */
static void _va_send_seq_checkpoint(void)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 4];
    uint32_t seq = _va_seq;
    uint32_t p = 0;
    packet[p++] = VA_EVENT_SEQ_CHECKPOINT;
    p += VA_SEQ_BYTES;
    packet[p++] = 0;                /* id unused */
    p += _va_put_ts(&packet[p], _va_get_timestamp_unlocked());
    packet[p++] = (uint8_t)(seq >> 0);
    packet[p++] = (uint8_t)(seq >> 8);
    packet[p++] = (uint8_t)(seq >> 16);
    packet[p++] = (uint8_t)(seq >> 24);
    _va_emit_packet(packet, p);
}

#if VA_METADATA
/* One bounded publication: a full ring leaves the request pending and does
   not consume sequence numbers. No table walking or stack scanning here. */
static bool _va_metadata_checkpoint(bool sync)
{
    uint8_t packet[30];
    uint32_t n = sync ? 12u : 0u;
    uint32_t length = n + 11u + ((sync && _va_metadata_running) ? 7u : 0u);
    if (_va_rambuf_free(_VA_RAMBUF.wrOff) < length)
        return false;
    if (sync)
    {
        memcpy(packet, VA_SYNC_MARKER, 12);
        VA_TP_OFFER(12);
    }
    VA_TP_OFFER(11);
    uint32_t seq = _va_seq++;
    uint64_t now = _va_get_timestamp_unlocked();
    packet[n++] = VA_EVENT_SEQ_CHECKPOINT;
    packet[n++] = (uint8_t)seq;
    packet[n++] = 0;
    n += _va_put_ts(packet + n, now);
    packet[n++] = (uint8_t)seq;
    packet[n++] = (uint8_t)(seq >> 8);
    packet[n++] = (uint8_t)(seq >> 16);
    packet[n++] = (uint8_t)(seq >> 24);
    if (sync && _va_metadata_running)
    {
        VA_TP_OFFER(7);
        packet[n++] = VA_EVENT_TASK_SWITCH | VA_EVENT_FLAG_START_END;
        packet[n++] = (uint8_t)_va_seq++;
        packet[n++] = _va_metadata_running;
        n += _va_put_ts(packet + n, now);
    }
    _va_send_bytes(packet, n);
    _va_metadata_last_checkpoint = now;
    if (sync) _VA_METADATA.ackSequence = seq;
    return true;
}

static void _va_metadata_request(uint32_t request)
{
    __DMB();
    uint32_t generation = _VA_METADATA.generation;
    uint32_t status = (_VA_METADATA.flags != 0u) ? 2u :
                      (_VA_METADATA.requestGeneration != generation ? 1u : 0u);
    if (status == 0u && !_va_metadata_checkpoint(true))
        return;
    _VA_METADATA.ackGeneration = generation;
    _VA_METADATA.ackStatus = status;
    __DMB();
    _VA_METADATA.ack = request; /* ACK only this request, after publication. */
}
#endif

void _va_send_setup_packet(uint8_t setupCode, uint8_t id, const char *name)
{
    size_t name_len = strlen(name);
#if VA_MAX_TASK_NAME_LEN < VA_CONTROL_TEXT_MIN_CAPACITY
    /* Control messages require a larger limit when display names are small. */
    if (setupCode == VA_SETUP_INFO || setupCode == VA_SETUP_OS_INFO)
    {
        if (name_len >= VA_CONTROL_TEXT_CAPACITY)
            name_len = VA_CONTROL_TEXT_CAPACITY - 1;
    }
    else
#endif
    if (name_len >= VA_MAX_TASK_NAME_LEN)
        name_len = VA_MAX_TASK_NAME_LEN - 1;
    uint8_t buf[3 + VA_SEQ_BYTES + VA_SETUP_TEXT_CAPACITY];
    uint32_t p = 0;
    buf[p++] = setupCode;
    p += VA_SEQ_BYTES;
    buf[p++] = id;
    buf[p++] = (uint8_t)name_len;
    memcpy(&buf[p], name, name_len);
    _va_emit_packet(buf, p + (uint32_t)name_len);
}

#if VA_NEEDS_OBJECT_REGISTRY
/* Typed per-object info: [0x7E][seq?][id][kind][value(4)], no name string.
   The id is in the sync-object id space. */
static void _va_send_object_info_packet(uint8_t id, uint8_t kind, uint32_t value)
{
    uint8_t packet[3 + VA_SEQ_BYTES + 4];
    uint32_t p = 0;
    packet[p++] = VA_SETUP_OBJECT_INFO;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    packet[p++] = kind;
    packet[p++] = (uint8_t)(value >> 0);
    packet[p++] = (uint8_t)(value >> 8);
    packet[p++] = (uint8_t)(value >> 16);
    packet[p++] = (uint8_t)(value >> 24);
    _va_emit_packet(packet, p);
}
#endif

/* Typed flags packet: [0x77][seq?][group][value(4)], no name string. */
static void _va_send_config_flags_packet(uint8_t group, uint32_t value)
{
    uint8_t packet[2 + VA_SEQ_BYTES + 4];
    uint32_t p = 0;
    packet[p++] = VA_SETUP_CONFIG_FLAGS;
    p += VA_SEQ_BYTES;
    packet[p++] = group;
    packet[p++] = (uint8_t)(value >> 0);
    packet[p++] = (uint8_t)(value >> 8);
    packet[p++] = (uint8_t)(value >> 16);
    packet[p++] = (uint8_t)(value >> 24);
    _va_emit_packet(packet, p);
}

#if VA_NEEDS_USER_TRACE_REGISTRY
void _va_send_user_setup_packet(uint8_t id, uint8_t type, const char *name)
{
    size_t name_len = strlen(name);
    if (name_len >= VA_MAX_TASK_NAME_LEN)
    {
        name_len = VA_MAX_TASK_NAME_LEN - 1;
    }
    uint8_t buf[4 + VA_SEQ_BYTES + VA_MAX_TASK_NAME_LEN];
    uint32_t p = 0;
    buf[p++] = VA_SETUP_USER_TRACE;
    p += VA_SEQ_BYTES;
    buf[p++] = id;
    buf[p++] = type;
    buf[p++] = (uint8_t)name_len;
    memcpy(&buf[p], name, name_len);
    _va_emit_packet(buf, p + (uint32_t)name_len);
}
#endif /* VA_NEEDS_USER_TRACE_REGISTRY */

#if VA_TRACE_USER_VALUES
void _va_send_user_event_packet(uint8_t id, int32_t value, uint64_t timestamp)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 4];
    uint32_t p = 0;
    packet[p++] = VA_EVENT_USER_TRACE;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(value >> 0);
    packet[p++] = (uint8_t)(value >> 8);
    packet[p++] = (uint8_t)(value >> 16);
    packet[p++] = (uint8_t)(value >> 24);
    _va_emit_packet(packet, p);
}

void _va_send_float_event_packet(uint8_t id, float value, uint64_t timestamp)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 4];
    uint32_t fbits;
    uint32_t p = 0;
    memcpy(&fbits, &value, sizeof(fbits));
    packet[p++] = VA_EVENT_FLOAT_TRACE;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(fbits >> 0);
    packet[p++] = (uint8_t)(fbits >> 8);
    packet[p++] = (uint8_t)(fbits >> 16);
    packet[p++] = (uint8_t)(fbits >> 24);
    _va_emit_packet(packet, p);
}

void _va_send_user_toggle_event_packet(uint8_t id, VA_UserToggleState_t state, uint64_t timestamp)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 1];
    uint32_t p = 0;
    packet[p++] = VA_EVENT_USER_TOGGLE;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(state);
    _va_emit_packet(packet, p);
}
#endif /* VA_TRACE_USER_VALUES */

#if VA_HAS_RTOS && VA_TRACE_TASK_NOTIFICATIONS
void _va_send_notification_event_packet(uint8_t type_byte, uint8_t id, uint8_t other_id, uint32_t value, uint64_t timestamp)
{
    uint8_t packet[3 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 4];
    uint32_t p = 0;
    packet[p++] = type_byte;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    packet[p++] = other_id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(value >> 0);
    packet[p++] = (uint8_t)(value >> 8);
    packet[p++] = (uint8_t)(value >> 16);
    packet[p++] = (uint8_t)(value >> 24);
    _va_emit_packet(packet, p);
}
#endif /* VA_TRACE_TASK_NOTIFICATIONS */

#if VA_NEEDS_BLOCKING_HOOK
void _va_send_mutex_contention_packet(uint8_t mutex_id, uint8_t waiting_task_id, uint8_t holder_task_id, uint64_t timestamp)
{
    uint8_t packet[4 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES];
    uint32_t p = 0;
    packet[p++] = VA_EVENT_MUTEX_CONTENTION;
    p += VA_SEQ_BYTES;
    packet[p++] = mutex_id;
    packet[p++] = waiting_task_id;
    packet[p++] = holder_task_id;
    p += _va_put_ts(&packet[p], timestamp);
    _va_emit_packet(packet, p);
}
#endif /* VA_NEEDS_BLOCKING_HOOK */

#if VA_NEEDS_TASK_SWITCH_EVENTS
void _va_send_task_create_packet(uint8_t id, uint64_t timestamp, uint32_t priority, uint32_t base_priority, uint32_t stack_size)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 12];
    uint32_t p = 0;
    packet[p++] = VA_EVENT_TASK_CREATE;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(priority >> 0);
    packet[p++] = (uint8_t)(priority >> 8);
    packet[p++] = (uint8_t)(priority >> 16);
    packet[p++] = (uint8_t)(priority >> 24);
    packet[p++] = (uint8_t)(base_priority >> 0);
    packet[p++] = (uint8_t)(base_priority >> 8);
    packet[p++] = (uint8_t)(base_priority >> 16);
    packet[p++] = (uint8_t)(base_priority >> 24);
    packet[p++] = (uint8_t)(stack_size >> 0);
    packet[p++] = (uint8_t)(stack_size >> 8);
    packet[p++] = (uint8_t)(stack_size >> 16);
    packet[p++] = (uint8_t)(stack_size >> 24);
    _va_emit_packet(packet, p);
}
#endif /* VA_NEEDS_TASK_SWITCH_EVENTS */

#if VA_HAS_RTOS && VA_TRACE_STACK_USAGE
void _va_send_stack_usage_packet(uint8_t id, uint64_t timestamp, uint32_t stack_used, uint32_t stack_total)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 8];
    uint32_t p = 0;
    packet[p++] = VA_EVENT_TASK_STACK_USAGE;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(stack_used >> 0);
    packet[p++] = (uint8_t)(stack_used >> 8);
    packet[p++] = (uint8_t)(stack_used >> 16);
    packet[p++] = (uint8_t)(stack_used >> 24);
    packet[p++] = (uint8_t)(stack_total >> 0);
    packet[p++] = (uint8_t)(stack_total >> 8);
    packet[p++] = (uint8_t)(stack_total >> 16);
    packet[p++] = (uint8_t)(stack_total >> 24);
    _va_emit_packet(packet, p);
}
#endif /* VA_TRACE_STACK_USAGE */

void _va_send_data_event_packet(uint8_t type_byte, uint8_t id, uint32_t value, uint64_t timestamp)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 4];
    uint32_t p = 0;
    packet[p++] = type_byte;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(value >> 0);
    packet[p++] = (uint8_t)(value >> 8);
    packet[p++] = (uint8_t)(value >> 16);
    packet[p++] = (uint8_t)(value >> 24);
    _va_emit_packet(packet, p);
}

#if VA_TRACE_HEAP_METRICS
void _va_send_heap_setup_packet(uint8_t id, const char *name, uint32_t totalSize)
{
    size_t name_len = strlen(name);
    if (name_len >= VA_MAX_TASK_NAME_LEN)
    {
        name_len = VA_MAX_TASK_NAME_LEN - 1;
    }
    uint8_t buf[7 + VA_SEQ_BYTES + VA_MAX_TASK_NAME_LEN];
    uint32_t p = 0;
    buf[p++] = VA_SETUP_HEAP_INFO;
    p += VA_SEQ_BYTES;
    buf[p++] = id;
    buf[p++] = (uint8_t)(totalSize >> 0);
    buf[p++] = (uint8_t)(totalSize >> 8);
    buf[p++] = (uint8_t)(totalSize >> 16);
    buf[p++] = (uint8_t)(totalSize >> 24);
    buf[p++] = (uint8_t)name_len;
    memcpy(&buf[p], name, name_len);
    _va_emit_packet(buf, p + (uint32_t)name_len);
}
#endif /* heap setup packet */

#if VA_TRACE_STRINGS
static void _va_emit_string_event(uint8_t id, const char *msg)
{
    if (!msg) return;
    size_t len = strlen(msg);
    if (len > VA_MAX_LOG_STRING_LEN) len = VA_MAX_LOG_STRING_LEN;
    if (len == 0) return;

    _va_service_pending_bundle();

    VA_CS_ENTER();
    uint64_t ts = _va_get_timestamp_unlocked();

    uint8_t buf[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 2 + VA_MAX_LOG_STRING_LEN];
    uint32_t p = 0;
    buf[p++] = VA_EVENT_STRING_EVENT;
    p += VA_SEQ_BYTES;
    buf[p++] = id;
    p += _va_put_ts(&buf[p], ts);
    buf[p++] = (uint8_t)(len >> 0);
    buf[p++] = (uint8_t)(len >> 8);
    memcpy(&buf[p], msg, len);
    p += (uint32_t)len;

    _va_emit_packet(buf, p);
    VA_CS_EXIT();
}
#endif /* VA_TRACE_STRINGS */

/* ── Timestamp ───────────────────────────────────────────────────── */

/* Tick-source read + software 64-bit extension. MUST run with interrupts
   masked; _va_get_timestamp() wraps it for callers outside a CS. */
uint64_t _va_get_timestamp_unlocked(void)
{
    uint32_t current = _VA_TICK_READ();
    if (current < _va_ts_last)
    {
        _va_ts_ovf++;
    }
    _va_ts_last = current;
    return (((uint64_t)_va_ts_ovf) << VA_TS_SOURCE_BITS) | current;
}

uint64_t _va_get_timestamp(void)
{
#if VA_ALLOWED_TO_DISABLE_INTERRUPTS
    uint32_t primask_state = __get_PRIMASK();
    __disable_irq();
    uint64_t ts = _va_get_timestamp_unlocked();
    __set_PRIMASK(primask_state);
    return ts;
#else
    /* Single-execution-context contract (see the knob's comment): the
       unlocked read is safe, and masking would break the knob's promise. */
    return _va_get_timestamp_unlocked();
#endif
}

void VA_TickOverflowCheck(void)
{
    if (!VA_IS_INIT) return;
#if VA_METADATA
    VA_ATOMIC(
        _va_metadata_poll();
        uint64_t now = _va_get_timestamp_unlocked();
        if (now - _va_metadata_last_checkpoint >= _va_metadata_heartbeat)
            (void)_va_metadata_checkpoint(false);
    );
#elif VA_TRANSPORT_IS_ITM
    /* Re-arm a stalled ITM pipe so a host that starts draining SWO is
       picked up even with the periodic bundle disabled. At most once per
       second: each re-arm makes the next packet burn the full stall-spin
       limit with interrupts masked. */
    {
        static uint64_t _va_last_itm_rearm;
        uint64_t now = _va_get_timestamp();
        if ((now - _va_last_itm_rearm) >= (uint64_t)_va_tick_hz)
        {
            _va_last_itm_rearm = now;
            _va_itm_stalled = 0;
        }
    }
#else
    (void)_va_get_timestamp();
#endif
#if VA_BUNDLE_SERVICE
    /* Unmasked thread-context service for pending setup bundles. */
    if (!_va_in_isr() && !_va_irqs_masked())
    {
        uint64_t now = (((uint64_t)_va_ts_ovf) << VA_TS_SOURCE_BITS) | _va_ts_last;
        _va_bundle.service_ts = (now != 0u) ? now : 1u;
#if VA_RAMBUF_ATTACH_BUNDLE
        if (_va_rambuf_poll_host(now))
            _va_bundle.due = true;
#endif
        _va_emit_due_bundle();
    }
#endif
}

VA_BufferStats_t VA_GetBufferStats(void)
{
    VA_BufferStats_t stats = {0, 0, 0};
#if VA_TRANSPORT_BUFFERED
    VA_CS_ENTER();
    stats.queuedBytes = _va_ring_used();
    stats.droppedPackets = _va_dropped_packets;
    stats.droppedBytes = _va_dropped_bytes;
    VA_CS_EXIT();
#endif
    return stats;
}

#if VA_TRANSPORT_BUFFERED
/* Queue loss metadata only when it fits; caller holds the critical section. */
static void _va_report_buffer_drops(void)
{
    uint32_t dropped = _va_dropped_packets - _va_reported_drops;
    if (dropped == 0)
        return;
    char msg[16];
    _va_u32_to_str(msg, sizeof(msg), "DROP:", dropped);
    uint32_t len = (uint32_t)strlen(msg);
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 2 + 15];
    uint32_t size = 2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 2 + len;
#if VA_TRANSPORT_IS_CUSTOM
    uint32_t required = (uint32_t)va_cobs_max_encoded_len(size);
#else
    uint32_t required = size;
#endif
    if (required > VA_BUFFER_SIZE - _va_ring_used())
        return;
    uint32_t p = 0;
    packet[p++] = VA_EVENT_STRING_EVENT;
    p += VA_SEQ_BYTES;
    packet[p++] = 0;
    p += _va_put_ts(&packet[p], _va_get_timestamp_unlocked());
    packet[p++] = (uint8_t)len;
    packet[p++] = 0;
    memcpy(&packet[p], msg, len);
    _va_emit_packet(packet, size);
    _va_reported_drops = _va_dropped_packets;
}
#endif

void VA_Drain(void)
{
#if VA_TRANSPORT_BUFFERED
    if (!VA_IS_INIT || _va_in_isr() || _va_irqs_masked())
        return;
    uint32_t remaining;
    {
        VA_CS_ENTER();
        if (_va_drain_active)
        {
            VA_CS_EXIT();
            return;
        }
        _va_drain_active = true;
        remaining = _va_ring_used();
        if (remaining > VA_DRAIN_MAX_BYTES)
            remaining = VA_DRAIN_MAX_BYTES;
        VA_CS_EXIT();
    }
    /* Retain queued bytes until accepted; new writes cannot extend the budget. */
    while (remaining != 0)
    {
        uint32_t offset;
        uint32_t n = remaining < 64u ? remaining : 64u;
        {
            VA_CS_ENTER();
            offset = _va_ring_tail % VA_BUFFER_SIZE;
            uint32_t contiguous = VA_BUFFER_SIZE - offset;
            if (n > contiguous)
                n = contiguous;
            VA_CS_EXIT();
        }
        /* The unchanged tail reserves this span until the send returns. */
        uint32_t written = _va_send_bytes(&_va_ring[offset], n);
        {
            VA_CS_ENTER();
            _va_ring_tail += written;
            VA_CS_EXIT();
        }
        remaining -= n;
        if (written < n)
            break;
    }
    {
        VA_CS_ENTER();
        _va_report_buffer_drops();
        _va_drain_active = false;
        VA_CS_EXIT();
    }
#endif
}

void VA_EmitSetupBundle(void)
{
    if (!VA_IS_INIT)
        return;
#if VA_METADATA
    /* Names are maintained as they change. Explicit legacy service calls
       also stay bounded in this mode. */
    VA_ATOMIC(_va_metadata_poll());
    return;
#endif

#if VA_TRANSPORT_IS_ITM
    /* Re-arm a stalled ITM pipe: one bounded retry per bundle period. */
    _va_itm_stalled = 0;
#endif

    /* Each packet under its own short CS so interrupts are serviced between
       packets; the host only needs each individual packet to be atomic. */

    VA_ATOMIC(_va_emit_sync_marker());
    VA_ATOMIC(_va_send_seq_checkpoint());

    {
        char info_buf[40];
        _va_u32_to_str(info_buf, sizeof(info_buf), "CLK:", _va_tick_hz);
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, info_buf));
    }

#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS)
    VA_ATOMIC(_va_send_setup_packet(VA_SETUP_OS_INFO, 0, "FreeRTOS"));
#elif (VA_RTOS_SELECT == VA_RTOS_ZEPHYR)
    VA_ATOMIC(_va_send_setup_packet(VA_SETUP_OS_INFO, 0, "Zephyr"));
#else
    VA_ATOMIC(_va_send_setup_packet(VA_SETUP_OS_INFO, 0, "BareMetal"));
#endif

    /* Which categories this firmware was built with (for late attachers),
       plus the build flags and recorder version - the version rides the
       bundle too so a host that attaches mid-session still learns it. */
    VA_ATOMIC(_va_send_config_flags_packet(VA_FLAG_GROUP_CATEGORIES, VA_TRACE_CATEGORY_MASK));
    VA_ATOMIC(_va_send_config_flags_packet(VA_FLAG_GROUP_BUILD, VA_BUILD_FLAGS));
    VA_ATOMIC(_va_send_config_flags_packet(VA_FLAG_GROUP_VERSION, VA_RECORDER_VERSION_PACKED));

#if VA_NEEDS_TASK_REGISTRY
    for (int i = 0; i < VA_MAX_TASKS; ++i)
    {
        /* Snapshot the whole entry under ONE short CS: the slot can be
           deleted and recycled between this loop's per-packet critical
           sections, and the fields must all describe the same task. */
        bool     active;
        uint8_t  tid = 0;
        void    *handle = NULL;
        char     name[VA_MAX_TASK_NAME_LEN];
#if VA_NEEDS_TASK_SWITCH_EVENTS
        uint32_t prio = 0, basePrio = 0, stackDepth = 0;
#endif
        VA_CS_ENTER();
        active = taskMap[i].active;
        if (active)
        {
            tid    = taskMap[i].id;
            handle = taskMap[i].handle;
            memcpy(name, taskMap[i].name, sizeof(name));
#if VA_NEEDS_TASK_SWITCH_EVENTS
            prio       = taskMap[i].uxPriority;
            basePrio   = taskMap[i].uxBasePriority;
            stackDepth = taskMap[i].ulStackDepth;
#endif
        }
        VA_CS_EXIT();
        if (!active)
            continue;

        /* Name map goes out whenever the registry exists. */
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_TASK_MAP, tid, name));

#if VA_NEEDS_TASK_SWITCH_EVENTS
        VA_ATOMIC(_va_send_task_create_packet(tid, _va_get_timestamp_unlocked(),
                                              prio, basePrio, stackDepth));
#endif

#if VA_TRACE_STACK_USAGE
        /* Stack-usage snapshot for late-attaching hosts. The adapter call
           dereferences the live TCB: it must run under the same CS that
           re-validates the slot. */
        VA_ATOMIC(
            if (handle != NULL && taskMap[i].active && taskMap[i].handle == handle)
            {
                uint32_t su = va_adapter_calculate_stack_usage(handle);
                uint32_t st = va_adapter_get_total_stack_size(handle);
                if (st > 0)
                    _va_send_stack_usage_packet(tid, _va_get_timestamp_unlocked(), su, st);
            });
#else
        (void)handle;
#endif
    }
#endif /* VA_NEEDS_TASK_REGISTRY */

#if VA_NEEDS_OBJECT_REGISTRY
    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        /* Snapshot under one CS so a slot recycled mid-loop cannot emit
           the new object's name under the old object's id. */
        bool                 active;
        uint8_t              oid = 0;
        VA_QueueObjectType_t otype = VA_OBJECT_TYPE_QUEUE;
        void                *ohandle = NULL;
        char                 oname[VA_MAX_TASK_NAME_LEN];
#if VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS
        uint32_t             ocap = 0;
#endif
        VA_CS_ENTER();
        active = queueObjectMap[i].active;
        if (active)
        {
            oid     = queueObjectMap[i].id;
            otype   = queueObjectMap[i].type;
            ohandle = queueObjectMap[i].handle;
            memcpy(oname, queueObjectMap[i].name, sizeof(oname));
#if VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS
            ocap    = queueObjectMap[i].heapCapacity;
#endif
        }
        VA_CS_EXIT();
        if (!active)
            continue;

        VA_ATOMIC(_va_send_setup_packet(_va_get_setup_packet_type(otype), oid, oname));
        VA_ATOMIC(_va_send_object_info_packet(oid, VA_OBJINFO_OBJECT_TYPE, (uint32_t)otype));
#if VA_NEEDS_RTOS_OPERATIONS
        VA_ATOMIC(
            if (queueObjectMap[i].active && queueObjectMap[i].id == oid) {
                _va_send_object_info_packet(oid, VA_OBJINFO_CAPACITY, queueObjectMap[i].capacity);
                _va_send_object_info_packet(oid, VA_OBJINFO_ELEMENT_SIZE, queueObjectMap[i].elementSize);
            });
#endif
        VA_ATOMIC(_va_send_object_info_packet(oid, VA_OBJINFO_OBJECT_ADDR,
                                              (uint32_t)(uintptr_t)ohandle));
#if VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS
        /* Heap capacity anchors the host's 100% line; re-sent so a host
           that attached after registration still learns it. */
        if (otype == VA_OBJECT_TYPE_HEAP && ocap > 0)
            VA_ATOMIC(_va_send_object_info_packet(oid, VA_OBJINFO_HEAP_CAPACITY, ocap));
#endif
    }
#endif

    /* Re-emit user trace + user event maps for mid-run attachers. */
#if VA_NEEDS_USER_TRACE_REGISTRY
    for (int i = 0; i < VA_MAX_USER_TRACES; ++i)
    {
        if (userTraceMap[i].active)
        {
            if (userTraceMap[i].type == (uint8_t)VA_USER_TYPE_ISR)
                VA_ATOMIC(_va_send_setup_packet(VA_SETUP_ISR_MAP, userTraceMap[i].id,
                                                userTraceMap[i].name));
            else
                VA_ATOMIC(_va_send_user_setup_packet(userTraceMap[i].id, userTraceMap[i].type,
                                                     userTraceMap[i].name));
        }
    }
#endif
#if VA_TRACE_USER_EVENTS
    for (int i = 0; i < VA_MAX_USER_EVENTS; ++i)
    {
        if (userEventMap[i].active)
            VA_ATOMIC(_va_send_setup_packet(VA_SETUP_USER_EVENT_MAP, userEventMap[i].id,
                                            userEventMap[i].name));
    }
#endif
#if VA_TRACE_GPIO
    for (int i = 0; i < VA_MAX_GPIOS; ++i)
    {
        if (gpioMap[i].active)
            VA_ATOMIC(_va_send_setup_packet(VA_SETUP_GPIO_MAP, gpioMap[i].id, gpioMap[i].name));
    }
#endif
#if VA_TRACE_HEAP_METRICS
    for (int i = 0; i < VA_MAX_HEAPS; ++i)
    {
        if (heapGaugeMap[i].active)
            VA_ATOMIC(_va_send_heap_setup_packet(heapGaugeMap[i].id, heapGaugeMap[i].name,
                                                 heapGaugeMap[i].totalSize));
    }
#endif

    /* Re-announce latched registry-overflow conditions. */
#if VA_NEEDS_TASK_REGISTRY
    if (_va_task_map_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_TASKMAP_FULL));
#endif
#if VA_NEEDS_USER_TRACE_REGISTRY
    if (_va_user_trace_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_USERMAP_FULL));
#endif
#if VA_NEEDS_OBJECT_REGISTRY
    if (_va_obj_map_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_OBJMAP_FULL));
#endif
#if VA_TRACE_USER_EVENTS
    if (_va_user_event_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_EVTMAP_FULL));
#endif
#if VA_TRACE_GPIO
    if (_va_gpio_map_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_GPIOMAP_FULL));
#endif
#if VA_TRACE_HEAP_METRICS
    if (_va_heap_map_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_HEAPMAP_FULL));
#endif
}

#if VA_TS_IS_DWT || VA_TRANSPORT_IS_ITM
/* DEMCR.TRCENA powers both the DWT and the ITM; CYCCNT setup follows only
   when timestamps come from the DWT. Compiled out when neither consumer
   exists - baseline cores have none of these registers. */
static void _va_enable_trace_hw(void)
{
#if (__ARM_ARCH >= 8)
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
#if VA_TS_IS_DWT
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
}
#endif /* VA_TS_IS_DWT || VA_TRANSPORT_IS_ITM */

/* ── Generic task-map helpers ────────────────────────────────────── */
#if VA_NEEDS_TASK_REGISTRY

int _va_find_task_index(void *handle)
{
    if (handle == NULL)
        return -1;

    if (handle == _va_task_cache_handle
        && _va_task_cache_idx >= 0
        && taskMap[_va_task_cache_idx].active
        && taskMap[_va_task_cache_idx].handle == handle)
        return _va_task_cache_idx;

    for (int i = 0; i < VA_MAX_TASKS; ++i)
    {
        if (taskMap[i].active && taskMap[i].handle == handle)
        {
            _va_task_cache_handle = handle;
            _va_task_cache_idx    = i;
            return i;
        }
    }
    return -1;
}

uint8_t _va_find_task_id(void *handle)
{
    int idx = _va_find_task_index(handle);
    return idx >= 0 ? taskMap[idx].id : 0;
}

uint8_t _va_assign_task_id(void *handle, const char *name)
{
    if (handle == NULL || name == NULL)
        return 0;

    /* Already registered: keep the existing id (create hooks can race the
       lazy switch-in registration). */
    {
        int existing = _va_find_task_index(handle);
        if (existing >= 0)
            return taskMap[existing].id;
    }

    int empty_slot = -1;
    for (int i = 0; i < VA_MAX_TASKS; ++i)
    {
        if (!taskMap[i].active)
        {
            empty_slot = i;
            break;
        }
    }
    if (empty_slot == -1 || next_task_id == 0)
    {
        /* Registry full: this thread traces as id 0. Announce on the wire,
           latched for re-announcement in every bundle. */
        if (!_va_task_map_overflow)
        {
            _va_task_map_overflow = true;
            _va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_TASKMAP_FULL);
        }
        return 0;
    }

    uint8_t new_id = next_task_id++;
    taskMap[empty_slot].active = true;
    taskMap[empty_slot].handle = handle;
#if VA_TRACE_TASK_STATES
    taskMap[empty_slot].state = 0;
    taskMap[empty_slot].waitReason = 0;
    taskMap[empty_slot].waitObject = 0;
    taskMap[empty_slot].waitDetail = 0;
#endif
    taskMap[empty_slot].id = new_id;
#if VA_TRACE_TASK_NOTIFICATIONS
    taskMap[empty_slot].last_notifier = NULL;
#endif
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES
    taskMap[empty_slot].uxPriority = g_task_uxPriority;
    taskMap[empty_slot].uxBasePriority = g_task_uxBasePriority;
#endif
#if VA_TRACE_TASKS || VA_TRACE_TASK_STATES || VA_TRACE_STACK_USAGE
    taskMap[empty_slot].ulStackDepth = g_task_ulStackDepth;
#endif
#if VA_TRACE_STACK_USAGE
    taskMap[empty_slot].pxStack = (void *)g_task_pxStack;
    taskMap[empty_slot].pxEndOfStack = (void *)g_task_pxEndOfStack;
    taskMap[empty_slot].lastStackEmitTs = 0;
    taskMap[empty_slot].hasStackSample = false;
#endif
#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_SLEEP
    taskMap[empty_slot].sleeping = false;
#endif

    _va_copy_name(taskMap[empty_slot].name, name);

    /* Prime the MRU cache with the freshly assigned slot. */
    _va_task_cache_handle = handle;
    _va_task_cache_idx    = empty_slot;

    _va_send_setup_packet(VA_SETUP_TASK_MAP, new_id, taskMap[empty_slot].name);
    return new_id;
}

/* Free a task's registry slot. The id is not reused; earlier events keep
   referring to it and the host keeps the name from previous bundles. */
static void _va_release_task(int idx)
{
    if (idx < 0)
        return;
#if VA_METADATA
    if (_va_metadata_running == taskMap[idx].id) _va_metadata_running = 0;
    _va_metadata_remove(taskMap[idx].id, true);
#endif
    taskMap[idx].active = false;
    taskMap[idx].handle = NULL;
    if (_va_task_cache_idx == idx)
    {
        _va_task_cache_handle = NULL;
        _va_task_cache_idx    = -1;
    }
}

#endif /* VA_NEEDS_TASK_REGISTRY */

/* ── Queue / sync-object map helpers ─────────────────────────────── */
#if VA_NEEDS_OBJECT_REGISTRY

const char *_va_get_object_type_name(VA_QueueObjectType_t type)
{
    switch (type)
    {
    case VA_OBJECT_TYPE_QUEUE:           return "Queue";
    case VA_OBJECT_TYPE_MUTEX:           return "Mutex";
    case VA_OBJECT_TYPE_COUNTING_SEM:    return "CountingSem";
    case VA_OBJECT_TYPE_BINARY_SEM:      return "BinarySem";
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX: return "RecursiveMutex";
    case VA_OBJECT_TYPE_EVENTFLAG:       return "EventFlag";
    case VA_OBJECT_TYPE_TIMER:           return "Timer";
    case VA_OBJECT_TYPE_HEAP:            return "Heap";
    case VA_OBJECT_TYPE_POWER_MGMT:      return "PowerMgmt";
    case VA_OBJECT_TYPE_STREAM_BUFFER:   return "StreamBuffer";
    case VA_OBJECT_TYPE_MESSAGE_BUFFER:  return "MessageBuffer";
    case VA_OBJECT_TYPE_MEM_SLAB:        return "MemorySlab";
    case VA_OBJECT_TYPE_CONDVAR:         return "Condvar";
    case VA_OBJECT_TYPE_POLL_SIGNAL:     return "PollSignal";
    default:                             return "Unknown";
    }
}

uint8_t _va_get_setup_packet_type(VA_QueueObjectType_t type)
{
    switch (type)
    {
    case VA_OBJECT_TYPE_QUEUE:
    /* Event flags ride the queue map; the "EventFlag" name suffix carries
       the type to the host (old hosts degrade to showing a queue). */
    case VA_OBJECT_TYPE_EVENTFLAG:
        return VA_SETUP_QUEUE_MAP;
    case VA_OBJECT_TYPE_MUTEX:
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX:
        return VA_SETUP_MUTEX_MAP;
    case VA_OBJECT_TYPE_COUNTING_SEM:
    case VA_OBJECT_TYPE_BINARY_SEM:
        return VA_SETUP_SEMAPHORE_MAP;
    case VA_OBJECT_TYPE_TIMER:
        return VA_SETUP_TIMER_MAP;
    case VA_OBJECT_TYPE_HEAP:
        return VA_SETUP_HEAP_MAP;
    case VA_OBJECT_TYPE_POWER_MGMT:
        return VA_SETUP_PM_MAP;
    default:
        return VA_SETUP_QUEUE_MAP;
    }
}

/* One-entry MRU cache (same rationale as the task cache). */
static void *_va_qobj_cache_handle = NULL;
static int   _va_qobj_cache_idx    = -1;

int _va_find_queue_object_index(void *handle)
{
    if (handle == NULL)
        return -1;

    if (handle == _va_qobj_cache_handle
        && _va_qobj_cache_idx >= 0
        && queueObjectMap[_va_qobj_cache_idx].active
        && queueObjectMap[_va_qobj_cache_idx].handle == handle)
        return _va_qobj_cache_idx;

    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        if (queueObjectMap[i].active && queueObjectMap[i].handle == handle)
        {
            _va_qobj_cache_handle = handle;
            _va_qobj_cache_idx    = i;
            return i;
        }
    }
    return -1;
}

uint8_t _va_find_queue_object_id(void *handle)
{
    int idx = _va_find_queue_object_index(handle);
    return idx >= 0 ? queueObjectMap[idx].id : 0;
}

VA_QueueObjectType_t _va_get_stored_queue_object_type(void *handle)
{
    int idx = _va_find_queue_object_index(handle);
    if (idx >= 0)
        return queueObjectMap[idx].type;
    return va_adapter_get_queue_object_type(handle);
}

uint8_t _va_assign_queue_object_id(void *handle, const char *name, VA_QueueObjectType_t type)
{
    if (handle == NULL)
        return 0;

    /* Already registered: keep the existing id (a thread's first-touch
       registration can race an ISR touching the same object). */
    {
        int existing = _va_find_queue_object_index(handle);
        if (existing >= 0)
            return queueObjectMap[existing].id;
    }

    int empty_slot = -1;
    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        if (!queueObjectMap[i].active)
        {
            empty_slot = i;
            break;
        }
    }
    if (empty_slot == -1 || next_queue_object_id == 0)
    {
        /* Registry full or id space exhausted: this object traces as id 0.
           Announce on the wire, latched for re-announcement in every
           bundle. */
        if (!_va_obj_map_overflow)
        {
            _va_obj_map_overflow = true;
            _va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_OBJMAP_FULL);
        }
        return 0;
    }

    uint8_t new_id = next_queue_object_id++;
    queueObjectMap[empty_slot].active = true;
    queueObjectMap[empty_slot].handle = handle;
    queueObjectMap[empty_slot].id = new_id;
    queueObjectMap[empty_slot].type = type;
#if VA_NEEDS_RTOS_OPERATIONS
    queueObjectMap[empty_slot].capacity = 0;
    queueObjectMap[empty_slot].elementSize = 0;
#endif
#if VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS
    queueObjectMap[empty_slot].heapCapacity = 0;
#endif

    /* Prime the MRU cache with the freshly assigned slot. */
    _va_qobj_cache_handle = handle;
    _va_qobj_cache_idx    = empty_slot;

    if (name && strlen(name) > 0)
    {
        _va_copy_name(queueObjectMap[empty_slot].name, name);
    }
    else
    {
        _va_copy_name(queueObjectMap[empty_slot].name, _va_get_object_type_name(type));
    }

    _va_send_setup_packet(_va_get_setup_packet_type(type), new_id, queueObjectMap[empty_slot].name);
    _va_send_object_info_packet(new_id, VA_OBJINFO_OBJECT_TYPE, (uint32_t)type);
    /* The handle address lets hosts name statically-defined objects from
       the ELF (heap-allocated handles simply resolve to nothing). */
    _va_send_object_info_packet(new_id, VA_OBJINFO_OBJECT_ADDR,
                                (uint32_t)(uintptr_t)handle);
    return new_id;
}

/* Does this object type still produce give/take events in this build?
   Every arm folds to a constant. */
static inline bool _va_type_emits_events(VA_QueueObjectType_t type)
{
    switch (type)
    {
    case VA_OBJECT_TYPE_MUTEX:
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX: return (VA_TRACE_MUTEXES != 0);
    case VA_OBJECT_TYPE_COUNTING_SEM:
    case VA_OBJECT_TYPE_BINARY_SEM:      return (VA_TRACE_SEMAPHORES != 0);
    case VA_OBJECT_TYPE_TIMER:           return (VA_TRACE_TIMERS != 0);
    case VA_OBJECT_TYPE_HEAP:            return (VA_TRACE_RTOS_HEAPS != 0);
    case VA_OBJECT_TYPE_POWER_MGMT:      return (VA_TRACE_PM != 0);
    case VA_OBJECT_TYPE_EVENTFLAG:       return (VA_TRACE_EVENT_FLAGS != 0);
    case VA_OBJECT_TYPE_STREAM_BUFFER:
    case VA_OBJECT_TYPE_MESSAGE_BUFFER:  return (VA_TRACE_STREAM_BUFFERS != 0);
    case VA_OBJECT_TYPE_MEM_SLAB:        return (VA_TRACE_MEM_SLABS != 0);
    case VA_OBJECT_TYPE_CONDVAR:         return (VA_TRACE_CONDVARS != 0);
    case VA_OBJECT_TYPE_POLL_SIGNAL:     return (VA_TRACE_POLL != 0);
    case VA_OBJECT_TYPE_QUEUE:
    default:                             return (VA_TRACE_QUEUES != 0);
    }
}

/* Mutexes need a slot even with VA_TRACE_MUTEXES off: the contention
   packet references the mutex by object id. */
static inline bool _va_type_needs_registry(VA_QueueObjectType_t type)
{
#if VA_TRACE_TASK_STATES
    (void)type;
    return true;
#endif
    if (type == VA_OBJECT_TYPE_MUTEX || type == VA_OBJECT_TYPE_RECURSIVE_MUTEX)
        return (VA_TRACE_MUTEXES != 0) || (VA_TRACE_MUTEX_CONTENTION != 0);
    return _va_type_emits_events(type);
}

/* Release a slot when an object turns out to belong to an untraced
   category (FreeRTOS corrects queue->mutex one call after creation). */
static void _va_release_queue_object(int idx)
{
    if (idx < 0)
        return;
#if VA_METADATA
    _va_metadata_remove(queueObjectMap[idx].id, false);
#endif
    queueObjectMap[idx].active = false;
    queueObjectMap[idx].handle = NULL;
    if (_va_qobj_cache_idx == idx)
    {
        _va_qobj_cache_handle = NULL;
        _va_qobj_cache_idx    = -1;
    }
}

/* Event code for an object type. */
static inline uint8_t _va_event_type_for_object(VA_QueueObjectType_t type)
{
    switch (type)
    {
    case VA_OBJECT_TYPE_MUTEX:
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX: return VA_EVENT_MUTEX;
    case VA_OBJECT_TYPE_COUNTING_SEM:
    case VA_OBJECT_TYPE_BINARY_SEM:      return VA_EVENT_SEMAPHORE;
    case VA_OBJECT_TYPE_TIMER:           return VA_EVENT_TIMER;
    case VA_OBJECT_TYPE_POWER_MGMT:      return VA_EVENT_PM_SUSPEND;
    case VA_OBJECT_TYPE_EVENTFLAG:       return VA_EVENT_EVENTFLAG;
    case VA_OBJECT_TYPE_QUEUE:
    default:                             return VA_EVENT_QUEUE;
    }
}

#endif /* VA_NEEDS_OBJECT_REGISTRY */

/* ── User-event map (RTOS-independent) ───────────────────────────── */
#if VA_TRACE_USER_EVENTS

static uint8_t _va_find_user_event_id(uint8_t event_id)
{
    for (int i = 0; i < VA_MAX_USER_EVENTS; ++i)
    {
        if (userEventMap[i].active && userEventMap[i].id == event_id)
        {
            return userEventMap[i].id;
        }
    }
    return 0;
}

static uint8_t _va_assign_user_event_id(uint8_t event_id, const char *name)
{
    if (name == NULL || event_id == 0)
        return 0;

    if (_va_find_user_event_id(event_id) != 0)
        return event_id;

    int empty_slot = -1;
    for (int i = 0; i < VA_MAX_USER_EVENTS; ++i)
    {
        if (!userEventMap[i].active)
        {
            empty_slot = i;
            break;
        }
    }
    if (empty_slot == -1)
    {
        if (!_va_user_event_overflow)
        {
            _va_user_event_overflow = true;
            _va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_EVTMAP_FULL);
        }
        return 0;
    }

    userEventMap[empty_slot].active = true;
    userEventMap[empty_slot].id = event_id;
    _va_copy_name(userEventMap[empty_slot].name, name);

    _va_send_setup_packet(VA_SETUP_USER_EVENT_MAP, event_id, userEventMap[empty_slot].name);
    return event_id;
}

#endif /* VA_TRACE_USER_EVENTS */

/* ── RTOS task-event hooks (generic - delegate to adapter for OS specifics) ─── */

#if VA_NEEDS_TASK_REGISTRY
void va_taskcreated(void *taskHandle, const char *name)
{
    VA_CS_ENTER();
    uint8_t assigned_id = _va_assign_task_id(taskHandle, name ? name : "???");
#if VA_NEEDS_TASK_SWITCH_EVENTS
    if (assigned_id > 0)
    {
        uint64_t timestamp = _va_get_timestamp();
        _va_send_task_create_packet(assigned_id, timestamp,
                                     g_task_uxPriority, g_task_uxBasePriority, g_task_ulStackDepth);
    }
#else
    /* Registry only: the TASK_MAP packet still goes out (other packets
       reference tasks by id). */
    VA_UNUSED(assigned_id);
#endif
    VA_CS_EXIT();
}

void va_taskdeleted(void *taskHandle)
{
    if (taskHandle == NULL)
        return;
    VA_CS_ENTER();
    va_logTaskState(taskHandle, VA_TASK_DELETED);
    _va_release_task(_va_find_task_index(taskHandle));
    VA_CS_EXIT();
}

/* Update a registered task's name and re-emit its name map (a task that
   gets its name after creation, e.g. Zephyr k_thread_name_set). Unknown
   handles are ignored; the switch-in path registers them with the new
   name instead. */
void va_taskrenamed(void *taskHandle, const char *name)
{
    if (taskHandle == NULL || name == NULL || name[0] == '\0')
        return;
    VA_CS_ENTER();
    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0)
    {
        _va_copy_name(taskMap[idx].name, name);
        _va_send_setup_packet(VA_SETUP_TASK_MAP, taskMap[idx].id, taskMap[idx].name);
    }
    VA_CS_EXIT();
}
#endif /* VA_NEEDS_TASK_REGISTRY */

#if VA_NEEDS_SWITCH_HOOK
void va_taskswitchedin(void *taskHandle)
{
    va_logTaskState(taskHandle, VA_TASK_RUNNING);
#if VA_NEEDS_TASK_SWITCH_EVENTS
    /* Scheduler (PendSV/ISR) context - no bundle service here. */
    VA_CS_ENTER();
    uint8_t id = _va_find_task_id(taskHandle);
    _va_send_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_TASK_SWITCH, id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
#else
    /* Stack-usage-only build: switch-in has nothing to do. */
    VA_UNUSED(taskHandle);
#endif
}

void va_taskswitchedout(void *taskHandle)
{
    VA_CS_ENTER();
#if VA_TRACE_STACK_USAGE
    int      idx = _va_find_task_index(taskHandle);
    uint8_t  id  = idx >= 0 ? taskMap[idx].id : 0;
#elif VA_NEEDS_TASK_SWITCH_EVENTS
    uint8_t  id  = _va_find_task_id(taskHandle);
#endif
    uint64_t now = _va_get_timestamp_unlocked();

#if VA_NEEDS_TASK_SWITCH_EVENTS
    _va_send_event_packet(VA_EVENT_TASK_SWITCH, id, now);
#endif

#if VA_TRACE_TASK_STATES
    VA_ATOMIC(
        int state_idx = _va_find_task_index(taskHandle);
        if (state_idx >= 0 && taskMap[state_idx].state == VA_TASK_RUNNING)
            va_logTaskState(taskHandle, VA_TASK_READY);
    );
#endif

#if VA_TRACE_STACK_USAGE
    /* The high-water scan walks the task's free stack, so the heartbeat
       gates the SCAN, not just the emission - this hook runs on every
       context switch with interrupts masked. */
    if (id != 0 && idx >= 0)
    {
        bool sample = (_va_stack_heartbeat_ticks == 0)
                    || !taskMap[idx].hasStackSample
                    || (now - taskMap[idx].lastStackEmitTs) >= _va_stack_heartbeat_ticks;
        if (sample)
        {
            uint32_t stack_used  = va_adapter_calculate_stack_usage(taskHandle);
            uint32_t stack_total = va_adapter_get_total_stack_size(taskHandle);
            if (stack_total > 0)
            {
#if VA_METADATA
                /* A requested checkpoint may have followed switch-out. Do
                   not reuse a timestamp from before that attach boundary. */
                now = _va_get_timestamp_unlocked();
#endif
                _va_send_stack_usage_packet(id, now, stack_used, stack_total);
                taskMap[idx].lastStackEmitTs = now;
                taskMap[idx].hasStackSample  = true;
            }
        }
    }
#else
    VA_UNUSED(now);
    VA_UNUSED(taskHandle);
#endif
    VA_CS_EXIT();
}
#endif /* VA_NEEDS_SWITCH_HOOK */

/* ── ISR logging (RTOS-independent) ──────────────────────────────── */
#if VA_TRACE_ISRS

void VA_LogISRStart(uint8_t isrId)
{
    VA_CS_ENTER();
    if (!VA_IS_INIT)
    {
        VA_CS_EXIT();
        return;
    }
    _va_send_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_ISR, isrId, _va_get_timestamp());
    VA_CS_EXIT();
}

void VA_LogISREnd(uint8_t isrId)
{
    VA_CS_ENTER();
    if (!VA_IS_INIT)
    {
        VA_CS_EXIT();
        return;
    }
    _va_send_event_packet(VA_EVENT_ISR, isrId, _va_get_timestamp());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_ISRS */

bool VA_IsInit(void)
{
    return VA_IS_INIT;
}

/* ── User-trace / data logging (RTOS-independent) ────────────────── */

#if VA_NEEDS_USER_TRACE_REGISTRY
/* Parenthesised name: the header defines a same-named guard macro. */
void (VA_RegisterUserTrace)(uint8_t id, const char *name, VA_UserTraceType_t type)
{
    VA_CS_ENTER();
    if (id == 0 || name == NULL)
    {
        VA_CS_EXIT();
        return;
    }

    /* Store for periodic re-emission; on overflow announce it on the wire. */
    int slot = -1;
    for (int i = 0; i < VA_MAX_USER_TRACES; ++i)
    {
        if (userTraceMap[i].active && userTraceMap[i].id == id) { slot = i; break; }
        if (slot < 0 && !userTraceMap[i].active) slot = i;
    }
    if (slot >= 0)
    {
        userTraceMap[slot].active = true;
        userTraceMap[slot].id = id;
        userTraceMap[slot].type = (uint8_t)type;
        _va_copy_name(userTraceMap[slot].name, name);
    }
    else if (!_va_user_trace_overflow)
    {
        _va_user_trace_overflow = true;
        _va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_USERMAP_FULL);
    }

    if (type == VA_USER_TYPE_ISR)
        _va_send_setup_packet(VA_SETUP_ISR_MAP, id, name);
    else
        _va_send_user_setup_packet(id, (uint8_t)type, name);
    VA_CS_EXIT();
}
#endif /* VA_NEEDS_USER_TRACE_REGISTRY */

#if VA_TRACE_USER_VALUES
void VA_LogTrace(uint8_t id, int32_t value)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_user_event_packet(id, value, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void VA_LogTraceFloat(uint8_t id, float value)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_float_event_packet(id, value, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void VA_LogToggle(uint8_t id, bool state)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_user_toggle_event_packet(id, state, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}
#endif /* VA_TRACE_USER_VALUES */

#if VA_TRACE_STRINGS
void VA_LogString(uint8_t id, const char *msg)
{
    _va_emit_string_event(id, msg);
}
#endif

#if VA_TRACE_GPIO
void VA_LogGPIO(uint8_t id, bool state)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_data_event_packet(VA_EVENT_GPIO, id, (uint32_t)state, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void VA_RegisterGPIO(uint8_t id, const char *name)
{
    VA_CS_ENTER();
    if (id == 0 || name == NULL)
    {
        VA_CS_EXIT();
        return;
    }

    /* Store for periodic re-emission (late-attaching hosts). */
    int slot = -1;
    for (int i = 0; i < VA_MAX_GPIOS; ++i)
    {
        if (gpioMap[i].active && gpioMap[i].id == id) { slot = i; break; }
        if (slot < 0 && !gpioMap[i].active) slot = i;
    }
    if (slot >= 0)
    {
        gpioMap[slot].active = true;
        gpioMap[slot].id = id;
        _va_copy_name(gpioMap[slot].name, name);
    }
    else if (!_va_gpio_map_overflow)
    {
        /* The name goes out once below but cannot be stored, so the
           periodic bundle will not carry it. Announce that. */
        _va_gpio_map_overflow = true;
        _va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_GPIOMAP_FULL);
    }

    _va_send_setup_packet(VA_SETUP_GPIO_MAP, id, name);
    VA_CS_EXIT();
}
#endif /* VA_TRACE_GPIO */

#if VA_TRACE_COUNTERS
void VA_LogCounter(uint8_t id, uint32_t value)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_data_event_packet(VA_EVENT_COUNTER, id, value, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}
#endif

#if VA_TRACE_HEAP_METRICS
void VA_LogHeap(uint8_t id, uint32_t usedBytes)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_data_event_packet(VA_EVENT_HEAP, id, usedBytes, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void VA_RegisterHeap(uint8_t id, const char *name, uint32_t totalSize)
{
    VA_CS_ENTER();
    if (id == 0 || name == NULL)
    {
        VA_CS_EXIT();
        return;
    }

    /* Store for periodic re-emission (late-attaching hosts). */
    int slot = -1;
    for (int i = 0; i < VA_MAX_HEAPS; ++i)
    {
        if (heapGaugeMap[i].active && heapGaugeMap[i].id == id) { slot = i; break; }
        if (slot < 0 && !heapGaugeMap[i].active) slot = i;
    }
    if (slot >= 0)
    {
        heapGaugeMap[slot].active = true;
        heapGaugeMap[slot].id = id;
        heapGaugeMap[slot].totalSize = totalSize;
        _va_copy_name(heapGaugeMap[slot].name, name);
    }
    else if (!_va_heap_map_overflow)
    {
        /* The name goes out once below but cannot be stored, so the
           periodic bundle will not carry it. Announce that. */
        _va_heap_map_overflow = true;
        _va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_HEAPMAP_FULL);
    }

    _va_send_heap_setup_packet(id, name, totalSize);
    VA_CS_EXIT();
}
#endif /* VA_TRACE_HEAP_METRICS */

/* ── Sleep enter/exit (k_sleep, k_msleep, k_usleep) ──────────────── */
#if VA_HAS_RTOS && VA_TRACE_SLEEP

void va_logSleepEnter(void *taskHandle)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_task_id(taskHandle);
    if (id != 0)
        _va_send_event_packet(VA_EVENT_SLEEP | VA_EVENT_FLAG_START_END, id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void va_logSleepExit(void *taskHandle)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_task_id(taskHandle);
    if (id != 0)
        _va_send_event_packet(VA_EVENT_SLEEP, id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_SLEEP */

/* ── PM (power management) suspend enter/exit ────────────────────── */
#if VA_HAS_RTOS && VA_TRACE_PM

/* Unique, stable sentinel handle for PM events. */
static uint8_t _va_pm_sentinel;

void va_logPMSuspendEnter(void)
{
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(&_va_pm_sentinel);
    if (id == 0)
        id = _va_assign_queue_object_id(&_va_pm_sentinel, "__va_pm__", VA_OBJECT_TYPE_POWER_MGMT);
    if (id != 0)
        _va_send_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_PM_SUSPEND, id, _va_get_timestamp());
    VA_CS_EXIT();
}

void va_logPMSuspendExit(uint8_t state)
{
    (void)state;
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(&_va_pm_sentinel);
    if (id != 0)
        _va_send_event_packet(VA_EVENT_PM_SUSPEND, id, _va_get_timestamp());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_PM */

/* ── Task notification hooks ─────────────────────────────────────── */
#if VA_HAS_RTOS && VA_TRACE_TASK_NOTIFICATIONS

void va_logtasknotifygive(void *srcHandle, void *destHandle, uint32_t value)
{
    VA_CS_ENTER();
    uint8_t src_id = _va_find_task_id(srcHandle);
    uint8_t dest_id = _va_find_task_id(destHandle);

    int idx = _va_find_task_index(destHandle);
    if (idx >= 0)
    {
        taskMap[idx].last_notifier = srcHandle;
    }

    _va_send_notification_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_TASK_NOTIFY,
                                        src_id, dest_id, value, _va_get_timestamp());
    VA_CS_EXIT();
}

void va_logtasknotifytake(void *taskHandle, uint32_t value)
{
    VA_CS_ENTER();
    uint8_t dest_id = _va_find_task_id(taskHandle);
    void *src = NULL;

    int idx = _va_find_task_index(taskHandle);
    if (idx >= 0)
    {
        src = taskMap[idx].last_notifier;
        taskMap[idx].last_notifier = NULL;
    }

    uint8_t src_id = _va_find_task_id(src);

    _va_send_notification_event_packet(VA_EVENT_TASK_NOTIFY,
                                        dest_id, src_id, value, _va_get_timestamp());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_TASK_NOTIFICATIONS */

/* ── Queue / sync-object event hooks ─────────────────────────────── */
#if VA_NEEDS_OBJECT_REGISTRY

void va_logQueueObjectCreate(void *queueObject, const char *name)
{
    va_logQueueObjectCreateWithType(queueObject, name);
}

void va_logQueueObjectDelete(void *queueObject)
{
    if (queueObject == NULL)
        return;
    VA_CS_ENTER();
    _va_release_queue_object(_va_find_queue_object_index(queueObject));
    VA_CS_EXIT();
}

void va_updateQueueObjectType(void *queueObject, const char *typeHint)
{
    if (queueObject == NULL)
        return;

    VA_CS_ENTER();

    int idx = _va_find_queue_object_index(queueObject);

    if (idx >= 0)
    {
        VA_QueueObjectType_t type = VA_OBJECT_TYPE_QUEUE;

        if (typeHint != NULL)
        {
            if (strstr(typeHint, "RecMutex") != NULL || strstr(typeHint, "RecursiveMutex") != NULL)
                type = VA_OBJECT_TYPE_RECURSIVE_MUTEX;
            else if (strstr(typeHint, "Mutex") != NULL)
                type = VA_OBJECT_TYPE_MUTEX;
            else if (strstr(typeHint, "CountSem") != NULL || strstr(typeHint, "CountingSem") != NULL)
                type = VA_OBJECT_TYPE_COUNTING_SEM;
            else if (strstr(typeHint, "BinSem") != NULL || strstr(typeHint, "BinarySem") != NULL)
                type = VA_OBJECT_TYPE_BINARY_SEM;
            else if (strstr(typeHint, "Timer") != NULL)
                type = VA_OBJECT_TYPE_TIMER;
            else if (strstr(typeHint, "Heap") != NULL)
                type = VA_OBJECT_TYPE_HEAP;
            else if (strstr(typeHint, "Semaphore") != NULL || strstr(typeHint, "Sem") != NULL)
                type = VA_OBJECT_TYPE_COUNTING_SEM;
            else if (strstr(typeHint, "EvtFlag") != NULL || strstr(typeHint, "EventFlag") != NULL)
                type = VA_OBJECT_TYPE_EVENTFLAG;
        }

        /* Corrected type belongs to an untraced category: give the slot
           back. */
        if (!_va_type_needs_registry(type))
        {
            _va_release_queue_object(idx);
            VA_CS_EXIT();
            return;
        }

        queueObjectMap[idx].type = type;

        _va_send_object_info_packet(queueObjectMap[idx].id, VA_OBJINFO_OBJECT_TYPE, (uint32_t)type);

        char descriptiveName[VA_MAX_TASK_NAME_LEN];
        const char *finalName = NULL;

        if (typeHint != NULL && strlen(typeHint) > 0)
        {
            finalName = typeHint;
            switch (type)
            {
            case VA_OBJECT_TYPE_MUTEX:
                if (strstr(typeHint, "Mutex") == NULL)
                {
                    _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "Mutex");
                    finalName = descriptiveName;
                }
                break;
            case VA_OBJECT_TYPE_RECURSIVE_MUTEX:
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "RecMutex");
                finalName = descriptiveName;
                break;
            default:
                break;
            }
        }

        /* Fall back to the type name if no hint was provided */
        if (finalName == NULL)
            finalName = _va_get_object_type_name(type);

        _va_copy_name(queueObjectMap[idx].name, finalName);

        _va_send_setup_packet(_va_get_setup_packet_type(type), queueObjectMap[idx].id, queueObjectMap[idx].name);
    }

    VA_CS_EXIT();
}

void va_logQueueObjectCreateWithType(void *queueObject, const char *typeHint)
{
    if (queueObject == NULL)
        return;

#if VA_NEEDS_SYNC_PREFILTER
    /* Never register an object whose category this build does not trace. */
    if (!_va_type_needs_registry(va_adapter_get_queue_object_type(queueObject)))
        return;
#endif

    VA_CS_ENTER();
    VA_QueueObjectType_t type = va_adapter_get_queue_object_type(queueObject);

    /* Adapter returned the default (QUEUE): infer from typeHint (Zephyr
       cannot classify a bare handle). */
    if (type == VA_OBJECT_TYPE_QUEUE && typeHint != NULL)
    {
        if (strstr(typeHint, "RecMutex") != NULL || strstr(typeHint, "RecursiveMutex") != NULL)
            type = VA_OBJECT_TYPE_RECURSIVE_MUTEX;
        else if (strstr(typeHint, "Mutex") != NULL)
            type = VA_OBJECT_TYPE_MUTEX;
        else if (strstr(typeHint, "CountSem") != NULL || strstr(typeHint, "CountingSem") != NULL)
            type = VA_OBJECT_TYPE_COUNTING_SEM;
        else if (strstr(typeHint, "BinSem") != NULL || strstr(typeHint, "BinarySem") != NULL)
            type = VA_OBJECT_TYPE_BINARY_SEM;
        else if (strstr(typeHint, "Semaphore") != NULL || strstr(typeHint, "Sem") != NULL)
            type = VA_OBJECT_TYPE_COUNTING_SEM;
        else if (strstr(typeHint, "Timer") != NULL)
            type = VA_OBJECT_TYPE_TIMER;
        else if (strstr(typeHint, "Heap") != NULL)
            type = VA_OBJECT_TYPE_HEAP;
        else if (strstr(typeHint, "EvtFlag") != NULL || strstr(typeHint, "EventFlag") != NULL)
            type = VA_OBJECT_TYPE_EVENTFLAG;
    }

    char descriptiveName[VA_MAX_TASK_NAME_LEN];
    const char *finalName = typeHint;

    if (typeHint != NULL && strlen(typeHint) > 0)
    {
        switch (type)
        {
        case VA_OBJECT_TYPE_QUEUE:
            if (strstr(typeHint, "Queue") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "Queue");
                finalName = descriptiveName;
            }
            break;
        case VA_OBJECT_TYPE_MUTEX:
            if (strstr(typeHint, "Mutex") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "Mutex");
                finalName = descriptiveName;
            }
            break;
        case VA_OBJECT_TYPE_RECURSIVE_MUTEX:
            if (strstr(typeHint, "RecMutex") == NULL && strstr(typeHint, "RecursiveMutex") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "RecMutex");
                finalName = descriptiveName;
            }
            break;
        case VA_OBJECT_TYPE_COUNTING_SEM:
            if (strstr(typeHint, "Sem") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "CountSem");
                finalName = descriptiveName;
            }
            break;
        case VA_OBJECT_TYPE_BINARY_SEM:
            if (strstr(typeHint, "Sem") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "BinSem");
                finalName = descriptiveName;
            }
            break;
        case VA_OBJECT_TYPE_TIMER:
            if (strstr(typeHint, "Timer") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "Timer");
                finalName = descriptiveName;
            }
            break;
        case VA_OBJECT_TYPE_HEAP:
            if (strstr(typeHint, "Heap") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "Heap");
                finalName = descriptiveName;
            }
            break;
        case VA_OBJECT_TYPE_EVENTFLAG:
            if (strstr(typeHint, "EvtFlag") == NULL && strstr(typeHint, "EventFlag") == NULL)
            {
                _va_strcat_suffix(descriptiveName, sizeof(descriptiveName), typeHint, "EvtFlag");
                finalName = descriptiveName;
            }
            break;
        default:
            break;
        }
    }

    _va_assign_queue_object_id(queueObject, finalName, type);
    VA_CS_EXIT();
}

/* User-assigned object name (FreeRTOS vQueueAddToRegistry). Replaces any
   auto-generated name verbatim and re-emits the name map. */
void va_logQueueObjectSetName(void *queueObject, const char *name)
{
    if (queueObject == NULL || name == NULL || name[0] == '\0')
        return;

    VA_CS_ENTER();
    int idx = _va_find_queue_object_index(queueObject);
    if (idx >= 0)
    {
        _va_copy_name(queueObjectMap[idx].name, name);
        _va_send_setup_packet(_va_get_setup_packet_type(queueObjectMap[idx].type),
                              queueObjectMap[idx].id, queueObjectMap[idx].name);
    }
    else
    {
        VA_QueueObjectType_t type = va_adapter_get_queue_object_type(queueObject);
        if (_va_type_needs_registry(type))
            _va_assign_queue_object_id(queueObject, name, type);
    }
    VA_CS_EXIT();
}

/* The pre-filter runs FIRST, before the bundle service and CS entry, which
   are the expensive part of this path. */

void va_logQueueObjectGive(void *queueObject, uint32_t timeout)
{
    VA_UNUSED(timeout);
    if (queueObject == NULL)
        return;

#if VA_NEEDS_SYNC_PREFILTER
    if (!_va_type_emits_events(va_adapter_get_queue_object_type(queueObject)))
        return;
#endif

    _va_service_pending_bundle();
    VA_CS_ENTER();
    int idx = _va_find_queue_object_index(queueObject);
    uint8_t id = idx >= 0 ? queueObjectMap[idx].id : 0;
    if (id == 0)
    {
        VA_QueueObjectType_t new_type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, new_type);
        idx = _va_find_queue_object_index(queueObject);
    }

    /* Authoritative check on the STORED type (the only gate on Zephyr). */
    VA_QueueObjectType_t type = idx >= 0 ? queueObjectMap[idx].type
                                       : va_adapter_get_queue_object_type(queueObject);
    if (_va_type_emits_events(type))
        _va_send_event_packet(VA_EVENT_FLAG_START_END | _va_event_type_for_object(type),
                              id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void va_logQueueObjectTake(void *queueObject, uint32_t timeout)
{
    VA_UNUSED(timeout);
    if (queueObject == NULL)
        return;

#if VA_NEEDS_SYNC_PREFILTER
    if (!_va_type_emits_events(va_adapter_get_queue_object_type(queueObject)))
        return;
#endif

    _va_service_pending_bundle();
    VA_CS_ENTER();
    int idx = _va_find_queue_object_index(queueObject);
    uint8_t id = idx >= 0 ? queueObjectMap[idx].id : 0;
    if (id == 0)
    {
        VA_QueueObjectType_t new_type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, new_type);
        idx = _va_find_queue_object_index(queueObject);
    }

    VA_QueueObjectType_t type = idx >= 0 ? queueObjectMap[idx].type
                                       : va_adapter_get_queue_object_type(queueObject);
    if (_va_type_emits_events(type))
        _va_send_event_packet(_va_event_type_for_object(type), id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

/* ── Typed object entry points ───────────────────────────────────────
   For objects whose native handle the adapter cannot classify (FreeRTOS
   software timers, event groups, heap sentinels). The caller-supplied type
   routes both registration and event emission; va_adapter_get_queue_object_type
   is never consulted, so these are safe for non-Queue_t handles. */

void va_logQueueObjectCreateTyped(void *queueObject, const char *name, VA_QueueObjectType_t type)
{
    if (queueObject == NULL || !_va_type_needs_registry(type))
        return;

    VA_CS_ENTER();
    int idx = _va_find_queue_object_index(queueObject);
    if (idx >= 0)
    {
        /* Repeated create at a live slot: refresh the type and name. */
        queueObjectMap[idx].type = type;
        if (name != NULL && name[0] != '\0')
        {
            _va_copy_name(queueObjectMap[idx].name, name);
            _va_send_setup_packet(_va_get_setup_packet_type(type),
                                  queueObjectMap[idx].id, queueObjectMap[idx].name);
        }
        _va_send_object_info_packet(queueObjectMap[idx].id, VA_OBJINFO_OBJECT_TYPE, (uint32_t)type);
    }
    else
    {
        _va_assign_queue_object_id(queueObject,
                                   (name != NULL && name[0] != '\0') ? name : NULL,
                                   type);
    }
    VA_CS_EXIT();
}

void va_logQueueObjectGiveTyped(void *queueObject, VA_QueueObjectType_t type)
{
    if (queueObject == NULL || !_va_type_emits_events(type))
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
        id = _va_assign_queue_object_id(queueObject, NULL, type);
    _va_send_event_packet(VA_EVENT_FLAG_START_END | _va_event_type_for_object(type),
                          id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void va_logQueueObjectTakeTyped(void *queueObject, VA_QueueObjectType_t type)
{
    if (queueObject == NULL || !_va_type_emits_events(type))
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
        id = _va_assign_queue_object_id(queueObject, NULL, type);
    _va_send_event_packet(_va_event_type_for_object(type), id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

/* ── Failed operations ───────────────────────────────────────────────
   One wire event covers every sync-object failure: timeout, no space, no
   data. Follows the object's own category, so a build that traces queues
   also sees their failures - no extra knob. */

void va_logObjectOpFailedTyped(void *queueObject, VA_QueueObjectType_t type,
                               bool giveSide, uint32_t detail)
{
    if (queueObject == NULL || !_va_type_emits_events(type))
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
        id = _va_assign_queue_object_id(queueObject, NULL, type);
    _va_send_data_event_packet(giveSide ? (VA_EVENT_FLAG_START_END | VA_EVENT_OP_FAILED)
                                        : VA_EVENT_OP_FAILED,
                               id, detail, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void va_logQueueObjectOpFailed(void *queueObject, bool giveSide, uint32_t detail)
{
    if (queueObject == NULL)
        return;

#if VA_NEEDS_SYNC_PREFILTER
    if (!_va_type_emits_events(va_adapter_get_queue_object_type(queueObject)))
        return;
#endif

    VA_CS_ENTER();
    VA_QueueObjectType_t type = _va_get_stored_queue_object_type(queueObject);
    VA_CS_EXIT();
    va_logObjectOpFailedTyped(queueObject, type, giveSide, detail);
}

#endif /* VA_NEEDS_OBJECT_REGISTRY */

/* ── Event flags (FreeRTOS event groups / Zephyr k_event) ────────── */
#if VA_HAS_RTOS && VA_TRACE_EVENT_FLAGS

void va_logEventFlagSet(void *flagObject, uint32_t bits)
{
    if (flagObject == NULL)
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(flagObject);
    if (id == 0)
        id = _va_assign_queue_object_id(flagObject, NULL, VA_OBJECT_TYPE_EVENTFLAG);
    _va_send_data_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_EVENTFLAG,
                               id, bits, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void va_logEventFlagWaitEnd(void *flagObject, uint32_t bits)
{
    if (flagObject == NULL)
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(flagObject);
    if (id == 0)
        id = _va_assign_queue_object_id(flagObject, NULL, VA_OBJECT_TYPE_EVENTFLAG);
    _va_send_data_event_packet(VA_EVENT_EVENTFLAG, id, bits, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_EVENT_FLAGS */

/* ── Two-value events (timer arm, deferred work) ─────────────────── */
#if VA_HAS_RTOS && (VA_TRACE_TIMERS || VA_TRACE_WORK || VA_TRACE_TASK_STATES)

/* [type][seq?][id][ts][v1(4)][v2(4)], both values little-endian. */
static void _va_send_dual_u32_packet(uint8_t type_byte, uint8_t id,
                                     uint32_t v1, uint32_t v2, uint64_t timestamp)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 8];
    uint32_t p = 0;
    packet[p++] = type_byte;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], timestamp);
    packet[p++] = (uint8_t)(v1 >> 0);
    packet[p++] = (uint8_t)(v1 >> 8);
    packet[p++] = (uint8_t)(v1 >> 16);
    packet[p++] = (uint8_t)(v1 >> 24);
    packet[p++] = (uint8_t)(v2 >> 0);
    packet[p++] = (uint8_t)(v2 >> 8);
    packet[p++] = (uint8_t)(v2 >> 16);
    packet[p++] = (uint8_t)(v2 >> 24);
    _va_emit_packet(packet, p);
}

#endif /* VA_TRACE_TIMERS || VA_TRACE_WORK */

#if VA_HAS_RTOS && (VA_TRACE_TASK_STATES || VA_NEEDS_RTOS_OPERATIONS || VA_HAS_NOTIFICATION_DETAILS)
static void _va_send_rtos_packet(uint8_t code, uint8_t id, uint32_t a, uint32_t b, uint32_t c)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES + 12];
    uint32_t p = 0;
    packet[p++] = code;
    p += VA_SEQ_BYTES;
    packet[p++] = id;
    p += _va_put_ts(&packet[p], _va_get_timestamp_unlocked());
    const uint32_t values[3] = {a, b, c};
    for (unsigned v = 0; v < 3; ++v)
        for (unsigned shift = 0; shift < 32; shift += 8)
            packet[p++] = (uint8_t)(values[v] >> shift);
    _va_emit_packet(packet, p);
}
#endif

#if VA_HAS_RTOS && VA_TRACE_TASK_STATES
void va_logTaskState(void *task, VA_TaskState_t state)
{
    if (!VA_IS_INIT || task == NULL) return;
    VA_CS_ENTER();
    int idx = _va_find_task_index(task);
    if (idx >= 0) {
        VA_TaskMapEntry_t *entry = &taskMap[idx];
        /* Generic pend must not erase a known sleep/suspend reason. */
        if (state == VA_TASK_BLOCKED &&
            (entry->state == VA_TASK_SLEEPING || entry->state == VA_TASK_SUSPENDED)) {
            VA_CS_EXIT();
            return;
        }
        if (entry->state != state && state != VA_TASK_RUNNING) {
            uint32_t reason = state == VA_TASK_BLOCKED ? entry->waitReason : 0;
            uint32_t object = state == VA_TASK_BLOCKED ? entry->waitObject : 0;
            _va_send_dual_u32_packet(VA_EVENT_TASK_STATE, entry->id,
                (uint32_t)state | (reason << 8) | (object << 16),
                state == VA_TASK_BLOCKED ? entry->waitDetail : 0,
                _va_get_timestamp_unlocked());
        }
        entry->state = (uint8_t)state;
        /* Wait metadata survives READY until the operation returns: stream
           buffers may internally wait on a notification more than once. */
    }
    VA_CS_EXIT();
}

void va_logTaskWait(void *task, VA_WaitReason_t reason, void *object,
                    VA_QueueObjectType_t type, uint32_t detail)
{
    if (!VA_IS_INIT || task == NULL) return;
    VA_CS_ENTER();
    int idx = _va_find_task_index(task);
    if (idx >= 0) {
        VA_TaskMapEntry_t *entry = &taskMap[idx];
        /* FreeRTOS implements buffer waits using notifications. Preserve
           the outer operation as the user-visible reason. */
        if (!(reason == VA_WAIT_NOTIFICATION &&
              (entry->waitReason == VA_WAIT_STREAM_SEND || entry->waitReason == VA_WAIT_STREAM_RECEIVE))) {
            entry->waitReason = (uint8_t)reason;
            entry->waitObject = object != NULL ? _va_assign_queue_object_id(object, NULL, type) : 0;
            entry->waitDetail = detail;
        }
    }
    VA_CS_EXIT();
}

void va_clearTaskWait(void *task)
{
    if (!VA_IS_INIT) return;
    VA_CS_ENTER();
    int idx = _va_find_task_index(task);
    if (idx >= 0) {
        taskMap[idx].waitReason = 0;
        taskMap[idx].waitObject = 0;
        taskMap[idx].waitDetail = 0;
    }
    VA_CS_EXIT();
}

void va_logTaskPriority(void *task, int32_t effective, int32_t base, uint32_t cause)
{
    if (!VA_IS_INIT) return;
    VA_CS_ENTER();
    int idx = _va_find_task_index(task);
    if (idx >= 0) {
        taskMap[idx].uxPriority = (uint32_t)effective;
        taskMap[idx].uxBasePriority = (uint32_t)base;
        _va_send_rtos_packet(VA_EVENT_TASK_PRIORITY, taskMap[idx].id,
            (uint32_t)effective, taskMap[idx].uxBasePriority, cause);
    }
    VA_CS_EXIT();
}
#endif

#if VA_HAS_NOTIFICATION_DETAILS
void va_logNotifyDetail(void *destination, void *sender, uint16_t exception,
                        uint8_t operation, uint32_t value, uint32_t index)
{
    if (!VA_IS_INIT || destination == NULL) return;
    VA_CS_ENTER();
    uint8_t id = _va_find_task_id(destination);
    uint8_t source = exception == 0 ? _va_find_task_id(sender) : 0;
    if (id != 0)
        _va_send_rtos_packet(VA_EVENT_NOTIFY_DETAILS, id, (uint32_t)operation |
            ((uint32_t)source << 8) | ((uint32_t)exception << 16), value, index);
    VA_CS_EXIT();
}
#endif

#if VA_NEEDS_RTOS_OPERATIONS
void va_logRtosObjectInfo(void *object, VA_QueueObjectType_t type,
                          uint32_t capacity, uint32_t elementSize)
{
    if (!VA_IS_INIT || object == NULL || !_va_type_emits_events(type)) return;
    VA_CS_ENTER();
    uint8_t id = _va_assign_queue_object_id(object, NULL, type);
    int idx = _va_find_queue_object_index(object);
    if (idx >= 0) {
        if (queueObjectMap[idx].capacity != capacity) {
            queueObjectMap[idx].capacity = capacity;
            _va_send_object_info_packet(id, VA_OBJINFO_CAPACITY, capacity);
        }
        if (queueObjectMap[idx].elementSize != elementSize) {
            queueObjectMap[idx].elementSize = elementSize;
            _va_send_object_info_packet(id, VA_OBJINFO_ELEMENT_SIZE, elementSize);
        }
    }
    VA_CS_EXIT();
}

void va_logRtosOperation(void *object, VA_QueueObjectType_t type, uint8_t event,
                         VA_RtosOperation_t operation, uint32_t value, uint32_t detail,
                         void *task, uint16_t exception)
{
    if (!VA_IS_INIT || !_va_type_emits_events(type)) return;
    VA_CS_ENTER();
    uint8_t taskId = exception == 0 ? _va_find_task_id(task) : 0;
    /* Poll waits belong to a task, not an ephemeral stack-allocated array. */
    uint8_t id = (event == VA_EVENT_POLL &&
                  (operation == VA_OP_WAIT_BEGIN || operation == VA_OP_WAIT_END))
        ? taskId : _va_assign_queue_object_id(object, NULL, type);
    if (id != 0)
        _va_send_rtos_packet(event, id, (uint32_t)operation | ((uint32_t)taskId << 8) |
                             ((uint32_t)exception << 16), value, detail);
    VA_CS_EXIT();
}
#endif

#if VA_HAS_RTOS && VA_TRACE_TIMERS && VA_TRACE_TIMER_CALLBACKS
void va_logTimerCallback(void *timer, bool enter, bool stop, void *handler)
{
    if (!VA_IS_INIT || timer == NULL) return;
    VA_CS_ENTER();
    uint8_t id = _va_assign_queue_object_id(timer, NULL, VA_OBJECT_TYPE_TIMER);
    if (id != 0)
        _va_send_dual_u32_packet(VA_EVENT_TIMER_CALLBACK | (enter ? VA_EVENT_FLAG_START_END : 0),
            id, stop ? 1 : 0, (uint32_t)(uintptr_t)handler, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}
#endif

/* ── Timer arm (duration/period payload) ─────────────────────────── */
#if VA_HAS_RTOS && VA_TRACE_TIMERS

void va_logTimerArm(void *timerObject, uint32_t durationMs, uint32_t periodMs)
{
    if (timerObject == NULL)
        return;
    /* Clamp so a "never" timeout survives the signed db columns. */
    if (durationMs > 0x7FFFFFFFu)
        durationMs = 0x7FFFFFFFu;
    if (periodMs > 0x7FFFFFFFu)
        periodMs = 0x7FFFFFFFu;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(timerObject);
    if (id == 0)
        id = _va_assign_queue_object_id(timerObject, NULL, VA_OBJECT_TYPE_TIMER);
    _va_send_dual_u32_packet(VA_EVENT_TIMER_ARM, id, durationMs, periodMs,
                             _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_TIMERS */

/* ── Deferred work (Zephyr k_work family) ────────────────────────── */
#if VA_HAS_RTOS && VA_TRACE_WORK

/* Work items consume no object ids (id byte 0); the handler address is
   the identity and the host symbolicates it from the ELF. */

void va_logWorkArm(void *handler, uint32_t delayMs)
{
    if (handler == NULL)
        return;
    /* Clamp so a "never" delay survives the signed db column. */
    if (delayMs > 0x7FFFFFFFu)
        delayMs = 0x7FFFFFFFu;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_dual_u32_packet(VA_EVENT_FLAG_START_END | VA_EVENT_WORK, 0,
                             (uint32_t)(uintptr_t)handler, delayMs,
                             _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void va_logWorkCancel(void *handler)
{
    if (handler == NULL)
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_dual_u32_packet(VA_EVENT_WORK, 0, (uint32_t)(uintptr_t)handler, 0,
                             _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_WORK */

#if VA_NEEDS_BLOCKING_HOOK
/* Registers the object even when VA_TRACE_MUTEXES is off: the contention
   packet needs the mutex and both tasks in the registries. */
void va_logQueueObjectBlocking(void *queueObject)
{
    if (queueObject == NULL)
        return;

#if VA_ADAPTER_CLASSIFIES_OBJECTS
    {
        VA_QueueObjectType_t probe = va_adapter_get_queue_object_type(queueObject);
        if (probe != VA_OBJECT_TYPE_MUTEX && probe != VA_OBJECT_TYPE_RECURSIVE_MUTEX)
            return;   /* only mutexes can be contended */
    }
#endif

    _va_service_pending_bundle();
    VA_CS_ENTER();

    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
    {
        VA_QueueObjectType_t new_type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, new_type);
    }

    VA_QueueObjectType_t type = _va_get_stored_queue_object_type(queueObject);

    if (type == VA_OBJECT_TYPE_MUTEX || type == VA_OBJECT_TYPE_RECURSIVE_MUTEX)
    {
        va_adapter_check_mutex_contention(queueObject, id);
    }

    VA_CS_EXIT();
}
#endif /* VA_NEEDS_BLOCKING_HOOK */

/* ── Heap alloc / free tracing (RTOS allocator) ──────────────────── */
#if VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS

void va_logHeapAllocFailed(void *heapObject, uint32_t requestedBytes)
{
    if (heapObject == NULL)
        return;

    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(heapObject);
    if (id == 0)
    {
        id = _va_assign_queue_object_id(heapObject, NULL, VA_OBJECT_TYPE_HEAP);
    }
    _va_send_data_event_packet(VA_EVENT_HEAP_FAIL, id, requestedBytes,
                               _va_get_timestamp());
    VA_CS_EXIT();
}

void va_logHeapCapacity(void *heapObject, const char *name, uint32_t totalSize)
{
    if (heapObject == NULL || name == NULL || totalSize == 0)
        return;

    VA_CS_ENTER();
    int idx = _va_find_queue_object_index(heapObject);
    if (idx < 0)
    {
        _va_assign_queue_object_id(heapObject, name, VA_OBJECT_TYPE_HEAP);
        idx = _va_find_queue_object_index(heapObject);
    }
    if (idx >= 0)
    {
        queueObjectMap[idx].heapCapacity = totalSize;
        /* Capacity travels in the sync-object id space (VA_SETUP_HEAP_INFO
           belongs to the manual heap gauges, whose ids can collide). */
        _va_send_object_info_packet(queueObjectMap[idx].id,
                                    VA_OBJINFO_HEAP_CAPACITY, totalSize);
    }
    VA_CS_EXIT();
}

void va_logHeapAlloc(void *heapObject, uint32_t allocBytes)
{
    if (heapObject == NULL)
        return;

    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(heapObject);
    if (id == 0)
    {
        id = _va_assign_queue_object_id(heapObject, NULL, VA_OBJECT_TYPE_HEAP);
    }
    _va_send_data_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_HEAP_SYNC,
                               id, allocBytes, _va_get_timestamp());
    VA_CS_EXIT();
}

void va_logHeapFree(void *heapObject, uint32_t allocatedBytes)
{
    if (heapObject == NULL)
        return;

    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(heapObject);
    if (id == 0)
    {
        id = _va_assign_queue_object_id(heapObject, NULL, VA_OBJECT_TYPE_HEAP);
    }
    _va_send_data_event_packet(VA_EVENT_HEAP_SYNC,
                               id, allocatedBytes, _va_get_timestamp());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_RTOS_HEAPS */

/* ── User Event Logging ──────────────────────────────────────────── */
#if VA_TRACE_USER_EVENTS

/* Parenthesised name: the header defines a same-named guard macro. */
void (VA_RegisterUserEvent)(uint8_t id, const char *name)
{
    VA_CS_ENTER();
    if (id == 0 || name == NULL)
    {
        VA_CS_EXIT();
        return;
    }
    _va_assign_user_event_id(id, name);
    VA_CS_EXIT();
}

void VA_LogEvent(uint8_t id, bool state)
{
    if (id == 0)
        return;
    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t event_flags = (state == USER_EVENT_START) ? (VA_EVENT_FLAG_START_END | VA_EVENT_USER_EVENT) : VA_EVENT_USER_EVENT;
    _va_send_event_packet(event_flags, id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

#endif /* VA_TRACE_USER_EVENTS */

/* ── Initialization ──────────────────────────────────────────────── */

#if VA_TRANSPORT_IS_CUSTOM
void VA_RegisterTransportSend(VA_TransportSendFn sendFn)
{
    s_user_send_fn = sendFn;
}
#endif

#if VA_TS_IS_DWT
/* Function-shaped CYCCNT read so the DWT arm can share the liveness probe. */
static uint32_t _va_read_cyccnt(void)
{
    return DWT->CYCCNT;
}
#endif

/* Bounded spin until the tick source shows movement. Must run with
   interrupts ENABLED: the dead-source case burns the full bound (~0.1 s).
   A healthy source exits within one tick period; the bound gives sources
   down to ~1 kHz several periods. */
static bool _va_tick_source_alive(VA_TimestampFn fn, uint32_t cpu_freq)
{
    const uint32_t first = fn() & VA_TS_SOURCE_MASK;
    uint32_t spins = (cpu_freq != 0u) ? (cpu_freq >> 6) : 1000000u;
    while (spins-- != 0u)
    {
        if ((fn() & VA_TS_SOURCE_MASK) != first)
            return true;
    }
    return false;
}

#if VA_TS_IS_CUSTOM
void VA_Init(uint32_t cpu_freq, VA_TimestampFn ts_fn, uint32_t tick_hz)
#else
void VA_Init(uint32_t cpu_freq)
#endif
{
    /* ── Tick-source verdict, decided before interrupts are masked ─────
       Both arms are probed for movement: the arch check only proves the
       architecture CAN have a cycle counter; a vendor-omitted DWT enables
       fine and never counts. A dead verdict is reported (and the recorder
       refused) once the transport is up. */
#if VA_TS_IS_DWT || VA_TRANSPORT_IS_ITM
    _va_enable_trace_hw();
#endif
    const char *ts_err = NULL;
#if VA_TS_IS_CUSTOM
    if (ts_fn == 0)
        ts_err = VA_INFO_TS_NULL;
    else if (tick_hz == 0u)
        ts_err = VA_INFO_TS_HZ_ZERO;
    else if (!_va_tick_source_alive(ts_fn, cpu_freq))
        ts_err = VA_INFO_TS_DEAD;
#else
    if (!_va_tick_source_alive(_va_read_cyccnt, cpu_freq))
        ts_err = VA_INFO_TS_DEAD;
#endif

    VA_CS_ENTER();
#if VA_TS_IS_CUSTOM
    /* Tick math and the reported CLK: rate come from the timer, not the
       CPU clock. A rejected source is never installed - trace calls must
       keep hitting the stub. */
    _va_ts_fn   = (ts_err == NULL) ? ts_fn : _va_ts_none;
    _va_tick_hz = tick_hz;
#else
    _va_tick_hz = cpu_freq;
#endif
    _va_ts_ovf  = 0;
    _va_ts_last = 0;

#if VA_NEEDS_TASK_REGISTRY
    /* Reset MRU lookup caches (stale slot indices from a previous run). */
    _va_task_cache_handle = NULL;
    _va_task_cache_idx    = -1;
#endif

    _va_seq = 0;
#if VA_METADATA
    _VA_METADATA.magic[0] = 0;
    __DMB();
    memset(&_VA_METADATA, 0, sizeof(_VA_METADATA));
    _VA_METADATA.version = 1;
    _VA_METADATA.tableAddr = (uint32_t)(uintptr_t)_va_metadata_bytes;
    _VA_METADATA.capacity = VA_METADATA_SIZE;
    _va_metadata_running = 0;
    _va_metadata_last_checkpoint = 0;
    /* At most two seconds, always below half a 32-bit wire-clock wrap. */
    _va_metadata_heartbeat = 2u * (uint64_t)_va_tick_hz;
    if (_va_metadata_heartbeat > 0x7fffffffu) _va_metadata_heartbeat = 0x7fffffffu;
#endif

#if VA_BUNDLE_SERVICE
    _va_bundle.last_ts    = 0;
    _va_bundle.emitting   = false;
    _va_bundle.due        = false;
    _va_bundle.service_ts = 0;
    /* Bundle interval in ticks; the 64-bit extended-timestamp compare in
       _va_emit_packet needs no wrap clamp. */
    _va_bundle.interval = ((uint64_t)_va_tick_hz / 1000) * VA_AUTO_SETUP_INTERVAL_MS;
    /* Hooks defer to a thread-context VA_TickOverflowCheck() caller seen
       within two bundle periods (two seconds with the periodic bundle off). */
    _va_bundle.defer_ticks = (_va_bundle.interval != 0u)
                               ? 2u * _va_bundle.interval
                               : 2u * (uint64_t)_va_tick_hz;
#endif
#if VA_RAMBUF_ATTACH_BUNDLE
    _va_rambuf_seen_rd     = 0;      /* the control block below starts at 0 */
    _va_rambuf_consume_ts  = 0;
    _va_rambuf_host_active = false;
    _va_rambuf_idle_ticks  = ((uint64_t)_va_tick_hz / 1000) * VA_RAMBUF_HOST_IDLE_MS;
#endif

#if VA_HAS_RTOS && VA_TRACE_STACK_USAGE
    _va_stack_heartbeat_ticks = ((uint64_t)_va_tick_hz / 1000) * VA_STACK_USAGE_HEARTBEAT_MS;
#endif

#if VA_TRANSPORT_BUFFERED
    _va_ring_head = 0;
    _va_ring_tail = 0;
    _va_dropped_packets = 0;
    _va_dropped_bytes = 0;
    _va_reported_drops = 0;
    _va_drain_active = false;
#endif

#if defined(VA_TP_TEST) && (VA_TP_TEST == 1)
    _VA_TP.offeredPackets = 0;
    _VA_TP.offeredBytes   = 0;
    _VA_TP.droppedPackets = 0;
    _VA_TP.droppedBytes   = 0;
    for (int i = 0; i < 8; ++i) _VA_TP.magic[i] = VA_TP_MAGIC[i];
#endif

#if VA_NEEDS_TASK_REGISTRY
    for (int i = 0; i < VA_MAX_TASKS; ++i)
    {
        taskMap[i].active = false;
        taskMap[i].handle = NULL;
        taskMap[i].id = 0;
#if VA_TRACE_TASK_NOTIFICATIONS
        taskMap[i].last_notifier = NULL;
#endif
#if VA_TRACE_STACK_USAGE
        taskMap[i].lastStackEmitTs = 0;
        taskMap[i].hasStackSample = false;
#endif
#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_SLEEP
        taskMap[i].sleeping = false;
#endif
    }
    next_task_id = 1;
    _va_task_map_overflow = false;
#endif

#if VA_NEEDS_OBJECT_REGISTRY
    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        queueObjectMap[i].active = false;
        queueObjectMap[i].handle = NULL;
        queueObjectMap[i].id = 0;
    }
    next_queue_object_id = 1;
    _va_obj_map_overflow = false;
    _va_qobj_cache_handle = NULL;
    _va_qobj_cache_idx    = -1;
#endif

#if VA_TRACE_USER_EVENTS
    for (int i = 0; i < VA_MAX_USER_EVENTS; ++i)
    {
        userEventMap[i].active = false;
        userEventMap[i].id = 0;
        userEventMap[i].name[0] = '\0';
    }
    _va_user_event_overflow = false;
#endif

#if VA_TRACE_GPIO
    for (int i = 0; i < VA_MAX_GPIOS; ++i)
    {
        gpioMap[i].active = false;
        gpioMap[i].id = 0;
        gpioMap[i].name[0] = '\0';
    }
    _va_gpio_map_overflow = false;
#endif

#if VA_TRACE_HEAP_METRICS
    for (int i = 0; i < VA_MAX_HEAPS; ++i)
    {
        heapGaugeMap[i].active = false;
        heapGaugeMap[i].id = 0;
        heapGaugeMap[i].totalSize = 0;
        heapGaugeMap[i].name[0] = '\0';
    }
    _va_heap_map_overflow = false;
#endif

#if VA_NEEDS_USER_TRACE_REGISTRY
    for (int i = 0; i < VA_MAX_USER_TRACES; ++i)
    {
        userTraceMap[i].active = false;
        userTraceMap[i].id = 0;
        userTraceMap[i].name[0] = '\0';
    }
    _va_user_trace_overflow = false;
#endif

#if VA_TRANSPORT_IS_ITM
    /* CoreSight LAR unlock: ARMv7-M only (removed on ARMv8-M, and CMSIS-6
       drops the field, so writing it would not compile). */
#if !defined(__ARM_ARCH_8M_MAIN__) && !defined(__ARM_ARCH_8M_BASE__) && !defined(__ARM_ARCH_8_1M_MAIN__)
    ITM->LAR = 0xC5ACCE55;
#endif
    ITM->TCR |= ITM_TCR_ITMENA_Msk;
    ITM->TER |= (1UL << VA_ITM_PORT);
    _va_itm_stalled = 0;
#elif VA_TRANSPORT_IS_JLINK
#if (VA_CONFIGURE_RTT == 1)
        SEGGER_RTT_Init();
    #if VA_RTT_BUFFER_SIZE > 0
        SEGGER_RTT_ConfigUpBuffer(VA_RTT_CHANNEL, "ViewAlyzer", s_va_rtt_up_buffer, sizeof(s_va_rtt_up_buffer), VA_RTT_MODE);
    #else
        SEGGER_RTT_ConfigUpBuffer(VA_RTT_CHANNEL, "ViewAlyzer", NULL, 0, VA_RTT_MODE);
    #endif /* VA_RTT_BUFFER_SIZE > 0 */
#endif /* VA_CONFIGURE_RTT */
#elif VA_TRANSPORT_IS_CUSTOM
    /* Nothing to init - user provides send function via VA_RegisterTransportSend() */
#elif VA_TRANSPORT_IS_RAMBUF && !VA_PM_VIA_TRANSPORT
    _VA_RAMBUF.magic[0] = '\0';   /* invalidate while (re)initialising */
    __DMB();
    _VA_RAMBUF.bufferAddr     = (uint32_t)(uintptr_t)&s_va_rambuf_storage[0];
    _VA_RAMBUF.bufferSize     = (uint32_t)VA_RAMBUF_SIZE;
    _VA_RAMBUF.wrOff          = 0;
    _VA_RAMBUF.rdOff          = 0;
    _VA_RAMBUF.droppedPackets = 0;
    _VA_RAMBUF.flags          = VA_RAMBUF_MODE;
#if VA_METADATA
    _VA_RAMBUF.flags |= 0x100u; /* ELF-assisted metadata protocol required */
#endif
    _VA_RAMBUF.cpuFreqHz      = _va_tick_hz;
    _VA_RAMBUF.wireVersion    = (uint8_t)VA_WIRE_VERSION;
    _VA_RAMBUF.tsBytes        = (uint8_t)VA_TIMESTAMP_BYTES;
    _VA_RAMBUF.recorderVersion = (uint16_t)((VA_RECORDER_VERSION_MAJOR << 8)
                                           | VA_RECORDER_VERSION_MINOR);
    __DMB();
    /* Magic written last and backwards, so a scanning host can never match a
       partially initialised control block. */
    for (int i = 15; i >= 0; i--)
        _VA_RAMBUF.magic[i] = VA_RAMBUF_MAGIC[i];
    __DMB();
#endif /* VA_TRANSPORT */

#if VA_PM_RING
    _VA_PMBUF.magic[0] = '\0';    /* invalidate while (re)initialising */
    __DMB();
    _VA_PMBUF.bufferAddr  = (uint32_t)(uintptr_t)&s_va_pm_storage[0];
    _VA_PMBUF.bufferSize  = VA_PM_SIZE;
    _VA_PMBUF.wrOff       = 0;
    _VA_PMBUF.rdOff       = 0;
    _VA_PMBUF.discarded   = 0;
    _VA_PMBUF.flags       = 0;
    _VA_PMBUF.cpuFreqHz   = _va_tick_hz;
    _VA_PMBUF.wireVersion = (uint8_t)VA_WIRE_VERSION;
    _VA_PMBUF.tsBytes     = (uint8_t)VA_TIMESTAMP_BYTES;
    _VA_PMBUF.recorderVersion = (uint16_t)((VA_RECORDER_VERSION_MAJOR << 8)
                                          | VA_RECORDER_VERSION_MINOR);
#if VA_SNAPSHOT_SETUP_SIZE > 0
    _VA_PMBUF.setupAddr   = (uint32_t)(uintptr_t)&s_va_pm_setup[0];
#else
    _VA_PMBUF.setupAddr   = 0;
#endif
    _VA_PMBUF.setupUsed   = 0;
    __DMB();
    /* Same rule as the live control block: magic written last and backwards
       so a scanning host can never match a partially initialised block. */
    for (int i = 15; i >= 0; i--)
        _VA_PMBUF.magic[i] = VA_PM_MAGIC[i];
    __DMB();
#endif /* VA_PM_RING */

    /* Transport writers drop bytes until VA_IS_INIT, so the gate must open
       before the tick-source verdict can put its failure report on the
       wire. Interrupts are masked, so nothing else can emit through the
       briefly-open gate of a failing init. */
    VA_IS_INIT = true;

    /* A dead or misconfigured tick source must never look like a working
       session: emit a sync marker plus one named ERR: packet, then refuse
       to start. */
    if (ts_err != NULL)
    {
        _va_emit_sync_marker();
        _va_send_setup_packet(VA_SETUP_INFO, 0, ts_err);
        VA_IS_INIT = false;
        VA_CS_EXIT();
        return;
    }

    _va_emit_sync_marker();

    /* Session-start marker: fresh VA_Init, not a periodic re-emission. */
    _va_send_setup_packet(VA_SETUP_INFO, 0, "SES:START");

    /* MUST come after SES:START (the host resets its sequence epoch on SES;
       the reverse order reads as a phantom loss burst). */
    _va_send_seq_checkpoint();

    char info_buf[40];
    _va_u32_to_str(info_buf, sizeof(info_buf), "CLK:", _va_tick_hz);
    _va_send_setup_packet(VA_SETUP_INFO, 0, info_buf);
#if VA_TRACE_ISRS
    _va_send_setup_packet(VA_SETUP_ISR_MAP, VA_ISR_ID_SYSTICK, "SysTick");
#endif

    _va_send_config_flags_packet(VA_FLAG_GROUP_CATEGORIES, VA_TRACE_CATEGORY_MASK);
    _va_send_config_flags_packet(VA_FLAG_GROUP_BUILD, VA_BUILD_FLAGS);
    _va_send_config_flags_packet(VA_FLAG_GROUP_VERSION, VA_RECORDER_VERSION_PACKED);

#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS)
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "FreeRTOS");
#elif (VA_RTOS_SELECT == VA_RTOS_ZEPHYR)
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "Zephyr");
#else
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "BareMetal");
#endif

#if VA_METADATA
    __DMB();
    for (int i = 15; i >= 0; --i)
        _VA_METADATA.magic[i] = "ViewAlyzerMD01\0\0"[i];
    __DMB();
#endif
    VA_CS_EXIT();
}

#endif /* VA_ENABLED check */

#ifdef __cplusplus
}
#endif
