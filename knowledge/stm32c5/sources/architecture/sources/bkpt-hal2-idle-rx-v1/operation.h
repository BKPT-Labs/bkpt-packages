// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#ifndef BKPT_FINITE_OPERATION_H
#define BKPT_FINITE_OPERATION_H
#include <stdint.h>

/* Portable foreground lifecycle. Adapters own hardware, IRQ synchronization and
 * proof of quiescence. No HAL/CMSIS types, allocation, retry or blocking waits.
 * During finite ACTIVE and all STOPPING states, poll at least once per clock
 * wrap. Finite durations are 1..INT32_MAX ticks. Deadlines are observed at poll
 * time, not a hard real-time response guarantee. Indefinite ACTIVE has no clock
 * expiry, but still needs polling to observe progress and errors.
 * At the operation deadline an error wins, then timeout, then completion.
 * At the recovery deadline proven quiescence wins over UNSAFE. These are
 * observation rules, not timestamps of physical completion. An explicit NONE
 * operation deadline permits indefinite waiting, never indefinite recovery.
 * It does not disable completion, error observation, cancellation or ownership. */
typedef enum {
  BKPT_OP_READY, BKPT_OP_ACTIVE, BKPT_OP_STOPPING,
  BKPT_OP_DONE, BKPT_OP_FAILED, BKPT_OP_UNSAFE
} bkpt_op_state;
typedef enum {
  BKPT_OP_NO_ERROR, BKPT_OP_IO_ERROR, BKPT_OP_TIMEOUT,
  BKPT_OP_CANCELLED, BKPT_OP_START_ERROR
} bkpt_op_reason;
typedef enum { BKPT_STOP_UNKNOWN, BKPT_STOP_PENDING, BKPT_STOPPED } bkpt_stop_state;
typedef enum { BKPT_DEADLINE_FINITE, BKPT_DEADLINE_NONE } bkpt_op_deadline_kind;
typedef struct {
  bkpt_op_deadline_kind kind;
  uint32_t ticks; /* NONE requires zero; zero is never a valid finite duration. */
} bkpt_op_deadline;
typedef struct {
  bkpt_op_state state;
  bkpt_op_reason reason;
  uint32_t started;
  uint32_t stop_started;
  uint32_t timeout;
  uint32_t recovery;
  bkpt_op_deadline_kind deadline_kind;
} bkpt_operation;
typedef struct {
  bkpt_op_state state;
  bkpt_op_reason reason;
  unsigned buffers_released;
} bkpt_op_result;

static inline bkpt_op_result bkpt_operation_result(const bkpt_operation *op)
{
  bkpt_op_result r = {op->state, op->reason,
    op->state == BKPT_OP_READY || op->state == BKPT_OP_DONE || op->state == BKPT_OP_FAILED};
  return r;
}
static inline int bkpt_operation_can_begin_with_deadline(const bkpt_operation *op,
                                           bkpt_op_deadline deadline, uint32_t recovery)
{
  /* Failure is terminal for this instance. No implicit recovery/rearm. */
  return (op->state == BKPT_OP_READY || op->state == BKPT_OP_DONE) &&
         recovery > 0U && recovery <= INT32_MAX &&
         ((deadline.kind == BKPT_DEADLINE_FINITE &&
           deadline.ticks > 0U && deadline.ticks <= INT32_MAX) ||
          (deadline.kind == BKPT_DEADLINE_NONE && deadline.ticks == 0U));
}
static inline int bkpt_operation_begin_with_deadline(bkpt_operation *op, uint32_t now,
                                       bkpt_op_deadline deadline, uint32_t recovery)
{
  if (!bkpt_operation_can_begin_with_deadline(op, deadline, recovery)) { return 0; }
  op->state = BKPT_OP_ACTIVE;
  op->reason = BKPT_OP_NO_ERROR;
  op->started = now;
  op->timeout = deadline.ticks;
  op->recovery = recovery;
  op->deadline_kind = deadline.kind;
  return 1;
}
/* Existing finite callers retain their signatures and validation. */
static inline int bkpt_operation_can_begin(const bkpt_operation *op,
                                           uint32_t timeout, uint32_t recovery)
{
  bkpt_op_deadline deadline = {BKPT_DEADLINE_FINITE, timeout};
  return bkpt_operation_can_begin_with_deadline(op, deadline, recovery);
}
static inline int bkpt_operation_begin(bkpt_operation *op, uint32_t now,
                                       uint32_t timeout, uint32_t recovery)
{
  bkpt_op_deadline deadline = {BKPT_DEADLINE_FINITE, timeout};
  return bkpt_operation_begin_with_deadline(op, now, deadline, recovery);
}
static inline void bkpt_operation_stop(bkpt_operation *op, uint32_t now,
                                       bkpt_op_reason reason)
{
  if (op->state == BKPT_OP_ACTIVE) {
    op->state = BKPT_OP_STOPPING;
    op->reason = reason;
    op->stop_started = now;
  }
}
static inline bkpt_op_result bkpt_operation_observe(bkpt_operation *op, uint32_t now,
    unsigned complete, unsigned error, bkpt_stop_state stopped)
{
  if (op->state == BKPT_OP_ACTIVE) {
    /* Errors win a simultaneous observation. Completion alone never frees DMA. */
    if (error) { bkpt_operation_stop(op, now, BKPT_OP_IO_ERROR); }
    else if (op->deadline_kind == BKPT_DEADLINE_FINITE &&
             (uint32_t)(now - op->started) >= op->timeout) {
      bkpt_operation_stop(op, now, BKPT_OP_TIMEOUT);
    }
    else if (complete && stopped == BKPT_STOPPED) { op->state = BKPT_OP_DONE; }
  }
  if (op->state == BKPT_OP_STOPPING) {
    if (stopped == BKPT_STOPPED) { op->state = BKPT_OP_FAILED; }
    else if ((uint32_t)(now - op->stop_started) >= op->recovery) {
      op->state = BKPT_OP_UNSAFE;
    }
  }
  return bkpt_operation_result(op);
}
#endif
