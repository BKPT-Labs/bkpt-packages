/**
 * @file ViewAlyzer.c
 * @brief ViewAlyzer Recorder Firmware - RTOS-Agnostic Core Engine
 *
 * This file contains the transport layer, timestamp engine, packet emission,
 * and generic task/object map management.  All RTOS-specific logic (stack
 * introspection, queue-type detection, mutex-holder queries) lives in the
 * corresponding adapter file (VA_Adapter_FreeRTOS.c, VA_Adapter_Zephyr.c, …).
 *
 * Copyright (c) 2025 Free Radical Labs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction for non-commercial purposes, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute,
 * and sublicense copies of the Software, subject to the conditions in the LICENSE file.
 *
 * For commercial licensing or questions about usage restrictions, contact:
 * support@viewalyzer.net
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#include "ViewAlyzer.h"

#ifdef __cplusplus
extern "C"
{
#endif
#if (VA_ENABLED == 1)

#include "VA_Internal.h"
#include <string.h>

// Include RTT header only if needed
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

/* ================================================================
 *  Throughput-test byte counters (compile-gated, OFF by default)
 * ================================================================
 *  Built only with -DVA_TP_TEST=1 (see the Nucleo_H723_Throughput
 *  example). The recorder tallies how many bytes it OFFERED to the
 *  active transport vs. how many the transport had to DROP — the
 *  offered-vs-delivered gap that a host-side "delivered KB/s" reading
 *  alone can't reveal. A debugger/host reads the _VA_TP block by
 *  symbol (or by scanning RAM for the "VATPCNT1" magic). Layout is a
 *  little-endian host contract: do not reorder fields.
 *
 *  offered  = every packet handed to _va_send_bytes (protocol bytes,
 *             pre-transport-framing — matches the host's decoded count)
 *  dropped  = packets the transport could not accept (ITM FIFO stall,
 *             RAM-ring full, or over-size). delivered = offered - dropped. */
#if defined(VA_TP_TEST) && (VA_TP_TEST == 1)
/* VA_TpCounters_t is declared in ViewAlyzer.h so the host/app can read it. */
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

/* Lightweight "name_Suffix" concatenation (replaces snprintf("%s_Suffix")) */
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

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__)
#define DWT_ENABLED 1
    static volatile uint32_t g_dwt_overflow_count = 0;
    static volatile uint32_t g_dwt_last_value = 0;
#else
#define DWT_ENABLED 0
#warning "ViewAlyzer requires DWT Cycle Counter (Cortex-M3/M4/M7). ViewAlyzer functions disabled."
#define DWT_NOT_AVAILABLE
#endif

#ifndef DWT_NOT_AVAILABLE // Only compile the rest if DWT is enabled

    /* ── Shared global state (exposed via VA_Internal.h) ────────── */
    volatile bool VA_IS_INIT = false;

    // --- Stream Sync Marker ---
    // Byte 3 and byte 9 encode the wire-format version so a host locks onto
    // the correct packet layout from the marker alone. Protocol v2 (32-bit
    // timestamps): "VAZ\x02" "SYNC02" AA 55. Protocol v3 = v2 plus a rolling
    // 8-bit sequence byte at offset 1 of every packet (see VA_SEQ_COUNTER):
    // "VAZ\x03" "SYNC03" AA 55. The marker itself carries no sequence byte
    // and does not consume a sequence number.
#if VA_SEQ_COUNTER
    static const uint8_t VA_SYNC_MARKER[] = {0x56, 0x41, 0x5A, 0x03, 0x53, 0x59, 0x4E, 0x43, 0x30, 0x33, 0xAA, 0x55};
#else
    static const uint8_t VA_SYNC_MARKER[] = {0x56, 0x41, 0x5A, 0x02, 0x53, 0x59, 0x4E, 0x43, 0x30, 0x32, 0xAA, 0x55};
#endif

#if VA_SEQ_COUNTER
    // Absolute count of sequence-stamped packets emitted since VA_Init. The
    // low 8 bits go on the wire in every packet; the full value rides in the
    // periodic VA_EVENT_SEQ_CHECKPOINT. Guarded by the caller's critical
    // section like all emission state (single writer).
    static uint32_t _va_seq = 0;
#endif

    // --- Task ID Mapping (RTOS-agnostic) ---
    VA_TaskMapEntry_t taskMap[VA_MAX_TASKS];
    uint8_t next_task_id = 1;
    static uint32_t _va_cpu_freq = 0;

    // MRU (one-entry) lookup caches. Trace hooks cluster heavily on the current
    // task and a few hot sync objects, so a single-entry cache turns the common
    // repeated linear scan into O(1). Reset on VA_Init; updated on assign.
    // Single-core assumption: reads/writes happen inside the caller's critical
    // section. On SMP these would need per-core caches or removal.
    static void *_va_task_cache_handle = NULL;
    static int   _va_task_cache_idx    = -1;

#if VA_AUTO_SETUP_INTERVAL_MS > 0
    static uint64_t _va_last_bundle_ts   = 0;
    static bool     _va_emitting_bundle  = false;
    static bool     _va_bundle_due       = false;   // set inside CS, serviced outside CS
    static uint64_t _va_interval_cycles  = 0;       // precomputed in VA_Init
#endif

#if VA_CAPTURE_STACK_USAGE
    static uint64_t _va_stack_heartbeat_cycles = 0; // precomputed in VA_Init
#endif

    volatile uint32_t notificationValue = 0;

    // Global variables to store task information during creation
    volatile void *g_task_pxStack = NULL;
    volatile void *g_task_pxEndOfStack = NULL;
    volatile uint32_t g_task_uxPriority = 0;
    volatile uint32_t g_task_uxBasePriority = 0;
    volatile uint32_t g_task_ulStackDepth = 0;

    // User Event Tracking (Independent of RTOS)
    typedef struct
    {
        uint8_t id;
        char name[VA_MAX_TASK_NAME_LEN];
        bool active;
    } VA_UserEventMapEntry_t;
    static VA_UserEventMapEntry_t userEventMap[VA_MAX_USER_EVENTS];

    // User trace registry (Independent of RTOS). Registrations are stored so
    // VA_EmitSetupBundle() can re-emit the id->name/type maps periodically —
    // without this, a host that attaches after boot (live attach, or a fused
    // ETM+ITM capture window) never learns the names and falls back to
    // synthetic "USER_<id>" labels.
    typedef struct
    {
        uint8_t id;
        uint8_t type;                       /* VA_UserTraceType_t */
        char name[VA_MAX_TASK_NAME_LEN];
        bool active;
    } VA_UserTraceMapEntry_t;
    static VA_UserTraceMapEntry_t userTraceMap[VA_MAX_USER_EVENTS];

#if VA_HAS_RTOS
    // --- Queue / sync-object map (RTOS-agnostic storage, adapter determines type) ---
    VA_QueueObjectMapEntry_t queueObjectMap[VA_MAX_SYNC_OBJECTS];
    uint8_t next_queue_object_id = 1;
#endif

/* ================================================================
 *  Transport layer
 * ================================================================ */

#if VA_TRANSPORT_IS_ITM
/* How long to spin waiting for the ITM stimulus FIFO before declaring the
   trace pipe STALLED. A draining pipe frees a FIFO slot within tens of
   microseconds even at slow SWO rates, so this limit (~ms at typical clocks)
   is only ever exhausted when the pipe is dead: nobody has enabled the SWO
   pin/TPIU (DBGMCU trace gating is debugger-owned), or the debugger went
   away. Without the limit the first emission spins forever and FREEZES the
   application — a target running VA-instrumented firmware with no host
   attached deadlocked at VA_Init's first setup packet. */
#ifndef VA_ITM_STALL_SPIN_LIMIT
#define VA_ITM_STALL_SPIN_LIMIT 50000u
#endif

/* Once stalled, emissions drop immediately (no spin cost) until the next
   periodic setup-bundle attempt re-arms one bounded retry (~every
   VA_AUTO_SETUP_INTERVAL_MS). A host that attaches and enables the drain
   therefore restores tracing within one bundle period, and its first
   packets are exactly the sync marker + setup bundle it needs to re-sync.
   Races on the flag are benign: worst case is one extra bounded spin. */
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
        VA_TP_DROP(length);   /* throughput test: pipe stalled → whole packet lost */
        return;     /* pipe known-dead: drop the packet, don't spin */
    }
    uint32_t i = 0;
    while (length >= 4)
    {
        uint32_t word = ((uint32_t)data[i + 3] << 24) |
                        ((uint32_t)data[i + 2] << 16) |
                        ((uint32_t)data[i + 1] << 8) |
                        ((uint32_t)data[i + 0] << 0);
        if (!ITM_SendU32(VA_ITM_PORT, word))
            return;  /* stalled mid-packet: drop the rest (host parser
                        treats the truncated packet as a corrupt run and
                        re-syncs at the next marker) */
        i += 4;
        length -= 4;
    }
    /* Send a 2-byte tail as one u16 write (3 wire bytes) instead of two u8
       writes (4 wire bytes) — saves a wire byte on every odd-length tail. */
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
/* ViewAlyzer-owned RAM ring buffer, drained by the host through the debug
   probe (ST-Link, CMSIS-DAP, J-Link, ...) with non-intrusive memory reads.
   The host locates _VA_RAMBUF by scanning RAM for the magic tag (or via the
   ELF symbol), then polls wrOff, reads new ring bytes, and advances rdOff.
   The struct layout below is a host-protocol contract: fixed field order,
   little-endian, magic at offset 0. Do not reorder or insert fields.

   Concurrency model: every emission runs inside a VA critical section
   (single writer), and the host is the only writer of rdOff — a single
   aligned 32-bit store from the probe, so no locking is needed on either
   side. The DMB before publishing wrOff guarantees the probe can never read
   an offset that points at bytes still in flight. */
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
    /* Lossless mode. The host advances rdOff through the debug port, so this
       makes progress even inside a critical section — but it stalls the
       firmware indefinitely when no host is draining the buffer. */
    while (_va_rambuf_free() < length) {}
#else
    /* Packets are dropped whole, never truncated, so every byte that does
       reach the ring is a complete packet and the stream stays parseable. */
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

#else
#error "VA_TRANSPORT must be ARM_ITM, JLINK_RTT, CUSTOM_TRANSPORT, or RAM_BUFFER"
#endif // VA_TRANSPORT

/* ================================================================
 *  Optional buffered transport — RAM ring drained by VA_Drain()
 * ================================================================ */
#if VA_TRANSPORT_BUFFERED
/* Free-running byte ring. head/tail are monotonically increasing counters
   (unsigned wraparound is well-defined and correct as long as the in-flight
   count never exceeds VA_BUFFER_SIZE, which the push guard enforces). All
   head/tail mutation happens inside a VA critical section (producers already
   hold one; VA_Drain takes one for the pop), so this is safe on single-core. */
static uint8_t           _va_ring[VA_BUFFER_SIZE];
static volatile uint32_t _va_ring_head = 0;   /* next write index (free-running) */
static volatile uint32_t _va_ring_tail = 0;   /* next read index  (free-running) */
static volatile uint32_t _va_dropped_packets = 0;
static volatile uint32_t _va_dropped_bytes   = 0;

static inline uint32_t _va_ring_used(void) { return _va_ring_head - _va_ring_tail; }

/* Push a whole packet or drop it entirely (never a partial write — a partial
   packet would corrupt the stream). Caller must hold a VA critical section. */
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

/* ================================================================
 *  Packet emission layer
 * ================================================================ */
static inline void _va_emit_packet_raw(const uint8_t *data, uint32_t length)
{
    VA_TP_OFFER(length);   /* throughput test: count offered load (no-op otherwise) */
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
    /* Stamp the rolling sequence byte here, in the single funnel every packet
       passes through, so the sequence order is exactly the wire order (the
       caller holds the critical section that serializes emission). Packets
       dropped downstream (ring full, ITM stall, rambuf full) have already
       consumed their number — the host sees those as sequence gaps, which is
       the point: source drops and transport loss are both real loss. */
    data[1] = (uint8_t)_va_seq++;
#endif
    /* The triggering packet goes out FIRST, the periodic bundle after it.
     * The packet's position on the wire is what time-correlation (fused
     * ETM+ITM captures) anchors on — emitting the bundle first would place
     * an event's bytes hundreds of ms after the code that logged it, so a
     * "jump to cause" lands in the bundle loop instead of the caller. The
     * host's sync scanning is order-agnostic, so parsing is unaffected. */
    _va_emit_packet_raw(data, length);

#if VA_AUTO_SETUP_INTERVAL_MS > 0
    /* Only FLAG the bundle as due here — do not emit it inline. This path
       runs inside the caller's critical section (and possibly in an ISR), so
       emitting ~1 KB of setup packets here would block interrupts for
       milliseconds. Emission is deferred to _va_service_pending_bundle(),
       called from thread context with no CS held (see WP-5). */
    if (!_va_emitting_bundle && _va_cpu_freq > 0 && !_va_bundle_due)
    {
        uint64_t now = _va_get_timestamp_unlocked();   /* already inside CS */
        if (now - _va_last_bundle_ts >= _va_interval_cycles)
            _va_bundle_due = true;
    }
#endif
}

/* Emit the sync marker. Markers are a fixed byte literal scanned for by the
   host — they carry no sequence byte and must not consume a sequence number,
   so they bypass _va_emit_packet's stamping. */
static inline void _va_emit_sync_marker(void)
{
    _va_emit_packet_raw(VA_SYNC_MARKER, sizeof(VA_SYNC_MARKER));
}

/* True when executing in an exception/interrupt context (any Cortex-M).
   PendSV (the RTOS context switch) also reports non-zero IPSR, so this
   correctly defers bundle emission out of the scheduler path too. */
static inline bool _va_in_isr(void)
{
    return (__get_IPSR() & 0x1FFu) != 0u;
}

void _va_service_pending_bundle(void)
{
#if VA_AUTO_SETUP_INTERVAL_MS > 0
    if (!_va_bundle_due || _va_emitting_bundle)
        return;
    if (_va_in_isr())
        return;   /* defer to the next thread-context log call */

    _va_bundle_due      = false;
    _va_last_bundle_ts  = _va_get_timestamp();
    _va_emitting_bundle = true;
    VA_EmitSetupBundle();          /* emits each packet under its own short CS */
    _va_emitting_bundle = false;
#endif
}

/* ================================================================
 *  Packet construction helpers (non-static — adapters use these)
 * ================================================================ */

/* Write the little-endian timestamp field (VA_TIMESTAMP_BYTES wide) at dst
   and return the number of bytes written. This is the single point that
   controls the wire timestamp width (v1 = 8 bytes, v2 = 4 bytes). */
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

/* All builders lay packets out as [type][seq][rest...]: the byte after the
   type code is reserved for the rolling sequence number (v3, VA_SEQ_BYTES=1)
   and stamped centrally in _va_emit_packet. With VA_SEQ_COUNTER=0 the slot
   collapses away and the layout is byte-identical to wire v2. */

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

/* ================================================================
 *  Timestamp
 * ================================================================ */

/* Core cycle-counter read + software 64-bit extension. MUST run with
   interrupts masked (the read-compare-update of the overflow counter is not
   otherwise atomic). Callers already inside a critical section use this
   directly; _va_get_timestamp() wraps it with the PRIMASK save/restore for
   callers that are not. */
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
}

void VA_Drain(void)
{
#if VA_TRANSPORT_BUFFERED
    if (!VA_IS_INIT)
        return;

    /* Report accumulated drops (if any) as a StringEvent (id 0). Snapshot and
       clear the counters under a CS, then log outside it — VA_LogString pushes
       into the same ring, which the flush loop below then drains. */
    VA_CS_ENTER();
    uint32_t dropped = _va_dropped_packets;
    if (dropped) { _va_dropped_packets = 0; _va_dropped_bytes = 0; }
    VA_CS_EXIT();
    if (dropped)
    {
        char msg[24];
        _va_u32_to_str(msg, sizeof(msg), "DROP:", dropped);
        VA_LogString(0, msg);
    }

    /* Flush the ring to the wire in bounded chunks. Each chunk is popped under
       a short CS; the (possibly blocking) send runs outside the CS so
       interrupts stay enabled while the transport FIFO drains. */
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
    /* Re-arm a stalled ITM pipe: retry one bounded emission per bundle
       period. If a host has attached and enabled the SWO drain since the
       stall, tracing resumes here — leading with the sync marker + setup
       bundle the host needs to sync onto the stream. */
    _va_itm_stalled = 0;
#endif

    /* Each packet is emitted under its OWN short critical section so
       interrupts are serviced between packets. The whole bundle (~1 KB) used
       to go out under a single VA_CS_ENTER, blocking interrupts for
       milliseconds. The host parser is order-agnostic between packets and
       only needs each individual packet to be atomic, so this is safe.
       The caller (_va_service_pending_bundle) runs in thread context with no
       CS held — see WP-5. */

    VA_ATOMIC(_va_emit_sync_marker());
#if VA_SEQ_COUNTER
    /* Absolute checkpoint rides with every bundle so a host attaching or
       recovering mid-stream can compute exact loss totals. */
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

#if VA_HAS_RTOS
    for (int i = 0; i < VA_MAX_TASKS; ++i)
    {
        if (taskMap[i].active)
        {
            uint8_t  tid    = taskMap[i].id;
            void    *handle = taskMap[i].handle;
            VA_ATOMIC(
                _va_send_setup_packet(VA_SETUP_TASK_MAP, tid, taskMap[i].name);
                _va_send_task_create_packet(tid, _va_get_timestamp_unlocked(),
                                            taskMap[i].uxPriority,
                                            taskMap[i].uxBasePriority,
                                            taskMap[i].ulStackDepth)
            );

#if VA_CAPTURE_STACK_USAGE
            /* Snapshot current stack usage so a late-attaching host gets a
               full picture within one bundle interval even though switch-out
               only re-emits on change. */
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

    for (int i = 0; i < VA_MAX_SYNC_OBJECTS; ++i)
    {
        if (queueObjectMap[i].active)
            VA_ATOMIC(_va_send_setup_packet(_va_get_setup_packet_type(queueObjectMap[i].type),
                                            queueObjectMap[i].id,
                                            queueObjectMap[i].name));
    }
#endif

    /* User trace + user event registrations (RTOS-independent): re-emit the
     * stored maps so hosts that attach mid-run (live attach, fused ETM+ITM
     * capture windows) resolve ids to real names instead of fallbacks. */
    for (int i = 0; i < VA_MAX_USER_EVENTS; ++i)
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
        if (userEventMap[i].active)
            VA_ATOMIC(_va_send_setup_packet(VA_SETUP_USER_EVENT_MAP, userEventMap[i].id,
                                            userEventMap[i].name));
    }
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

/* ================================================================
 *  Generic task-map helpers
 * ================================================================ */

/* Single cached scanner. Trace hooks call this repeatedly for the same
   handle (e.g. switch-in then stack-usage), so a one-entry MRU cache turns
   the common case into O(1). */
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
        return 0;

    uint8_t new_id = next_task_id++;
    taskMap[empty_slot].active = true;
    taskMap[empty_slot].handle = handle;
    taskMap[empty_slot].id = new_id;
    taskMap[empty_slot].last_notifier = NULL;

    taskMap[empty_slot].pxStack = (void *)g_task_pxStack;
    taskMap[empty_slot].pxEndOfStack = (void *)g_task_pxEndOfStack;
    taskMap[empty_slot].uxPriority = g_task_uxPriority;
    taskMap[empty_slot].uxBasePriority = g_task_uxBasePriority;
    taskMap[empty_slot].ulStackDepth = g_task_ulStackDepth;
    taskMap[empty_slot].lastStackUsed = 0;
    taskMap[empty_slot].lastStackEmitTs = 0;
    taskMap[empty_slot].hasStackSample = false;

    strncpy(taskMap[empty_slot].name, name, VA_MAX_TASK_NAME_LEN - 1);
    taskMap[empty_slot].name[VA_MAX_TASK_NAME_LEN - 1] = '\0';

    /* Prime the MRU cache with the freshly assigned slot. */
    _va_task_cache_handle = handle;
    _va_task_cache_idx    = empty_slot;

    _va_send_setup_packet(VA_SETUP_TASK_MAP, new_id, taskMap[empty_slot].name);
    return new_id;
}

/* ================================================================
 *  Queue / sync-object map helpers
 * ================================================================ */
#if VA_HAS_RTOS

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

/* One-entry MRU cache for the sync-object map (same rationale as the task
   cache): a single kernel op does several lookups of the same handle. */
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
        strncpy(queueObjectMap[empty_slot].name, name, VA_MAX_TASK_NAME_LEN - 1);
    }
    else
    {
        strncpy(queueObjectMap[empty_slot].name, _va_get_object_type_name(type), VA_MAX_TASK_NAME_LEN - 1);
    }
    queueObjectMap[empty_slot].name[VA_MAX_TASK_NAME_LEN - 1] = '\0';

    _va_send_setup_packet(_va_get_setup_packet_type(type), new_id, queueObjectMap[empty_slot].name);
    return new_id;
}

#endif /* VA_HAS_RTOS */

/* ================================================================
 *  User-event map (RTOS-independent)
 * ================================================================ */

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
    strncpy(userEventMap[empty_slot].name, name, VA_MAX_TASK_NAME_LEN - 1);
    userEventMap[empty_slot].name[VA_MAX_TASK_NAME_LEN - 1] = '\0';

    _va_send_setup_packet(VA_SETUP_USER_EVENT_MAP, event_id, userEventMap[empty_slot].name);
    return event_id;
}

/* ================================================================
 *  RTOS task-event hooks (generic — delegate to adapter for OS specifics)
 * ================================================================ */

void va_taskcreated(void *taskHandle, const char *name)
{
#if VA_HAS_RTOS
    VA_CS_ENTER();
    uint8_t assigned_id = _va_assign_task_id(taskHandle, name ? name : "???");
    if (assigned_id > 0)
    {
        uint64_t timestamp = _va_get_timestamp();
        _va_send_task_create_packet(assigned_id, timestamp,
                                     g_task_uxPriority, g_task_uxBasePriority, g_task_ulStackDepth);
    }
    VA_CS_EXIT();
#else
    VA_UNUSED(taskHandle);
    VA_UNUSED(name);
#endif
}

void va_taskswitchedin(void *taskHandle)
{
#if VA_HAS_RTOS
    /* Runs in scheduler (PendSV/ISR) context — no bundle service here.
       Stack usage is measured once per execution slice, on switch-OUT. */
    VA_CS_ENTER();
    uint8_t id = _va_find_task_id(taskHandle);
    _va_send_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_TASK_SWITCH, id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
#else
    VA_UNUSED(taskHandle);
#endif
}

void va_taskswitchedout(void *taskHandle)
{
#if VA_HAS_RTOS
    VA_CS_ENTER();
    int     idx = _va_find_task_index(taskHandle);
    uint8_t id  = idx >= 0 ? taskMap[idx].id : 0;
    uint64_t now = _va_get_timestamp_unlocked();
    _va_send_event_packet(VA_EVENT_TASK_SWITCH, id, now);

#if VA_CAPTURE_STACK_USAGE
    /* Emit stack usage only when it changed since the last sample for this
       task, or after a heartbeat interval — the value rarely changes between
       switches, so this removes the bulk of these packets while keeping the
       host's per-task stack display live. (Was emitted on every switch, both
       in and out — the dominant slice of stream bandwidth.) */
    if (id != 0)
    {
        uint32_t stack_used  = va_adapter_calculate_stack_usage(taskHandle);
        uint32_t stack_total = va_adapter_get_total_stack_size(taskHandle);
        if (stack_total > 0)
        {
            bool emit = true;
            if (idx >= 0)
            {
                bool changed   = !taskMap[idx].hasStackSample
                               || stack_used != taskMap[idx].lastStackUsed;
                bool heartbeat = (_va_stack_heartbeat_cycles == 0)
                               || !taskMap[idx].hasStackSample
                               || (now - taskMap[idx].lastStackEmitTs) >= _va_stack_heartbeat_cycles;
                emit = changed || heartbeat;
            }
            if (emit)
            {
                _va_send_stack_usage_packet(id, now, stack_used, stack_total);
                if (idx >= 0)
                {
                    taskMap[idx].lastStackUsed   = stack_used;
                    taskMap[idx].lastStackEmitTs = now;
                    taskMap[idx].hasStackSample  = true;
                }
            }
        }
    }
#endif
    VA_CS_EXIT();
#else
    VA_UNUSED(taskHandle);
#endif
}

/* ================================================================
 *  ISR logging (RTOS-independent)
 * ================================================================ */

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

bool va_isnit(void)
{
    return VA_IS_INIT;
}

/* ================================================================
 *  User-trace / data logging (RTOS-independent)
 * ================================================================ */

void VA_RegisterUserTrace(uint8_t id, const char *name, VA_UserTraceType_t type)
{
    VA_CS_ENTER();
    if (id == 0 || name == NULL)
    {
        VA_CS_EXIT();
        return;
    }

    /* Remember (or update) the registration so the periodic setup bundle can
     * re-emit it for late-attaching hosts. Full registry => emit-only (the
     * one-shot packet below still goes out). */
    int slot = -1;
    for (int i = 0; i < VA_MAX_USER_EVENTS; ++i)
    {
        if (userTraceMap[i].active && userTraceMap[i].id == id) { slot = i; break; }
        if (slot < 0 && !userTraceMap[i].active) slot = i;
    }
    if (slot >= 0)
    {
        userTraceMap[slot].active = true;
        userTraceMap[slot].id = id;
        userTraceMap[slot].type = (uint8_t)type;
        strncpy(userTraceMap[slot].name, name, VA_MAX_TASK_NAME_LEN - 1);
        userTraceMap[slot].name[VA_MAX_TASK_NAME_LEN - 1] = '\0';
    }

    if (type == VA_USER_TYPE_ISR)
        _va_send_setup_packet(VA_SETUP_ISR_MAP, id, name);
    else
        _va_send_user_setup_packet(id, (uint8_t)type, name);
    VA_CS_EXIT();
}

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

void VA_LogString(uint8_t id, const char *msg)
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

void VA_LogToggle(uint8_t id, bool state)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_user_toggle_event_packet(id, state, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void VA_LogGPIO(uint8_t id, bool state)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_data_event_packet(VA_EVENT_GPIO, id, (uint32_t)state, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void VA_LogCounter(uint8_t id, uint32_t value)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_data_event_packet(VA_EVENT_COUNTER, id, value, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

void VA_LogHeap(uint8_t id, uint32_t usedBytes)
{
    _va_service_pending_bundle();
    VA_CS_ENTER();
    _va_send_data_event_packet(VA_EVENT_HEAP, id, usedBytes, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
}

/* ================================================================
 *  Sleep enter/exit (k_sleep, k_msleep, k_usleep)
 * ================================================================ */

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

/* ================================================================
 *  PM (power management) suspend enter/exit
 * ================================================================ */

/* Sentinel handle used as the sync-object pointer for PM events.
   Using a static variable's address guarantees a unique, stable value. */
static uint8_t _va_pm_sentinel;

void va_logPMSuspendEnter(void)
{
#if VA_HAS_RTOS
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(&_va_pm_sentinel);
    if (id == 0)
        id = _va_assign_queue_object_id(&_va_pm_sentinel, "__va_pm__", VA_OBJECT_TYPE_POWER_MGMT);
    if (id != 0)
        _va_send_event_packet(VA_EVENT_FLAG_START_END | VA_EVENT_PM_SUSPEND, id, _va_get_timestamp());
    VA_CS_EXIT();
#endif
}

void va_logPMSuspendExit(uint8_t state)
{
#if VA_HAS_RTOS
    (void)state;
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(&_va_pm_sentinel);
    if (id != 0)
        _va_send_event_packet(VA_EVENT_PM_SUSPEND, id, _va_get_timestamp());
    VA_CS_EXIT();
#endif
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

/* ================================================================
 *  Task notification hooks
 * ================================================================ */

void va_logtasknotifygive(void *srcHandle, void *destHandle, uint32_t value)
{
#if VA_HAS_RTOS
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
#else
    VA_UNUSED(srcHandle);
    VA_UNUSED(destHandle);
    VA_UNUSED(value);
#endif
}

void va_logtasknotifytake(void *taskHandle, uint32_t value)
{
#if VA_HAS_RTOS
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
#else
    VA_UNUSED(taskHandle);
    VA_UNUSED(value);
#endif
}

/* ================================================================
 *  Queue / sync-object event hooks
 * ================================================================ */

void va_logQueueObjectCreate(void *queueObject, const char *name)
{
    va_logQueueObjectCreateWithType(queueObject, name);
}

void va_updateQueueObjectType(void *queueObject, const char *typeHint)
{
#if VA_HAS_RTOS
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

        strncpy(queueObjectMap[idx].name, finalName, VA_MAX_TASK_NAME_LEN - 1);
        queueObjectMap[idx].name[VA_MAX_TASK_NAME_LEN - 1] = '\0';

        _va_send_setup_packet(_va_get_setup_packet_type(type), queueObjectMap[idx].id, queueObjectMap[idx].name);
    }

    VA_CS_EXIT();
#else
    VA_UNUSED(queueObject);
    VA_UNUSED(typeHint);
#endif
}

void va_logQueueObjectCreateWithType(void *queueObject, const char *typeHint)
{
#if VA_HAS_RTOS
    if (queueObject == NULL)
        return;

    VA_CS_ENTER();
    VA_QueueObjectType_t type = va_adapter_get_queue_object_type(queueObject);

    /* If the adapter returned the default (QUEUE), infer from typeHint.
     * This is essential for Zephyr where k_mutex/k_sem/k_msgq are separate
     * types that can't be distinguished from a void* alone. */
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
#else
    VA_UNUSED(queueObject);
    VA_UNUSED(typeHint);
#endif
}

void va_logQueueObjectGive(void *queueObject, uint32_t timeout)
{
    VA_UNUSED(timeout);
#if VA_HAS_RTOS
    if (queueObject == NULL)
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
    {
        VA_QueueObjectType_t type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, type);
    }

    uint8_t event_type;
    VA_QueueObjectType_t type = _va_get_stored_queue_object_type(queueObject);
    switch (type)
    {
    case VA_OBJECT_TYPE_MUTEX:
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX:
        event_type = VA_EVENT_MUTEX;
        break;
    case VA_OBJECT_TYPE_COUNTING_SEM:
    case VA_OBJECT_TYPE_BINARY_SEM:
        event_type = VA_EVENT_SEMAPHORE;
        break;
    case VA_OBJECT_TYPE_TIMER:
        event_type = VA_EVENT_TIMER;
        break;
    case VA_OBJECT_TYPE_POWER_MGMT:
        event_type = VA_EVENT_PM_SUSPEND;
        break;
    case VA_OBJECT_TYPE_QUEUE:
    default:
        event_type = VA_EVENT_QUEUE;
        break;
    }

    _va_send_event_packet(VA_EVENT_FLAG_START_END | event_type, id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
#endif
}

void va_logQueueObjectTake(void *queueObject, uint32_t timeout)
{
#if VA_HAS_RTOS
    if (queueObject == NULL)
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();
    VA_UNUSED(timeout);
    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
    {
        VA_QueueObjectType_t type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, type);
    }

    uint8_t event_type;
    VA_QueueObjectType_t type = _va_get_stored_queue_object_type(queueObject);

    switch (type)
    {
    case VA_OBJECT_TYPE_MUTEX:
    case VA_OBJECT_TYPE_RECURSIVE_MUTEX:
        event_type = VA_EVENT_MUTEX;
        break;
    case VA_OBJECT_TYPE_COUNTING_SEM:
    case VA_OBJECT_TYPE_BINARY_SEM:
        event_type = VA_EVENT_SEMAPHORE;
        break;
    case VA_OBJECT_TYPE_TIMER:
        event_type = VA_EVENT_TIMER;
        break;
    case VA_OBJECT_TYPE_POWER_MGMT:
        event_type = VA_EVENT_PM_SUSPEND;
        break;
    case VA_OBJECT_TYPE_QUEUE:
    default:
        event_type = VA_EVENT_QUEUE;
        break;
    }

    _va_send_event_packet(event_type, id, _va_get_timestamp_unlocked());
    VA_CS_EXIT();
#else
    VA_UNUSED(queueObject);
    VA_UNUSED(timeout);
#endif
}

void va_logQueueObjectBlocking(void *queueObject)
{
#if VA_HAS_RTOS
    if (queueObject == NULL)
        return;

    _va_service_pending_bundle();
    VA_CS_ENTER();

    uint8_t id = _va_find_queue_object_id(queueObject);
    if (id == 0)
    {
        VA_QueueObjectType_t type = va_adapter_get_queue_object_type(queueObject);
        id = _va_assign_queue_object_id(queueObject, NULL, type);
    }

    VA_QueueObjectType_t type = _va_get_stored_queue_object_type(queueObject);

    if (type == VA_OBJECT_TYPE_MUTEX || type == VA_OBJECT_TYPE_RECURSIVE_MUTEX)
    {
        va_adapter_check_mutex_contention(queueObject, id);
    }

    VA_CS_EXIT();
#else
    VA_UNUSED(queueObject);
#endif
}

/* ================================================================
 *  Heap alloc / free tracing
 * ================================================================ */

void va_logHeapAlloc(void *heapObject, uint32_t allocBytes)
{
#if VA_HAS_RTOS
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
#else
    VA_UNUSED(heapObject);
    VA_UNUSED(allocBytes);
#endif
}

void va_logHeapFree(void *heapObject, uint32_t allocatedBytes)
{
#if VA_HAS_RTOS
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
#else
    VA_UNUSED(heapObject);
    VA_UNUSED(allocatedBytes);
#endif
}

/* ================================================================
 *  User Event Logging
 * ================================================================ */

void VA_RegisterUserEvent(uint8_t id, const char *name)
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

void VA_RegisterUserFunction(uint8_t id, const char *name)
{
    VA_RegisterUserEvent(id, name);
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

void VA_LogUserEvent(uint8_t id, bool state)
{
    VA_LogEvent(id, state);
}

/* ================================================================
 *  Initialization
 * ================================================================ */

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

    /* Reset MRU lookup caches (stale slot indices from a previous run). */
    _va_task_cache_handle = NULL;
    _va_task_cache_idx    = -1;

#if VA_SEQ_COUNTER
    _va_seq = 0;
#endif

#if VA_AUTO_SETUP_INTERVAL_MS > 0
    _va_last_bundle_ts  = 0;
    _va_emitting_bundle = false;
    _va_bundle_due      = false;
    /* Precompute the bundle interval in cycles once (was recomputed per
       packet). Guard against cpu_freq == 0. */
    _va_interval_cycles = ((uint64_t)_va_cpu_freq / 1000) * VA_AUTO_SETUP_INTERVAL_MS;
#endif

#if VA_CAPTURE_STACK_USAGE
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

#if VA_HAS_RTOS
    for (int i = 0; i < VA_MAX_TASKS; ++i)
    {
        taskMap[i].active = false;
        taskMap[i].handle = NULL;
        taskMap[i].id = 0;
        taskMap[i].last_notifier = NULL;
        taskMap[i].lastStackUsed = 0;
        taskMap[i].lastStackEmitTs = 0;
        taskMap[i].hasStackSample = false;
    }
    next_task_id = 1;
    notificationValue = 0;

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

    for (int i = 0; i < VA_MAX_USER_EVENTS; ++i)
    {
        userEventMap[i].active = false;
        userEventMap[i].id = 0;
        userEventMap[i].name[0] = '\0';
    }

    _va_enable_dwt_counter();

#if VA_TRANSPORT_IS_ITM
    /* CoreSight software lock (Lock Access Register) exists only on ARMv7-M
       (Cortex-M3/M4/M7). ARMv8-M (Cortex-M23/M33/M55) removed it, and CMSIS-6
       drops LAR from ITM_Type — writing it there fails to compile. The unlock
       is a no-op on ARMv8-M, so skip it. */
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
    #endif // VA_RTT_BUFFER_SIZE > 0
#endif // VA_CONFIGURE_RTT
#elif VA_TRANSPORT_IS_CUSTOM
    // Nothing to init — user provides send function via VA_RegisterTransportSend()
#elif VA_TRANSPORT_IS_RAMBUF
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
#endif // VA_TRANSPORT
    VA_IS_INIT = true;

    _va_emit_sync_marker();

    /* Session-start marker — distinguishes a fresh VA_Init from periodic
       auto-bundle re-emissions (VA_EmitSetupBundle).  The host uses this
       to know that all subsequent data is from the current session, not
       stale bytes left over in the RTT ring buffer from a previous run. */
    _va_send_setup_packet(VA_SETUP_INFO, 0, "SES:START");

#if VA_SEQ_COUNTER
    /* MUST come after SES:START: the host resets its sequence expectation
       and checkpoint baseline on SES (new counter epoch, not loss). A
       checkpoint sent before it would be judged against the previous
       session's counter — a phantom loss burst on every re-init — and its
       baseline would be wiped by the epoch reset an instant later. */
    _va_send_seq_checkpoint();
#endif

    char info_buf[40];
    _va_u32_to_str(info_buf, sizeof(info_buf), "CLK:", _va_cpu_freq);
    _va_send_setup_packet(VA_SETUP_INFO, 0, info_buf);
    _va_send_setup_packet(VA_SETUP_ISR_MAP, VA_ISR_ID_SYSTICK, "SysTick");
#if (LOG_PENDSV == 1)
    _va_send_setup_packet(VA_SETUP_ISR_MAP, VA_ISR_ID_PENDSV, "PendSV");
#endif
#if (!VA_HAS_RTOS)
// TODO this is redundant with the OS info packet below, consider consolidating
    _va_send_setup_packet(VA_SETUP_CONFIG_FLAGS, 0, "NO_RTOS");
#endif

#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS)
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "FreeRTOS");
#elif (VA_RTOS_SELECT == VA_RTOS_ZEPHYR)
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "Zephyr");
#else
    _va_send_setup_packet(VA_SETUP_OS_INFO, 0, "BareMetal");
#endif

    VA_CS_EXIT();
}

#endif // DWT_NOT_AVAILABLE check
#endif // VA_ENABLED check

#ifdef __cplusplus
}
#endif
