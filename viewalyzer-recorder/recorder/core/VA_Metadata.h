/* SPDX-License-Identifier: Apache-2.0 */
/* Private implementation, included once by ViewAlyzer.c.
   All firmware accesses run under the recorder critical section. */
typedef struct
{
    char magic[16];
    uint32_t version, tableAddr, capacity;
    volatile uint32_t used, generation, flags;
    volatile uint32_t request, requestGeneration; /* host-owned */
    volatile uint32_t ack, ackGeneration, ackSequence, ackStatus;
} VA_MetadataControl_t;
typedef char va_metadata_abi_size[(sizeof(VA_MetadataControl_t) == 64) ? 1 : -1];
VA_RAMBUF_ATTRIBUTES VA_MetadataControl_t _VA_METADATA __attribute__((aligned(4)));
static VA_RAMBUF_ATTRIBUTES uint8_t _va_metadata_bytes[VA_METADATA_SIZE];
static uint64_t _va_metadata_last_checkpoint;
static uint64_t _va_metadata_heartbeat;
static uint8_t _va_metadata_running;
static void _va_metadata_request(uint32_t request) __attribute__((noinline));
static inline void _va_metadata_poll(void)
{
    uint32_t request = _VA_METADATA.request;
    if (request != _VA_METADATA.ack && VA_IS_INIT)
        _va_metadata_request(request);
}

static uint32_t _va_metadata_length(uint32_t off)
{
    return (uint32_t)_va_metadata_bytes[off] | ((uint32_t)_va_metadata_bytes[off + 1] << 8);
}

static bool _va_metadata_object_name(uint8_t type)
{
    return type == VA_SETUP_SEMAPHORE_MAP || type == VA_SETUP_MUTEX_MAP ||
           type == VA_SETUP_QUEUE_MAP || (type >= VA_SETUP_TIMER_MAP && type <= VA_SETUP_PM_MAP);
}

static void _va_metadata_begin(void)
{
    _VA_METADATA.generation++;
    __DMB();
}

static void _va_metadata_end(void)
{
    uint32_t next = _VA_METADATA.generation + 1u;
    /* Zero is the reset witness in the host-owned requestGeneration word. */
    if (next == 0u) next = 2u;
    __DMB();
    _VA_METADATA.generation = next;
}

/* Cache only registration packets, never scan this table for ordinary events.
   A record is [u16 length][wire packet]. Sequence and creation time are zero:
   the host synthesizes their attach-time values when persisting the prelude. */
static __attribute__((noinline)) void _va_metadata_store(const uint8_t *data, uint32_t length)
{
    if (!VA_IS_INIT || length < 3u || length > 65535u)
        return;
    if (data[0] == VA_SETUP_INFO && length >= 8u && memcmp(data + 4, "SES:", 4) == 0)
        return;
    uint32_t off = 0, old = 0, used = _VA_METADATA.used;
    while (off < used)
    {
        uint32_t n = _va_metadata_length(off);
        const uint8_t *p = &_va_metadata_bytes[off + 2];
        bool same = (p[0] == data[0] || (_va_metadata_object_name(p[0]) && _va_metadata_object_name(data[0]))) && p[2] == data[2];
        if (same && data[0] == VA_SETUP_OBJECT_INFO)
            same = p[3] == data[3];
        if (same && data[0] == VA_SETUP_INFO)
            same = (n == length && memcmp(p + 3, data + 3, length - 3) == 0) ||
                   (n >= 8u && length >= 8u && memcmp(p + 4, "CLK:", 4) == 0 && memcmp(data + 4, "CLK:", 4) == 0);
        if (same) { old = n + 2; break; }
        off += n + 2;
    }
    if (used - old + length + 2 > VA_METADATA_SIZE)
    {
        /* Latched: never silently present an incomplete table as complete. */
        _va_metadata_begin();
        _VA_METADATA.flags |= 1u;
        _va_metadata_end();
        return;
    }
    _va_metadata_begin();
    memmove(&_va_metadata_bytes[off + length + 2], &_va_metadata_bytes[off + old], used - off - old);
    _va_metadata_bytes[off] = (uint8_t)length;
    _va_metadata_bytes[off + 1] = (uint8_t)(length >> 8);
    memcpy(&_va_metadata_bytes[off + 2], data, length);
    _va_metadata_bytes[off + 3] = 0;
    if (data[0] == VA_EVENT_TASK_CREATE)
        memset(&_va_metadata_bytes[off + 5], 0, 4);
    _VA_METADATA.used = used - old + length + 2;
    _va_metadata_end();
}

#if VA_NEEDS_TASK_REGISTRY || VA_NEEDS_OBJECT_REGISTRY
static void _va_metadata_remove(uint8_t id, bool task)
{
    uint32_t off = 0;
    _va_metadata_begin();
    while (off < _VA_METADATA.used)
    {
        uint32_t n = _va_metadata_length(off) + 2;
        uint8_t *p = &_va_metadata_bytes[off + 2];
        bool match = task ? (p[0] == VA_SETUP_TASK_MAP || p[0] == VA_EVENT_TASK_CREATE)
                          : (_va_metadata_object_name(p[0]) || p[0] == VA_SETUP_OBJECT_INFO);
        if (match && p[2] == id)
        {
            memmove(&_va_metadata_bytes[off], &_va_metadata_bytes[off + n], _VA_METADATA.used - off - n);
            _VA_METADATA.used -= n;
        }
        else off += n;
    }
    _va_metadata_end();
}
#endif
