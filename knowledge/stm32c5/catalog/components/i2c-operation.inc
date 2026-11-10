// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* Appended to the pinned SDK-specific private initialization/IRQ adapter. */
#if USE_HAL_I2C_REGISTER_CALLBACKS != 1 || USE_HAL_I2C_DMA != 1
#error "Managed I2C requires per-handle callbacks and DMA support"
#endif
_Static_assert({{length_bytes}}U > 0U && {{length_bytes}}U <= 255U, "finite I2C length");
_Static_assert({{device_address_7bit}}U <= 127U, "7-bit I2C address");
#define OP_I2C_IRQS (I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_ADDRIE | I2C_CR1_NACKIE | I2C_CR1_STOPIE | I2C_CR1_TCIE | I2C_CR1_ERRIE)
static bkpt_operation operation;
static volatile unsigned operation_complete;
static volatile unsigned operation_error;
static unsigned operation_initialized;
static void op_complete(hal_i2c_handle_t *h) { (void)h; operation_complete = 1U; }
static void op_error(hal_i2c_handle_t *h) { (void)h; operation_error = 1U; }
static void op_aborted(hal_i2c_handle_t *h) { (void)h; }
static void op_orphan_aborted(hal_dma_handle_t *h) { (void)h; }
static int op_context(void)
{
  return __get_IPSR() == 0U && __get_PRIMASK() == 0U &&
         __get_BASEPRI() == 0U && __get_FAULTMASK() == 0U;
}
typedef struct { uint32_t tx, rx, event, error; } op_irq_state;
static op_irq_state op_lock(void)
{
  op_irq_state saved = {NVIC_GetEnableIRQ({{tx_dma_irq}}), NVIC_GetEnableIRQ({{rx_dma_irq}}),
    NVIC_GetEnableIRQ({{event_irq}}), NVIC_GetEnableIRQ({{error_irq}})};
  HAL_CORTEX_NVIC_DisableIRQ({{tx_dma_irq}});
  HAL_CORTEX_NVIC_DisableIRQ({{rx_dma_irq}});
  HAL_CORTEX_NVIC_DisableIRQ({{event_irq}});
  HAL_CORTEX_NVIC_DisableIRQ({{error_irq}});
  __DSB(); __ISB();
  return saved;
}
static void op_unlock(op_irq_state saved)
{
  if (saved.tx) { HAL_CORTEX_NVIC_EnableIRQ({{tx_dma_irq}}); }
  if (saved.rx) { HAL_CORTEX_NVIC_EnableIRQ({{rx_dma_irq}}); }
  if (saved.event) { HAL_CORTEX_NVIC_EnableIRQ({{event_irq}}); }
  if (saved.error) { HAL_CORTEX_NVIC_EnableIRQ({{error_irq}}); }
}
static int op_dma_stopped(const hal_dma_handle_t *d)
{
  return HAL_DMA_GetState(d) == HAL_DMA_STATE_IDLE &&
    LL_DMA_IsActiveFlag_IDLE(HAL_DMA_GetLLInstance(d)) != 0U &&
    LL_DMA_IsEnabledChannel(HAL_DMA_GetLLInstance(d)) == 0U;
}
/* Called after each owned HAL IRQ as well as under the foreground IRQ lock.
 * The pinned HAL DMA completion may re-enable STOP/TC after an error made the
 * peripheral idle. Disable those orphan sources before returning from the IRQ.
 * Never disturb the peripheral's active/abort state machine. */
static void operation_irq_settle(void)
{
  if (HAL_I2C_GetState(&peripheral) == HAL_I2C_STATE_IDLE &&
      peripheral.mode == HAL_I2C_MODE_NONE && peripheral.xfer_isr == NULL) {
    I2C_TypeDef *i2c = (I2C_TypeDef *)(uintptr_t)peripheral.instance;
    LL_I2C_DisableIT(i2c, OP_I2C_IRQS);
    LL_I2C_DisableDMAReq_TX(i2c);
    LL_I2C_DisableDMAReq_RX(i2c);
  }
}
static bkpt_stop_state op_stopped(void)
{
  I2C_TypeDef *i2c = (I2C_TypeDef *)(uintptr_t)peripheral.instance;
  return HAL_I2C_GetState(&peripheral) == HAL_I2C_STATE_IDLE &&
    peripheral.xfer_isr == NULL && peripheral.mode == HAL_I2C_MODE_NONE &&
    (i2c->CR1 & OP_I2C_IRQS) == 0U &&
    LL_I2C_IsEnabledDMAReq_TX(i2c) == 0U && LL_I2C_IsEnabledDMAReq_RX(i2c) == 0U &&
    op_dma_stopped(&dma_tx) && op_dma_stopped(&dma_rx) ? BKPT_STOPPED : BKPT_STOP_PENDING;
}
static void op_stop_step(void)
{
  hal_i2c_state_t state = HAL_I2C_GetState(&peripheral);
  /* Owned vectors stay masked across the state check and abort request. */
  if ((state == HAL_I2C_STATE_TX || state == HAL_I2C_STATE_RX) &&
      peripheral.mode == HAL_I2C_MODE_MASTER) {
    (void)HAL_I2C_MASTER_Abort_IT(&peripheral, {{device_address_7bit}}U << 1U);
  }
  /* I2C owns DMA cancellation until its STOP/error state machine relinquishes
   * the transfer. Issuing a second DMA abort here can race STOP against SUSP. */
  if (HAL_I2C_GetState(&peripheral) != HAL_I2C_STATE_IDLE ||
      peripheral.mode != HAL_I2C_MODE_NONE || peripheral.xfer_isr != NULL) { return; }
  /* The HAL error path can leave a DMA channel running after software IDLE.
   * Stop only the two channels allocated to this adapter, without waiting.
   * Peripheral STOP/abort handling and DMA suspend IRQs still must progress. */
  operation_irq_settle();
  hal_dma_handle_t *channels[2] = {&dma_tx, &dma_rx};
  for (unsigned i = 0; i < 2U; ++i) {
    hal_dma_state_t ds = HAL_DMA_GetState(channels[i]);
    if (!op_dma_stopped(channels[i]) &&
        (ds == HAL_DMA_STATE_ACTIVE || ds == HAL_DMA_STATE_IDLE)) {
      /* I2C relinquished this orphan. DMA's abort IRQ unconditionally calls
       * this pointer; normal I2C starts may have left it NULL. Do not replace
       * an abort still owned by I2C (ABORT state is excluded above). */
      channels[i]->p_xfer_abort_cb = op_orphan_aborted;
      (void)HAL_DMA_Abort_IT(channels[i]);
    }
  }
}
hal_status_t {{unit}}_init(void)
{
  if (!op_context()) { return HAL_BUSY; }
  if (operation_initialized) { return status; }
  operation_initialized = 1U;
  status = operation_hardware_init();
  if (status != HAL_OK) { return status; }
  if (HAL_I2C_MASTER_RegisterTxCpltCallback(&peripheral, op_complete) != HAL_OK ||
      HAL_I2C_MASTER_RegisterRxCpltCallback(&peripheral, op_complete) != HAL_OK ||
      HAL_I2C_RegisterErrorCallback(&peripheral, op_error) != HAL_OK ||
      HAL_I2C_RegisterAbortCpltCallback(&peripheral, op_aborted) != HAL_OK) {
    status = HAL_ERROR;
  }
  return status;
}
static int op_start(void *buffer, uint32_t bytes, uint32_t now,
                    uint32_t timeout, uint32_t recovery, unsigned receive)
{
  if (!operation_initialized || status != HAL_OK || !op_context() || buffer == NULL ||
      bytes == 0U || bytes > {{length_bytes}}U || bytes > 255U ||
      !bkpt_operation_can_begin(&operation, timeout, recovery)) { return 1; }
  op_irq_state saved = op_lock();
  I2C_TypeDef *i2c = (I2C_TypeDef *)(uintptr_t)peripheral.instance;
  if (op_stopped() != BKPT_STOPPED || LL_I2C_IsActiveFlag_BUSY(i2c) != 0U) {
    op_unlock(saved); return 1;
  }
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_tx), LL_DMA_FLAG_ALL);
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_rx), LL_DMA_FLAG_ALL);
  HAL_CORTEX_NVIC_ClearPendingIRQ({{tx_dma_irq}});
  HAL_CORTEX_NVIC_ClearPendingIRQ({{rx_dma_irq}});
  operation_complete = operation_error = 0U;
  (void)bkpt_operation_begin(&operation, now, timeout, recovery);
  /* Ownership/event state exists before hardware can complete. A HAL start
   * failure becomes a polled failure, not an unproved release of the buffer. */
  hal_status_t started = receive ?
    HAL_I2C_MASTER_Receive_DMA(&peripheral, {{device_address_7bit}}U << 1U, buffer, bytes) :
    HAL_I2C_MASTER_Transmit_DMA(&peripheral, {{device_address_7bit}}U << 1U, buffer, bytes);
  if (started != HAL_OK) { bkpt_operation_stop(&operation, now, BKPT_OP_START_ERROR); }
  op_unlock(saved);
  return 0;
}
int {{unit}}_start_rx(void *buffer, uint32_t bytes, uint32_t now, uint32_t timeout, uint32_t recovery)
{ return op_start(buffer, bytes, now, timeout, recovery, 1U); }
int {{unit}}_start_tx(const void *buffer, uint32_t bytes, uint32_t now, uint32_t timeout, uint32_t recovery)
{ return op_start((void *)buffer, bytes, now, timeout, recovery, 0U); }
bkpt_op_result {{unit}}_poll(uint32_t now)
{
  if (!operation_initialized || status != HAL_OK || !op_context()) {
    return bkpt_operation_result(&operation);
  }
  op_irq_state saved = op_lock();
  operation_irq_settle();
  bkpt_op_result result = bkpt_operation_observe(&operation, now,
    operation_complete, operation_error, op_stopped());
  if (result.state == BKPT_OP_STOPPING) {
    op_stop_step();
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
