// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* Appended to the pinned SDK-specific private initialization/IRQ adapter. */
/* This pinned HAL exposes DMA per-handle registration unconditionally. */
_Static_assert({{width_bytes}}U == 1U || {{width_bytes}}U == 2U || {{width_bytes}}U == 4U, "DMA width");
_Static_assert({{length_bytes}}U > 0U && {{length_bytes}}U <= 65535U &&
               {{length_bytes}}U % {{width_bytes}}U == 0U, "finite DMA length");
static bkpt_operation operation;
static volatile unsigned operation_complete;
static volatile unsigned operation_error;
static unsigned operation_initialized;
static void op_complete(hal_dma_handle_t *h) { (void)h; operation_complete = 1U; }
static void op_error(hal_dma_handle_t *h) { (void)h; operation_error = 1U; }
static void op_aborted(hal_dma_handle_t *h) { (void)h; }
static int op_context(void)
{
  return __get_IPSR() == 0U && __get_PRIMASK() == 0U &&
         __get_BASEPRI() == 0U && __get_FAULTMASK() == 0U;
}
static uint32_t op_lock(void)
{
  uint32_t saved = NVIC_GetEnableIRQ({{dma_irq}});
  HAL_CORTEX_NVIC_DisableIRQ({{dma_irq}});
  __DSB(); __ISB();
  return saved;
}
static void op_unlock(uint32_t saved)
{ if (saved) { HAL_CORTEX_NVIC_EnableIRQ({{dma_irq}}); } }
static bkpt_stop_state op_stopped(void)
{
  return HAL_DMA_GetState(&dma) == HAL_DMA_STATE_IDLE &&
    LL_DMA_IsActiveFlag_IDLE(HAL_DMA_GetLLInstance(&dma)) != 0U &&
    LL_DMA_IsEnabledChannel(HAL_DMA_GetLLInstance(&dma)) == 0U ? BKPT_STOPPED : BKPT_STOP_PENDING;
}
hal_status_t {{unit}}_init(void)
{
  if (!op_context()) { return HAL_BUSY; }
  if (operation_initialized) { return status; }
  operation_initialized = 1U;
  status = operation_hardware_init();
  if (status != HAL_OK) { return status; }
  if (HAL_DMA_RegisterXferCpltCallback(&dma, op_complete) != HAL_OK ||
      HAL_DMA_RegisterXferErrorCallback(&dma, op_error) != HAL_OK ||
      HAL_DMA_RegisterXferAbortCallback(&dma, op_aborted) != HAL_OK) { status = HAL_ERROR; }
  return status;
}
int {{unit}}_start(const void *src, void *dst, uint32_t now, uint32_t timeout, uint32_t recovery)
{
  uintptr_t s = (uintptr_t)src, d = (uintptr_t)dst;
  const uintptr_t n = {{length_bytes}}U;
  if (!operation_initialized || status != HAL_OK || !op_context() || src == NULL || dst == NULL ||
      s % {{width_bytes}}U != 0U || d % {{width_bytes}}U != 0U ||
      s > UINT32_MAX || d > UINT32_MAX || n > UINT32_MAX - s || n > UINT32_MAX - d ||
      (s < d + n && d < s + n) ||
      !bkpt_operation_can_begin(&operation, timeout, recovery)) { return 1; }
  uint32_t saved = op_lock();
  if (op_stopped() != BKPT_STOPPED) { op_unlock(saved); return 1; }
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma), LL_DMA_FLAG_ALL);
  HAL_CORTEX_NVIC_ClearPendingIRQ({{dma_irq}});
  operation_complete = operation_error = 0U;
  (void)bkpt_operation_begin(&operation, now, timeout, recovery);
  if (HAL_DMA_StartDirectXfer_IT(&dma, (uint32_t)s, (uint32_t)d, {{length_bytes}}U) != HAL_OK) {
    bkpt_operation_stop(&operation, now, BKPT_OP_START_ERROR);
  }
  op_unlock(saved);
  return 0;
}
bkpt_op_result {{unit}}_poll(uint32_t now)
{
  if (!operation_initialized || status != HAL_OK || !op_context()) {
    return bkpt_operation_result(&operation);
  }
  uint32_t saved = op_lock();
  bkpt_op_result result = bkpt_operation_observe(&operation, now,
    operation_complete, operation_error, op_stopped());
  if (result.state == BKPT_OP_STOPPING) {
    hal_dma_state_t state = HAL_DMA_GetState(&dma);
    if (state == HAL_DMA_STATE_IDLE || state == HAL_DMA_STATE_ACTIVE) {
      (void)HAL_DMA_Abort_IT(&dma);
    }
    result = bkpt_operation_observe(&operation, now,
      operation_complete, operation_error, op_stopped());
  }
  op_unlock(saved);
  return result;
}
bkpt_op_result {{unit}}_cancel(uint32_t now)
{
  if (!operation_initialized || status != HAL_OK || !op_context()) {
    return bkpt_operation_result(&operation);
  }
  (void){{unit}}_poll(now);
  bkpt_operation_stop(&operation, now, BKPT_OP_CANCELLED);
  return {{unit}}_poll(now);
}
