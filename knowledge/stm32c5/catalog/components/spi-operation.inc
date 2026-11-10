// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* SDK-specific full-duplex, master, 8-bit, finite DMA operation. The stop path
 * uses staged LL suspension because this SDK's HAL_SPI_Abort_IT busy-waits.
 * No raw handle is exposed. Failed data are invalid; failed instances never
 * restart. External chip select remains caller-owned until buffers_released. */
#if USE_HAL_SPI_REGISTER_CALLBACKS != 1 || USE_HAL_SPI_DMA != 1 || USE_HAL_SPI_GET_LAST_ERRORS != 1
#error "Managed SPI requires per-handle callbacks, DMA and last-error support"
#endif
_Static_assert({{length_bytes}}U > 0U && {{length_bytes}}U <= 65535U, "finite SPI length");
#define OP_SPI_IRQS (LL_SPI_IT_EOT | LL_SPI_IT_DXP | LL_SPI_IT_TXP | LL_SPI_IT_RXP | LL_SPI_IT_OVR | LL_SPI_IT_UDR | LL_SPI_IT_TIFRE | LL_SPI_IT_MODF)
static bkpt_operation operation;
static volatile unsigned operation_complete, operation_error;
static unsigned operation_initialized, operation_stop_started;
static void op_complete(hal_spi_handle_t *h) { (void)h; operation_complete = 1U; }
static void op_error(hal_spi_handle_t *h) { (void)h; operation_error = 1U; }
static void op_aborted(hal_spi_handle_t *h) { (void)h; }
static void op_dma_event(hal_dma_handle_t *h) { (void)h; }
static int op_context(void)
{
  return __get_IPSR() == 0U && __get_PRIMASK() == 0U &&
         __get_BASEPRI() == 0U && __get_FAULTMASK() == 0U;
}
typedef struct { uint32_t tx, rx, peripheral; } op_irq_state;
static op_irq_state op_lock(void)
{
  op_irq_state saved = {NVIC_GetEnableIRQ({{tx_dma_irq}}), NVIC_GetEnableIRQ({{rx_dma_irq}}),
                        NVIC_GetEnableIRQ({{peripheral_irq}})};
  HAL_CORTEX_NVIC_DisableIRQ({{tx_dma_irq}});
  HAL_CORTEX_NVIC_DisableIRQ({{rx_dma_irq}});
  HAL_CORTEX_NVIC_DisableIRQ({{peripheral_irq}});
  __DSB(); __ISB();
  return saved;
}
static void op_unlock(op_irq_state saved)
{
  if (saved.tx) { HAL_CORTEX_NVIC_EnableIRQ({{tx_dma_irq}}); }
  if (saved.rx) { HAL_CORTEX_NVIC_EnableIRQ({{rx_dma_irq}}); }
  if (saved.peripheral) { HAL_CORTEX_NVIC_EnableIRQ({{peripheral_irq}}); }
}
static int op_dma_stopped(const hal_dma_handle_t *d)
{
  return HAL_DMA_GetState(d) == HAL_DMA_STATE_IDLE &&
         LL_DMA_IsActiveFlag_IDLE(HAL_DMA_GetLLInstance(d)) != 0U &&
         LL_DMA_IsEnabledChannel(HAL_DMA_GetLLInstance(d)) == 0U;
}
static bkpt_stop_state op_stopped(void)
{
  SPI_TypeDef *spi = (SPI_TypeDef *)(uintptr_t)peripheral.instance;
  return HAL_SPI_GetState(&peripheral) != HAL_SPI_STATE_ABORT &&
         LL_SPI_IsActiveMasterTransfer(spi) == 0U && LL_SPI_IsEnabled(spi) == 0U &&
         LL_SPI_IsEnabledDMAReq_TX(spi) == 0U && LL_SPI_IsEnabledDMAReq_RX(spi) == 0U &&
         (spi->IER & OP_SPI_IRQS) == 0U && op_dma_stopped(&dma_tx) && op_dma_stopped(&dma_rx)
         ? BKPT_STOPPED : BKPT_STOP_PENDING;
}
static int op_rx_pending(SPI_TypeDef *spi)
{
  return LL_SPI_IsActiveFlag_RXP(spi) != 0U || LL_SPI_IsActiveFlag_RXWNE(spi) != 0U ||
         LL_SPI_GetRxFIFOPackingLevel(spi) != 0U;
}
static void op_stop_step(void)
{
  SPI_TypeDef *spi = (SPI_TypeDef *)(uintptr_t)peripheral.instance;
  hal_dma_handle_t *channels[2] = {&dma_tx, &dma_rx};
  if (!operation_stop_started) {
    operation_stop_started = 1U;
    /* All three vectors are masked. Take over DMA callback ownership so a late
     * HAL SPI callback cannot close/reconfigure the peripheral during suspension.
     * The DMA IRQ handler continues settling physical channel state. */
    LL_SPI_DisableIT(spi, OP_SPI_IRQS);
    for (unsigned i = 0; i < 2U; ++i) {
      channels[i]->p_xfer_cplt_cb = op_dma_event;
      channels[i]->p_xfer_halfcplt_cb = op_dma_event;
      channels[i]->p_xfer_error_cb = op_dma_event;
      channels[i]->p_xfer_abort_cb = op_dma_event;
      channels[i]->p_xfer_suspend_cb = op_dma_event;
    }
  }
  if (LL_SPI_IsEnabled(spi) != 0U && LL_SPI_IsActiveMasterTransfer(spi) != 0U) {
    LL_SPI_SuspendMasterTransfer(spi);
    return; /* Observe frame completion on a subsequent poll; never wait. */
  }
  if (LL_SPI_IsEnabled(spi) != 0U && LL_SPI_IsActiveFlag_SUSP(spi) == 0U &&
      LL_SPI_IsActiveFlag_EOT(spi) == 0U && LL_SPI_IsActiveFlag_TXC(spi) == 0U) { return; }
  LL_SPI_DisableDMAReq_TX(spi);
  LL_SPI_DisableDMAReq_RX(spi);
  for (unsigned i = 0; i < 2U; ++i) {
    hal_dma_state_t state = HAL_DMA_GetState(channels[i]);
    if (!op_dma_stopped(channels[i]) &&
        (state == HAL_DMA_STATE_IDLE || state == HAL_DMA_STATE_ACTIVE)) {
      (void)HAL_DMA_Abort_IT(channels[i]);
    }
  }
  if (!op_dma_stopped(&dma_tx) || !op_dma_stopped(&dma_rx)) { return; }
  /* No DMA can read RXDR now. Drain a fixed byte budget per poll; cancelled
   * partial data are discarded and never reported as a completed exchange. */
  for (unsigned i = 0; i < 4U && op_rx_pending(spi); ++i) { (void)LL_SPI_ReceiveData8(spi); }
  if (op_rx_pending(spi)) { return; }
  /* RM requires SUSP cleared before SPE. Keep the independent stopped check:
   * if CSTART does not settle after an earlier vendor error, fail closed. */
  LL_SPI_ClearFlag(spi, LL_SPI_FLAG_EOT | LL_SPI_FLAG_TXTF | LL_SPI_FLAG_OVR |
                       LL_SPI_FLAG_UDR | LL_SPI_FLAG_TIFRE | LL_SPI_FLAG_MODF | LL_SPI_FLAG_SUSP);
  LL_SPI_Disable(spi);
  __DSB();
  if (LL_SPI_IsActiveMasterTransfer(spi) != 0U || LL_SPI_IsEnabled(spi) != 0U) { return; }
  /* Do not manufacture HAL IDLE: physical buffer release is distinct from
   * peripheral recovery. Failure is terminal and no raw accessor is exposed. */
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_tx), LL_DMA_FLAG_ALL);
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_rx), LL_DMA_FLAG_ALL);
  HAL_CORTEX_NVIC_ClearPendingIRQ({{tx_dma_irq}});
  HAL_CORTEX_NVIC_ClearPendingIRQ({{rx_dma_irq}});
  HAL_CORTEX_NVIC_ClearPendingIRQ({{peripheral_irq}});
}
hal_status_t {{unit}}_init(void)
{
  if (!op_context()) { return HAL_BUSY; }
  if (operation_initialized) { return status; }
  operation_initialized = 1U;
  status = operation_hardware_init();
  if (status != HAL_OK) { return status; }
  if (HAL_SPI_RegisterTxRxCpltCallback(&peripheral, op_complete) != HAL_OK ||
      HAL_SPI_RegisterErrorCallback(&peripheral, op_error) != HAL_OK ||
      HAL_SPI_RegisterAbortCpltCallback(&peripheral, op_aborted) != HAL_OK) { status = HAL_ERROR; }
  return status;
}
int {{unit}}_start(const void *tx, void *rx, uint32_t bytes, uint32_t now,
                   uint32_t timeout, uint32_t recovery)
{
  if (!operation_initialized || status != HAL_OK || !op_context() || tx == NULL || rx == NULL ||
      bytes == 0U || bytes > {{length_bytes}}U ||
      !bkpt_operation_can_begin(&operation, timeout, recovery)) { return 1; }
  uintptr_t t = (uintptr_t)tx, r = (uintptr_t)rx;
  if (t > UINTPTR_MAX - bytes || r > UINTPTR_MAX - bytes || (t < r + bytes && r < t + bytes)) { return 1; }
  op_irq_state saved = op_lock();
  if (op_stopped() != BKPT_STOPPED || HAL_SPI_GetState(&peripheral) != HAL_SPI_STATE_IDLE) {
    op_unlock(saved); return 1;
  }
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_tx), LL_DMA_FLAG_ALL);
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_rx), LL_DMA_FLAG_ALL);
  HAL_CORTEX_NVIC_ClearPendingIRQ({{tx_dma_irq}});
  HAL_CORTEX_NVIC_ClearPendingIRQ({{rx_dma_irq}});
  operation_complete = operation_error = operation_stop_started = 0U;
  (void)bkpt_operation_begin(&operation, now, timeout, recovery);
  if (HAL_SPI_TransmitReceive_DMA(&peripheral, tx, rx, bytes) != HAL_OK) {
    bkpt_operation_stop(&operation, now, BKPT_OP_START_ERROR);
  }
  op_unlock(saved);
  return 0;
}
bkpt_op_result {{unit}}_poll(uint32_t now)
{
  if (!operation_initialized || status != HAL_OK || !op_context()) { return bkpt_operation_result(&operation); }
  op_irq_state saved = op_lock();
  /* HAL may record an error before its DMA-abort callback is delivered. */
  if (HAL_SPI_GetLastErrorCodes(&peripheral) != 0U) { operation_error = 1U; }
  bkpt_op_result result = bkpt_operation_observe(&operation, now, operation_complete, operation_error, op_stopped());
  if (result.state == BKPT_OP_STOPPING) {
    op_stop_step();
    result = bkpt_operation_observe(&operation, now, operation_complete, operation_error, op_stopped());
  }
  op_unlock(saved);
  return result;
}
bkpt_op_result {{unit}}_cancel(uint32_t now)
{
  if (!operation_initialized || status != HAL_OK || !op_context()) { return bkpt_operation_result(&operation); }
  (void){{unit}}_poll(now);
  bkpt_operation_stop(&operation, now, BKPT_OP_CANCELLED);
  return {{unit}}_poll(now);
}
