// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#ifndef BKPT_FINITE_OPERATION_H
#define BKPT_FINITE_OPERATION_H
#include <stdint.h>

/* Portable foreground lifecycle. Adapters own hardware, IRQ synchronization and
 * proof of quiescence. No HAL/CMSIS types, allocation, retry or blocking waits.
 * Poll at least once per clock wrap. Durations are 1..INT32_MAX ticks; deadlines
 * are observed at poll time, not a hard real-time response guarantee.
 * At the operation deadline an error wins, then timeout, then completion.
 * At the recovery deadline proven quiescence wins over UNSAFE. These are
 * observation rules, not timestamps of physical completion. */
typedef enum {
  BKPT_OP_READY, BKPT_OP_ACTIVE, BKPT_OP_STOPPING,
  BKPT_OP_DONE, BKPT_OP_FAILED, BKPT_OP_UNSAFE
} bkpt_op_state;
typedef enum {
  BKPT_OP_NO_ERROR, BKPT_OP_IO_ERROR, BKPT_OP_TIMEOUT,
  BKPT_OP_CANCELLED, BKPT_OP_START_ERROR
} bkpt_op_reason;
typedef enum { BKPT_STOP_UNKNOWN, BKPT_STOP_PENDING, BKPT_STOPPED } bkpt_stop_state;
typedef struct {
  bkpt_op_state state;
  bkpt_op_reason reason;
  uint32_t started;
  uint32_t stop_started;
  uint32_t timeout;
  uint32_t recovery;
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
static inline int bkpt_operation_can_begin(const bkpt_operation *op,
                                           uint32_t timeout, uint32_t recovery)
{
  /* Failure is terminal for this instance. No implicit recovery/rearm. */
  return (op->state == BKPT_OP_READY || op->state == BKPT_OP_DONE) &&
         timeout > 0U && timeout <= INT32_MAX && recovery > 0U && recovery <= INT32_MAX;
}
static inline int bkpt_operation_begin(bkpt_operation *op, uint32_t now,
                                       uint32_t timeout, uint32_t recovery)
{
  if (!bkpt_operation_can_begin(op, timeout, recovery)) { return 0; }
  op->state = BKPT_OP_ACTIVE;
  op->reason = BKPT_OP_NO_ERROR;
  op->started = now;
  op->timeout = timeout;
  op->recovery = recovery;
  return 1;
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
    else if ((uint32_t)(now - op->started) >= op->timeout) {
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
