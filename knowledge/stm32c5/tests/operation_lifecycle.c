// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#include "../catalog/components/operation.h"
#include <assert.h>

/* A non-ST adapter: completions and quiescence are independent observations. */
struct fake_device { unsigned done, error; bkpt_stop_state ownership; };
static bkpt_op_result poll_fake(bkpt_operation *op, struct fake_device *d, uint32_t now)
{ return bkpt_operation_observe(op, now, d->done, d->error, d->ownership); }
int main(void)
{
  bkpt_operation op = {0};
  struct fake_device d = {0,0,BKPT_STOP_PENDING};
  assert(bkpt_operation_result(&op).buffers_released);
  assert(!bkpt_operation_begin(&op,0,0,10));
  assert(!bkpt_operation_begin(&op,0,UINT32_MAX,10));
  assert(bkpt_operation_begin(&op,UINT32_MAX-5U,10,4));
  assert(!bkpt_operation_begin(&op,0,10,4));
  assert(poll_fake(&op,&d,3).state == BKPT_OP_ACTIVE);
  assert(poll_fake(&op,&d,4).state == BKPT_OP_STOPPING);
  assert(op.reason == BKPT_OP_TIMEOUT);
  d.ownership=BKPT_STOP_UNKNOWN;
  assert(!poll_fake(&op,&d,7).buffers_released);
  assert(poll_fake(&op,&d,8).state == BKPT_OP_UNSAFE);
  d.ownership=BKPT_STOPPED;
  assert(!poll_fake(&op,&d,9).buffers_released);
  assert(!bkpt_operation_begin(&op,10,10,4));
  op=(bkpt_operation){0};
  assert(bkpt_operation_begin(&op,0,10,4));
  d=(struct fake_device){1,0,BKPT_STOP_PENDING};
  assert(!poll_fake(&op,&d,1).buffers_released);
  d.ownership=BKPT_STOPPED;
  assert(poll_fake(&op,&d,2).state == BKPT_OP_DONE);
  assert(bkpt_operation_begin(&op,3,10,4));
  d=(struct fake_device){1,1,BKPT_STOPPED};
  assert(poll_fake(&op,&d,4).state == BKPT_OP_FAILED);
  assert(op.reason == BKPT_OP_IO_ERROR);
  assert(bkpt_operation_result(&op).buffers_released);
  assert(!bkpt_operation_begin(&op,5,10,4));
  op=(bkpt_operation){0};
  assert(bkpt_operation_begin(&op,0,10,4));
  bkpt_operation_stop(&op,1,BKPT_OP_CANCELLED);
  bkpt_operation_stop(&op,3,BKPT_OP_START_ERROR);
  assert(op.stop_started==1 && op.reason==BKPT_OP_CANCELLED);
  d=(struct fake_device){0,0,BKPT_STOP_PENDING};
  assert(!poll_fake(&op,&d,4).buffers_released);
  d.ownership=BKPT_STOPPED;
  assert(poll_fake(&op,&d,5).state==BKPT_OP_FAILED);
  op=(bkpt_operation){0};
  bkpt_operation_stop(&op,0,BKPT_OP_CANCELLED);
  assert(op.state==BKPT_OP_READY);
  assert(bkpt_operation_begin(&op,0,10,4));
  d=(struct fake_device){1,0,BKPT_STOP_UNKNOWN};
  assert(poll_fake(&op,&d,9).state==BKPT_OP_ACTIVE);
  d.ownership=BKPT_STOPPED;
  assert(poll_fake(&op,&d,10).state==BKPT_OP_FAILED);
  assert(op.reason==BKPT_OP_TIMEOUT);
  op=(bkpt_operation){0};
  assert(bkpt_operation_begin(&op,0,10,4));
  assert(poll_fake(&op,&d,1).state==BKPT_OP_DONE);
  bkpt_operation_stop(&op,2,BKPT_OP_CANCELLED);
  assert(op.state==BKPT_OP_DONE);
  op=(bkpt_operation){0};
  assert(bkpt_operation_begin(&op,UINT32_MAX-2U,1,4));
  d=(struct fake_device){0,0,BKPT_STOP_PENDING};
  assert(poll_fake(&op,&d,UINT32_MAX-1U).state==BKPT_OP_STOPPING);
  assert(poll_fake(&op,&d,1).state==BKPT_OP_STOPPING);
  d.ownership=BKPT_STOPPED;
  assert(poll_fake(&op,&d,2).state==BKPT_OP_FAILED);
  /* Explicit no-deadline policy is distinct from invalid finite timeout zero. */
  bkpt_op_deadline indefinite = {BKPT_DEADLINE_NONE, 0U};
  bkpt_op_deadline invalid_kind = {(bkpt_op_deadline_kind)99, 0U};
  bkpt_op_deadline invalid_ticks = {BKPT_DEADLINE_NONE, 1U};
  op=(bkpt_operation){0};
  assert(!bkpt_operation_begin_with_deadline(&op,0,invalid_kind,4));
  assert(!bkpt_operation_begin_with_deadline(&op,0,invalid_ticks,4));
  assert(!bkpt_operation_begin_with_deadline(&op,0,indefinite,0));
  assert(!bkpt_operation_begin_with_deadline(&op,0,indefinite,UINT32_MAX));
  assert(op.state==BKPT_OP_READY);
  assert(!bkpt_operation_begin(&op,0,0,4));
  assert(bkpt_operation_begin_with_deadline(&op,0,indefinite,4));
  assert(op.deadline_kind==BKPT_DEADLINE_NONE);
  d=(struct fake_device){0,0,BKPT_STOP_PENDING};
  for (unsigned wrap=0;wrap<3;wrap++) {
    assert(poll_fake(&op,&d,INT32_MAX).state==BKPT_OP_ACTIVE);
    assert(poll_fake(&op,&d,UINT32_MAX).state==BKPT_OP_ACTIVE);
    assert(poll_fake(&op,&d,1).state==BKPT_OP_ACTIVE);
    assert(!bkpt_operation_result(&op).buffers_released);
  }
  d.done=1;
  assert(poll_fake(&op,&d,2).state==BKPT_OP_ACTIVE);
  d.ownership=BKPT_STOPPED;
  assert(poll_fake(&op,&d,3).state==BKPT_OP_DONE);
  /* An old finite caller after indefinite DONE must restore its own deadline. */
  assert(bkpt_operation_begin(&op,4,2,4));
  assert(op.deadline_kind==BKPT_DEADLINE_FINITE);
  d=(struct fake_device){0,0,BKPT_STOP_PENDING};
  assert(poll_fake(&op,&d,5).state==BKPT_OP_ACTIVE);
  assert(poll_fake(&op,&d,6).state==BKPT_OP_STOPPING);
  assert(op.reason==BKPT_OP_TIMEOUT);
  assert(poll_fake(&op,&d,10).state==BKPT_OP_UNSAFE);
  for (unsigned cancel=0;cancel<2;cancel++) {
    op=(bkpt_operation){0};
    assert(bkpt_operation_begin_with_deadline(&op,0,indefinite,4));
    d=(struct fake_device){1,!cancel,BKPT_STOP_PENDING};
    if (cancel) { bkpt_operation_stop(&op,UINT32_MAX-2U,BKPT_OP_CANCELLED); }
    assert(poll_fake(&op,&d,UINT32_MAX-2U).state==BKPT_OP_STOPPING);
    assert(op.reason==(cancel?BKPT_OP_CANCELLED:BKPT_OP_IO_ERROR));
    assert(!poll_fake(&op,&d,0).buffers_released);
    d.ownership=BKPT_STOPPED;
    assert(poll_fake(&op,&d,1).state==BKPT_OP_FAILED);
    assert(bkpt_operation_result(&op).buffers_released);
    assert(!bkpt_operation_begin_with_deadline(&op,2,indefinite,4));
  }
  op=(bkpt_operation){0};
  assert(bkpt_operation_begin_with_deadline(&op,0,indefinite,4));
  bkpt_operation_stop(&op,UINT32_MAX-2U,BKPT_OP_CANCELLED);
  d=(struct fake_device){0,0,BKPT_STOP_PENDING};
  assert(poll_fake(&op,&d,1).state==BKPT_OP_UNSAFE);
  d.ownership=BKPT_STOPPED;
  assert(!poll_fake(&op,&d,2).buffers_released);
  return 0;
}
