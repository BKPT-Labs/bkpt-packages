/**
 * @file VA_Internal.h
 * @brief ViewAlyzer Internal API - shared between the core engine and RTOS adapters.
 *
 * NOT part of the public user API: packet helpers, timestamp engine, and
 * registry structures shared with the RTOS adapters. The only ViewAlyzer
 * header that pulls in the device/CMSIS header.
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

#ifndef VA_INTERNAL_H
#define VA_INTERNAL_H

#include "ViewAlyzer.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if (VA_ENABLED == 1)

/* Device/CMSIS header (ITM / DWT / __get_PRIMASK / __DMB). Pass a BARE
   token, never a quoted string:
       -DVA_DEVICE_HEADER=stm32g474xx.h   (Zephyr sets cmsis_core.h) */
#if !defined(VA_DEVICE_HEADER)
#error "ViewAlyzer: set the device header, e.g. -DVA_DEVICE_HEADER=stm32g474xx.h (Zephyr: cmsis_core.h)"
#endif
#include VA_STR(VA_DEVICE_HEADER)

#ifdef __cplusplus
extern "C" {
#endif

/* ── Critical-section helpers (preserve PRIMASK) ───────────────── */
#if VA_ALLOWED_TO_DISABLE_INTERRUPTS
#define VA_CS_ENTER()                        \
    uint32_t __va_primask = __get_PRIMASK(); \
    __disable_irq()
#define VA_CS_EXIT() __set_PRIMASK(__va_primask)
#else
#define VA_CS_ENTER() ((void)0)
#define VA_CS_EXIT()  ((void)0)
#endif

/* Run statements inside their own short critical section (own scope, so
   several uses can share a block). */
#define VA_ATOMIC(...) do { VA_CS_ENTER(); __VA_ARGS__; VA_CS_EXIT(); } while (0)

#define VA_UNUSED(x) (void)(x)

/* ── Task / object map entry (RTOS-agnostic) ─────────────────────
   Per-entry fields follow the enabled categories so disabled categories
   cost no .bss. */
#if VA_NEEDS_TASK_REGISTRY
typedef struct {
    void    *handle;                        /* Generic pointer to TCB / thread struct */
    uint8_t  id;                            /* ViewAlyzer internal ID                 */
    char     name[VA_MAX_TASK_NAME_LEN];
    bool     active;
#if VA_TRACE_TASK_NOTIFICATIONS
    void    *last_notifier;                 /* For notification tracking              */
#endif
#if VA_TRACE_TASKS
    uint32_t uxPriority;                    /* TASK_CREATE payload                    */
    uint32_t uxBasePriority;
#endif
#if VA_TRACE_TASKS || VA_TRACE_STACK_USAGE
    uint32_t ulStackDepth;                  /* total stack, in the adapter's units     */
#endif
#if VA_TRACE_STACK_USAGE
    /* Stack monitoring - adapters populate these during task create */
    void    *pxStack;
    void    *pxEndOfStack;
    /* Stack-usage heartbeat pacing (see va_taskswitchedout) */
    uint64_t lastStackEmitTs;               /* cycle count of last emission   */
    bool     hasStackSample;                /* false until first emission     */
#endif
#if (VA_RTOS_SELECT == VA_RTOS_FREERTOS) && VA_TRACE_SLEEP
    /* FreeRTOS has no delay/suspend EXIT trace point; the flag carries the
       sleep across to the next switch-in or resume, which emits the exit. */
    bool     sleeping;
#endif
} VA_TaskMapEntry_t;
#endif /* VA_NEEDS_TASK_REGISTRY */

/* ── Queue / sync-object map entry (RTOS-agnostic) ─────────────── */
#if VA_NEEDS_OBJECT_REGISTRY
typedef struct {
    void               *handle;
    uint8_t             id;
    char                name[VA_MAX_TASK_NAME_LEN];
    VA_QueueObjectType_t type;
    bool                active;
#if VA_HAS_RTOS && VA_TRACE_RTOS_HEAPS
    uint32_t            heapCapacity;    /* bytes; 0 = unknown. Re-emitted with
                                            each setup bundle so late-attaching
                                            hosts learn heap capacity. */
#endif
} VA_QueueObjectMapEntry_t;
#endif

/* ── Shared global state (defined in ViewAlyzer.c) ─────────────── */
extern volatile bool      VA_IS_INIT;

#if VA_NEEDS_TASK_REGISTRY
extern VA_TaskMapEntry_t  taskMap[VA_MAX_TASKS];
extern uint8_t            next_task_id;
/* The g_task_* creation scratch globals are declared in ViewAlyzer.h - the
   FreeRTOS hook header needs them and cannot include this file. */
#endif

#if VA_NEEDS_OBJECT_REGISTRY
extern VA_QueueObjectMapEntry_t queueObjectMap[VA_MAX_SYNC_OBJECTS];
extern uint8_t                  next_queue_object_id;
#endif

/* ── Packet emission helpers (defined in ViewAlyzer.c) ──────────── */
uint64_t _va_get_timestamp(void);
/* Same as _va_get_timestamp() but WITHOUT the PRIMASK save/disable/restore.
   Call ONLY with interrupts already masked (inside a VA_CS_ENTER section). */
uint64_t _va_get_timestamp_unlocked(void);

/* Emit the periodic setup bundle if one is due. MUST be called from a
   thread context that does NOT currently hold a VA critical section - it
   emits each bundle packet under its own short CS so interrupts are serviced
   between packets. No-op in ISR context or when no bundle is due. */
void _va_service_pending_bundle(void);

/* Emit one packet. The byte at data[1] is the reserved sequence slot and is
   overwritten here - builders must lay packets out as [type][seq][rest...]
   and never pass read-only storage. */
void _va_emit_packet(uint8_t *data, uint32_t length);
void _va_send_event_packet(uint8_t type_byte, uint8_t id, uint64_t timestamp);
void _va_send_setup_packet(uint8_t setupCode, uint8_t id, const char *name);
void _va_send_data_event_packet(uint8_t type_byte, uint8_t id, uint32_t value, uint64_t timestamp);

#if VA_NEEDS_USER_TRACE_REGISTRY
void _va_send_user_setup_packet(uint8_t id, uint8_t type, const char *name);
#endif
#if VA_TRACE_USER_VALUES
void _va_send_user_event_packet(uint8_t id, int32_t value, uint64_t timestamp);
void _va_send_float_event_packet(uint8_t id, float value, uint64_t timestamp);
void _va_send_user_toggle_event_packet(uint8_t id, VA_UserToggleState_t state, uint64_t timestamp);
#endif
#if VA_HAS_RTOS && VA_TRACE_TASK_NOTIFICATIONS
void _va_send_notification_event_packet(uint8_t type_byte, uint8_t id, uint8_t other_id, uint32_t value, uint64_t timestamp);
#endif
#if VA_NEEDS_BLOCKING_HOOK
void _va_send_mutex_contention_packet(uint8_t mutex_id, uint8_t waiting_task_id, uint8_t holder_task_id, uint64_t timestamp);
#endif
#if VA_NEEDS_TASK_SWITCH_EVENTS
void _va_send_task_create_packet(uint8_t id, uint64_t timestamp, uint32_t priority, uint32_t base_priority, uint32_t stack_size);
#endif
#if VA_HAS_RTOS && VA_TRACE_STACK_USAGE
void _va_send_stack_usage_packet(uint8_t id, uint64_t timestamp, uint32_t stack_used, uint32_t stack_total);
#endif
#if VA_TRACE_HEAP_METRICS
void _va_send_heap_setup_packet(uint8_t id, const char *name, uint32_t totalSize);
#endif

/* ── Generic map helpers (defined in ViewAlyzer.c) ──────────────── */
#if VA_NEEDS_TASK_REGISTRY
uint8_t _va_find_task_id(void *handle);
int     _va_find_task_index(void *handle);
uint8_t _va_assign_task_id(void *handle, const char *name);
#endif

#if VA_NEEDS_OBJECT_REGISTRY
uint8_t              _va_find_queue_object_id(void *handle);
int                  _va_find_queue_object_index(void *handle);
uint8_t              _va_assign_queue_object_id(void *handle, const char *name, VA_QueueObjectType_t type);
const char          *_va_get_object_type_name(VA_QueueObjectType_t type);
uint8_t              _va_get_setup_packet_type(VA_QueueObjectType_t type);
VA_QueueObjectType_t _va_get_stored_queue_object_type(void *handle);
#endif

#ifdef __cplusplus
}
#endif

#endif /* VA_ENABLED */

#endif /* VA_INTERNAL_H */
