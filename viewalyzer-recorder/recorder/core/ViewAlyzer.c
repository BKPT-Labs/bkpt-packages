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
#ifndef VA_RTT_MODE
#define VA_RTT_MODE SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL
#endif
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

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__)
#define DWT_ENABLED 1
    static volatile uint32_t g_dwt_overflow_count = 0;
    static volatile uint32_t g_dwt_last_value = 0;
#else
#define DWT_ENABLED 0
#warning "ViewAlyzer requires DWT Cycle Counter (Cortex-M3/M4/M7). ViewAlyzer functions disabled."
#define DWT_NOT_AVAILABLE
#endif

#ifndef DWT_NOT_AVAILABLE /* Only compile the rest if DWT is enabled */

    /* ── Shared global state (exposed via VA_Internal.h) ────────── */
    volatile bool VA_IS_INIT = false;

    /* Stream sync marker. Encodes the wire version (SYNC03 = v3 sequence
       bytes, SYNC02 = v2); carries no sequence byte itself. */
#if VA_SEQ_COUNTER
    static const uint8_t VA_SYNC_MARKER[] = {0x56, 0x41, 0x5A, 0x03, 0x53, 0x59, 0x4E, 0x43, 0x30, 0x33, 0xAA, 0x55};
#else
    static const uint8_t VA_SYNC_MARKER[] = {0x56, 0x41, 0x5A, 0x02, 0x53, 0x59, 0x4E, 0x43, 0x30, 0x32, 0xAA, 0x55};
#endif

#if VA_SEQ_COUNTER
    /* Absolute packet count since VA_Init; low 8 bits go on the wire. */
    static uint32_t _va_seq = 0;
#endif

    static uint32_t _va_cpu_freq = 0;

/* Info-packet markers; must fit VA_MAX_TASK_NAME_LEN untruncated. */
#define VA_INFO_TASKMAP_FULL  "ERR:TASKMAPFULL"
#define VA_INFO_USERMAP_FULL  "ERR:USERMAPFULL"
typedef char va_assert_info_markers_fit_[
    (sizeof (VA_INFO_TASKMAP_FULL) <= VA_MAX_TASK_NAME_LEN &&
     sizeof (VA_INFO_USERMAP_FULL) <= VA_MAX_TASK_NAME_LEN) ? 1 : -1];

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

#if VA_AUTO_SETUP_INTERVAL_MS > 0
    /* Periodic-bundle due tracking (raw CYCCNT, wrap-safe, ISR-safe). */
    static struct {
        uint32_t last_cyc;   /* raw CYCCNT at the last bundle */
        uint32_t interval;   /* cycles, <= 2^31-1; 0 until VA_Init ("not configured") */
        bool     due;        /* set inside CS (possibly in an ISR), serviced outside */
        bool     emitting;   /* thread context only */
    } _va_bundle;
#endif

#if VA_HAS_RTOS && VA_TRACE_STACK_USAGE
    static uint64_t _va_stack_heartbeat_cycles = 0; /* precomputed in VA_Init */
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
    uint8_t           wireVersion; /* 3 = seq bytes present, 2 = legacy     */
    uint8_t           tsBytes;     /* wire timestamp width (4)              */
    uint16_t          reserved;
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

/* Once stalled, emissions drop until the periodic bundle re-arms a retry. */
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
static void _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT)
        return;
    if (_va_itm_stalled)
    {
        VA_TP_DROP(length);
        return;   /* pipe known-dead: drop the packet, don't spin */
    }
    uint32_t i = 0;
    while (length >= 4)
    {
        uint32_t word = ((uint32_t)data[i + 3] << 24) |
                        ((uint32_t)data[i + 2] << 16) |
                        ((uint32_t)data[i + 1] << 8) |
                        ((uint32_t)data[i + 0] << 0);
        if (!ITM_SendU32(VA_ITM_PORT, word))
            return;   /* stalled mid-packet: host re-syncs at the next marker */
        i += 4;
        length -= 4;
    }
    if (length >= 2)
    {
        uint16_t half = (uint16_t)(((uint16_t)data[i + 1] << 8) | (uint16_t)data[i + 0]);
        if (!ITM_SendU16(VA_ITM_PORT, half))
            return;
        i += 2;
        length -= 2;
    }
    if (length > 0)
    {
        if (!ITM_SendU8(VA_ITM_PORT, data[i]))
            return;
        i++;
        length--;
    }
}

#elif VA_TRANSPORT_IS_JLINK
static void _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT)
        return;
    SEGGER_RTT_Write(VA_RTT_CHANNEL, data, length);
}

#elif VA_TRANSPORT_IS_CUSTOM
static void _va_send_bytes(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT || s_user_send_fn == NULL)
        return;
    s_user_send_fn(data, length);
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
} VA_RamBufControlBlock_t;

static VA_RAMBUF_ATTRIBUTES uint8_t s_va_rambuf_storage[VA_RAMBUF_SIZE];
VA_RAMBUF_ATTRIBUTES VA_RamBufControlBlock_t _VA_RAMBUF __attribute__((aligned(4)));

static const char VA_RAMBUF_MAGIC[16] = "ViewAlyzerRB01";

/* One byte is always kept unused so wrOff == rdOff means exactly "empty". */
static inline uint32_t _va_rambuf_free(void)
{
    uint32_t wr = _VA_RAMBUF.wrOff;
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
#if (VA_RAMBUF_MODE == VA_RAMBUF_MODE_BLOCK)
    /* Lossless: stalls the firmware when no host is draining. */
    while (_va_rambuf_free() < length) {}
#else
    /* Drop whole packets, never truncate - the stream stays parseable. */
    if (_va_rambuf_free() < length)
    {
        _VA_RAMBUF.droppedPackets++;
        VA_TP_DROP(length);
        return;
    }
#endif
    uint32_t wr = _VA_RAMBUF.wrOff;
    uint32_t chunk = (uint32_t)VA_RAMBUF_SIZE - wr;
    if (chunk > length)
        chunk = length;
    memcpy(&s_va_rambuf_storage[wr], data, chunk);
    if (chunk < length)
        memcpy(&s_va_rambuf_storage[0], data + chunk, length - chunk);
    wr += length;
    if (wr >= (uint32_t)VA_RAMBUF_SIZE)
        wr -= (uint32_t)VA_RAMBUF_SIZE;
    __DMB();   /* ring bytes must be probe-visible before the offset is */
    _VA_RAMBUF.wrOff = wr;
}
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

static inline uint32_t _va_ring_used(void) { return _va_ring_head - _va_ring_tail; }

/* Push a whole packet or drop it entirely (a partial write would corrupt the
   stream). Caller must hold a VA critical section. */
static void _va_ring_push(const uint8_t *data, uint32_t length)
{
    if (length > (uint32_t)(VA_BUFFER_SIZE) - _va_ring_used())
    {
        _va_dropped_packets++;
        _va_dropped_bytes += length;
        return;
    }
    for (uint32_t i = 0; i < length; ++i)
        _va_ring[(_va_ring_head + i) % VA_BUFFER_SIZE] = data[i];
    _va_ring_head += length;
}
#endif /* VA_TRANSPORT_BUFFERED */

/* ── Packet emission layer ───────────────────────────────────────── */
static inline void _va_emit_packet_raw(const uint8_t *data, uint32_t length)
{
    VA_TP_OFFER(length);
#if VA_SNAPSHOT
    /* Tee every packet into the post-mortem ring, pre-COBS. */
    _va_pm_write(data, length);
#endif
#if VA_TRANSPORT_IS_CUSTOM
    uint8_t cobs_buf[VA_MAX_PACKET_SIZE + (VA_MAX_PACKET_SIZE / 254) + 2];
    size_t encoded_len = va_cobs_encode(data, (size_t)length, cobs_buf);
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
#if VA_SEQ_COUNTER
    /* Stamped in the single funnel so sequence order is exactly wire order;
       packets dropped downstream keep their number (host sees gaps). */
    data[1] = (uint8_t)_va_seq++;
#endif
    /* Triggering packet first, periodic bundle after: time-correlation
       anchors on the packet's wire position. */
    _va_emit_packet_raw(data, length);

#if VA_AUTO_SETUP_INTERVAL_MS > 0
    /* Only flag the bundle as due - emitting ~1 KB inline here would block
       interrupts. _va_service_pending_bundle() emits from thread context. */
    {
        const uint32_t interval = _va_bundle.interval;
        if (interval != 0u && !_va_bundle.emitting && !_va_bundle.due &&
            (uint32_t)(DWT->CYCCNT - _va_bundle.last_cyc) >= interval)
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
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__)
    if (__get_BASEPRI() != 0u)
        return true;
#endif
    return false;
}

void _va_service_pending_bundle(void)
{
#if VA_AUTO_SETUP_INTERVAL_MS > 0
    if (!_va_bundle.due || _va_bundle.emitting)
        return;
    if (_va_in_isr() || _va_irqs_masked())
        return;   /* defer to the next unmasked thread-context log call */

    _va_bundle.due      = false;
    _va_bundle.last_cyc = DWT->CYCCNT;
    _va_bundle.emitting = true;
    VA_EmitSetupBundle();          /* emits each packet under its own short CS */
    _va_bundle.emitting = false;
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
   stamped centrally in _va_emit_packet (collapses away in wire v2). */

void _va_send_event_packet(uint8_t type_byte, uint8_t id, uint64_t timestamp)
{
    uint8_t packet[2 + VA_SEQ_BYTES + VA_TIMESTAMP_BYTES];
    packet[0] = type_byte;
    packet[1 + VA_SEQ_BYTES] = id;
    _va_put_ts(&packet[2 + VA_SEQ_BYTES], timestamp);
    _va_emit_packet(packet, sizeof(packet));
}

#if VA_SEQ_COUNTER
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
#endif

void _va_send_setup_packet(uint8_t setupCode, uint8_t id, const char *name)
{
    uint8_t name_len = (uint8_t)strlen(name);
    if (name_len >= VA_MAX_TASK_NAME_LEN)
    {
        name_len = VA_MAX_TASK_NAME_LEN - 1;
    }
    uint8_t buf[3 + VA_SEQ_BYTES + VA_MAX_TASK_NAME_LEN];
    uint32_t p = 0;
    buf[p++] = setupCode;
    p += VA_SEQ_BYTES;
    buf[p++] = id;
    buf[p++] = name_len;
    memcpy(&buf[p], name, name_len);
    _va_emit_packet(buf, p + name_len);
}

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
    uint8_t name_len = (uint8_t)strlen(name);
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
    buf[p++] = name_len;
    memcpy(&buf[p], name, name_len);
    _va_emit_packet(buf, p + name_len);
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

#if VA_TRACE_HEAP_METRICS || (VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS)
void _va_send_heap_setup_packet(uint8_t id, const char *name, uint32_t totalSize)
{
    uint8_t name_len = (uint8_t)strlen(name);
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
    buf[p++] = name_len;
    memcpy(&buf[p], name, name_len);
    _va_emit_packet(buf, p + name_len);
}
#endif /* heap setup packet */

#if VA_TRACE_STRINGS || VA_TRANSPORT_BUFFERED
/* Session infrastructure, not the VA_TRACE_STRINGS category: VA_Drain()
   loss reporting also goes through here. */
static void _va_emit_string_event(uint8_t id, const char *msg)
{
    if (!msg) return;
    uint16_t len = (uint16_t)strlen(msg);
    if (len == 0) return;
    if (len > VA_MAX_LOG_STRING_LEN) len = VA_MAX_LOG_STRING_LEN;

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
    p += len;

    _va_emit_packet(buf, p);
    VA_CS_EXIT();
}
#endif /* VA_TRACE_STRINGS || VA_TRANSPORT_BUFFERED */

/* ── Timestamp ───────────────────────────────────────────────────── */

/* Cycle-counter read + software 64-bit extension. MUST run with interrupts
   masked; _va_get_timestamp() wraps it for callers outside a CS. */
uint64_t _va_get_timestamp_unlocked(void)
{
    uint32_t current_dwt = DWT->CYCCNT;
    if (current_dwt < g_dwt_last_value)
    {
        g_dwt_overflow_count++;
    }
    g_dwt_last_value = current_dwt;
    return (((uint64_t)g_dwt_overflow_count) << 32) | current_dwt;
}

uint64_t _va_get_timestamp(void)
{
    uint32_t primask_state = __get_PRIMASK();
    __disable_irq();
    uint64_t ts = _va_get_timestamp_unlocked();
    __set_PRIMASK(primask_state);
    return ts;
}

void VA_TickOverflowCheck(void)
{
    if (!VA_IS_INIT) return;
    (void)_va_get_timestamp();
    /* Also a guaranteed unmasked-thread-context point for a due bundle, in
       case every log call happens under masked interrupts. */
    _va_service_pending_bundle();
}

void VA_Drain(void)
{
#if VA_TRANSPORT_BUFFERED
    if (!VA_IS_INIT)
        return;

    /* Report accumulated drops as a StringEvent (id 0). */
    VA_CS_ENTER();
    uint32_t dropped = _va_dropped_packets;
    if (dropped) { _va_dropped_packets = 0; _va_dropped_bytes = 0; }
    VA_CS_EXIT();
    if (dropped)
    {
        char msg[24];
        _va_u32_to_str(msg, sizeof(msg), "DROP:", dropped);
        _va_emit_string_event(0, msg);
    }

    /* Flush in bounded chunks; the possibly-blocking send runs outside the
       CS so interrupts stay enabled. */
    for (;;)
    {
        uint8_t  chunk[64];
        uint32_t n = 0;
        VA_CS_ENTER();
        uint32_t used = _va_ring_used();
        n = (used < sizeof(chunk)) ? used : (uint32_t) sizeof(chunk);
        for (uint32_t i = 0; i < n; ++i)
            chunk[i] = _va_ring[(_va_ring_tail + i) % VA_BUFFER_SIZE];
        _va_ring_tail += n;
        VA_CS_EXIT();

        if (n == 0)
            break;
        _va_send_bytes(chunk, n);
    }
#endif
}

void VA_EmitSetupBundle(void)
{
    if (!VA_IS_INIT)
        return;

#if VA_TRANSPORT_IS_ITM
    /* Re-arm a stalled ITM pipe: one bounded retry per bundle period. */
    _va_itm_stalled = 0;
#endif

    /* Each packet under its own short CS so interrupts are serviced between
       packets; the host only needs each individual packet to be atomic. */

    VA_ATOMIC(_va_emit_sync_marker());
#if VA_SEQ_COUNTER
    VA_ATOMIC(_va_send_seq_checkpoint());
#endif

    {
        char info_buf[40];
        _va_u32_to_str(info_buf, sizeof(info_buf), "CLK:", _va_cpu_freq);
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, info_buf));
    }

#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS)
    VA_ATOMIC(_va_send_setup_packet(VA_SETUP_OS_INFO, 0, "FreeRTOS"));
#elif (VA_RTOS_SELECT == VA_RTOS_ZEPHYR)
    VA_ATOMIC(_va_send_setup_packet(VA_SETUP_OS_INFO, 0, "Zephyr"));
#else
    VA_ATOMIC(_va_send_setup_packet(VA_SETUP_OS_INFO, 0, "BareMetal"));
#endif

    /* Which categories this firmware was built with (for late attachers). */
    VA_ATOMIC(_va_send_config_flags_packet(VA_FLAG_GROUP_CATEGORIES, VA_TRACE_CATEGORY_MASK));
    VA_ATOMIC(_va_send_config_flags_packet(VA_FLAG_GROUP_BUILD, VA_BUILD_FLAGS));

#if VA_NEEDS_TASK_REGISTRY
    for (int i = 0; i < VA_MAX_TASKS; ++i)
    {
        if (taskMap[i].active)
        {
            uint8_t  tid    = taskMap[i].id;
            void    *handle = taskMap[i].handle;

            /* Name map goes out whenever the registry exists. */
            VA_ATOMIC(_va_send_setup_packet(VA_SETUP_TASK_MAP, tid, taskMap[i].name));

#if VA_NEEDS_TASK_SWITCH_EVENTS
            VA_ATOMIC(_va_send_task_create_packet(tid, _va_get_timestamp_unlocked(),
                                                  taskMap[i].uxPriority,
                                                  taskMap[i].uxBasePriority,
                                                  taskMap[i].ulStackDepth));
#endif

#if VA_TRACE_STACK_USAGE
            /* Stack-usage snapshot for late-attaching hosts. */
            if (handle != NULL)
            {
                uint32_t su = va_adapter_calculate_stack_usage(handle);
                uint32_t st = va_adapter_get_total_stack_size(handle);
                if (st > 0)
                    VA_ATOMIC(_va_send_stack_usage_packet(tid, _va_get_timestamp_unlocked(), su, st));
            }
#else
            (void)handle;
#endif
        }
    }
#endif /* VA_NEEDS_TASK_REGISTRY */

#if VA_NEEDS_OBJECT_REGISTRY
    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        if (queueObjectMap[i].active)
            VA_ATOMIC(_va_send_setup_packet(_va_get_setup_packet_type(queueObjectMap[i].type),
                                            queueObjectMap[i].id,
                                            queueObjectMap[i].name));
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

    /* Re-announce latched registry-overflow conditions. */
#if VA_NEEDS_TASK_REGISTRY
    if (_va_task_map_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_TASKMAP_FULL));
#endif
#if VA_NEEDS_USER_TRACE_REGISTRY
    if (_va_user_trace_overflow)
        VA_ATOMIC(_va_send_setup_packet(VA_SETUP_INFO, 0, VA_INFO_USERMAP_FULL));
#endif
}

static void _va_enable_dwt_counter(void)
{
#if (__ARM_ARCH >= 8)
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

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
    taskMap[empty_slot].id = new_id;
#if VA_TRACE_TASK_NOTIFICATIONS
    taskMap[empty_slot].last_notifier = NULL;
#endif
#if VA_TRACE_TASKS
    taskMap[empty_slot].uxPriority = g_task_uxPriority;
    taskMap[empty_slot].uxBasePriority = g_task_uxBasePriority;
#endif
#if VA_TRACE_TASKS || VA_TRACE_STACK_USAGE
    taskMap[empty_slot].ulStackDepth = g_task_ulStackDepth;
#endif
#if VA_TRACE_STACK_USAGE
    taskMap[empty_slot].pxStack = (void *)g_task_pxStack;
    taskMap[empty_slot].pxEndOfStack = (void *)g_task_pxEndOfStack;
    taskMap[empty_slot].lastStackEmitTs = 0;
    taskMap[empty_slot].hasStackSample = false;
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
    default:                             return "Unknown";
    }
}

uint8_t _va_get_setup_packet_type(VA_QueueObjectType_t type)
{
    switch (type)
    {
    case VA_OBJECT_TYPE_QUEUE:
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
        return 0;

    uint8_t new_id = next_queue_object_id++;
    queueObjectMap[empty_slot].active = true;
    queueObjectMap[empty_slot].handle = handle;
    queueObjectMap[empty_slot].id = new_id;
    queueObjectMap[empty_slot].type = type;

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
    case VA_OBJECT_TYPE_QUEUE:
    default:                             return (VA_TRACE_QUEUES != 0);
    }
}

/* Mutexes need a slot even with VA_TRACE_MUTEXES off: the contention
   packet references the mutex by object id. */
static inline bool _va_type_needs_registry(VA_QueueObjectType_t type)
{
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
        return 0;

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
    _va_release_task(_va_find_task_index(taskHandle));
    VA_CS_EXIT();
}
#endif /* VA_NEEDS_TASK_REGISTRY */

#if VA_NEEDS_SWITCH_HOOK
void va_taskswitchedin(void *taskHandle)
{
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

#if VA_TRACE_STACK_USAGE
    /* The high-water scan walks the task's free stack, so the heartbeat
       gates the SCAN, not just the emission - this hook runs on every
       context switch with interrupts masked. */
    if (id != 0 && idx >= 0)
    {
        bool sample = (_va_stack_heartbeat_cycles == 0)
                    || !taskMap[idx].hasStackSample
                    || (now - taskMap[idx].lastStackEmitTs) >= _va_stack_heartbeat_cycles;
        if (sample)
        {
            uint32_t stack_used  = va_adapter_calculate_stack_usage(taskHandle);
            uint32_t stack_total = va_adapter_get_total_stack_size(taskHandle);
            if (stack_total > 0)
            {
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
            else if (strstr(typeHint, "Semaphore") != NULL || strstr(typeHint, "Sem") != NULL)
                type = VA_OBJECT_TYPE_COUNTING_SEM;
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
        default:
            break;
        }
    }

    _va_assign_queue_object_id(queueObject, finalName, type);
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
    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
    {
        VA_QueueObjectType_t new_type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, new_type);
    }

    /* Authoritative check on the STORED type (the only gate on Zephyr). */
    VA_QueueObjectType_t type = _va_get_stored_queue_object_type(queueObject);
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
    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
    {
        VA_QueueObjectType_t new_type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, new_type);
    }

    VA_QueueObjectType_t type = _va_get_stored_queue_object_type(queueObject);
    if (_va_type_emits_events(type))
        _va_send_event_packet(_va_event_type_for_object(type), id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

#endif /* VA_NEEDS_OBJECT_REGISTRY */

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
    uint8_t id = _va_find_queue_object_id(heapObject);
    if (id == 0)
    {
        id = _va_assign_queue_object_id(heapObject, name, VA_OBJECT_TYPE_HEAP);
    }
    _va_send_heap_setup_packet(id, name, totalSize);
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

void VA_Init(uint32_t cpu_freq)
{
    VA_CS_ENTER();
    _va_cpu_freq = cpu_freq;
    g_dwt_overflow_count = 0;
    g_dwt_last_value = 0;

#if VA_NEEDS_TASK_REGISTRY
    /* Reset MRU lookup caches (stale slot indices from a previous run). */
    _va_task_cache_handle = NULL;
    _va_task_cache_idx    = -1;
#endif

#if VA_SEQ_COUNTER
    _va_seq = 0;
#endif

#if VA_AUTO_SETUP_INTERVAL_MS > 0
    _va_bundle.last_cyc = 0;
    _va_bundle.emitting = false;
    _va_bundle.due      = false;
    /* Bundle interval in cycles, clamped to 2^31-1 for wrap-safe compares. */
    {
        uint64_t interval = ((uint64_t)_va_cpu_freq / 1000) * VA_AUTO_SETUP_INTERVAL_MS;
        _va_bundle.interval = (interval > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (uint32_t)interval;
    }
#endif

#if VA_HAS_RTOS && VA_TRACE_STACK_USAGE
    _va_stack_heartbeat_cycles = ((uint64_t)_va_cpu_freq / 1000) * VA_STACK_USAGE_HEARTBEAT_MS;
#endif

#if VA_TRANSPORT_BUFFERED
    _va_ring_head = 0;
    _va_ring_tail = 0;
    _va_dropped_packets = 0;
    _va_dropped_bytes = 0;
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

    _va_enable_dwt_counter();

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
    _VA_PMBUF.cpuFreqHz   = _va_cpu_freq;
    _VA_PMBUF.wireVersion = (uint8_t)(VA_SEQ_COUNTER ? 3 : 2);
    _VA_PMBUF.tsBytes     = (uint8_t)VA_TIMESTAMP_BYTES;
    _VA_PMBUF.reserved    = 0;
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
    VA_IS_INIT = true;

    _va_emit_sync_marker();

    /* Session-start marker: fresh VA_Init, not a periodic re-emission. */
    _va_send_setup_packet(VA_SETUP_INFO, 0, "SES:START");

#if VA_SEQ_COUNTER
    /* MUST come after SES:START (the host resets its sequence epoch on SES;
       the reverse order reads as a phantom loss burst). */
    _va_send_seq_checkpoint();
#endif

    char info_buf[40];
    _va_u32_to_str(info_buf, sizeof(info_buf), "CLK:", _va_cpu_freq);
    _va_send_setup_packet(VA_SETUP_INFO, 0, info_buf);
#if VA_TRACE_ISRS
    _va_send_setup_packet(VA_SETUP_ISR_MAP, VA_ISR_ID_SYSTICK, "SysTick");
#endif

    _va_send_config_flags_packet(VA_FLAG_GROUP_CATEGORIES, VA_TRACE_CATEGORY_MASK);
    _va_send_config_flags_packet(VA_FLAG_GROUP_BUILD, VA_BUILD_FLAGS);

#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS)
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "FreeRTOS");
#elif (VA_RTOS_SELECT == VA_RTOS_ZEPHYR)
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "Zephyr");
#else
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "BareMetal");
#endif

    VA_CS_EXIT();
}

#endif /* DWT_NOT_AVAILABLE check */
#endif /* VA_ENABLED check */

#ifdef __cplusplus
}
#endif
