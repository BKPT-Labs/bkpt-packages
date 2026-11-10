// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* SDK-specific fixed-length UART TX/RX adapter. The private peripheral and both DMA
 * channels are exclusively owned. TX and RX are serialized, not independent
 * duplex operations. Failures are terminal; wire-idle is not a buffer-release
 * predicate. The callback for successful TX does establish UART TC.
 * arm_rx waits without an operation deadline for the requested byte count;
 * it is not circular reception, idle-line framing or a first-byte deadline.
 * Error/cancel recovery remains bounded and keeps the same ownership rules.
 * Cancellation or a receive error permanently retires this instance; arm_rx
 * is not a cancel/rearm listener. Idle-line framing errors can also retire it. */
#if USE_HAL_UART_REGISTER_CALLBACKS != 1 || USE_HAL_UART_DMA != 1 || USE_HAL_UART_GET_LAST_ERRORS != 1
#error "Managed UART requires per-handle callbacks, DMA and last-error support"
#endif
_Static_assert({{length_bytes}}U > 0U && {{length_bytes}}U <= 65535U, "finite UART length");
#define OP_UART_CR1_IRQS (LL_USART_CR1_RXNEIE_RXFNEIE | LL_USART_CR1_PEIE | LL_USART_CR1_TXEIE_TXFNFIE | LL_USART_CR1_TCIE | LL_USART_CR1_RXFFIE | LL_USART_CR1_TXFEIE | LL_USART_CR1_IDLEIE | LL_USART_CR1_RTOIE | LL_USART_CR1_CMIE)
#define OP_UART_CR3_IRQS (LL_USART_CR3_EIE | LL_USART_CR3_RXFTIE | LL_USART_CR3_TXFTIE | LL_USART_CR3_CTSIE)
static bkpt_operation operation;
static volatile unsigned operation_complete, operation_error;
static uint32_t operation_bytes;
static unsigned operation_initialized, operation_stop_started, operation_receive;
static void op_tx_complete(hal_uart_handle_t *h) { (void)h; operation_complete = 1U; }
static void op_rx_complete(hal_uart_handle_t *h, uint32_t bytes, hal_uart_rx_event_types_t event)
{
  (void)h;
  if (event == HAL_UART_RX_EVENT_TC && bytes == operation_bytes) { operation_complete = 1U; }
  else { operation_error = 1U; }
}
static void op_error(hal_uart_handle_t *h) { (void)h; operation_error = 1U; }
static void op_aborted(hal_uart_handle_t *h) { (void)h; }
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
  return (operation_stop_started ||
          (HAL_UART_GetTxState(uart) == HAL_UART_TX_STATE_IDLE &&
           HAL_UART_GetRxState(uart) == HAL_UART_RX_STATE_IDLE)) &&
         LL_USART_IsEnabledDMAReq_TX({{peripheral_symbol}}) == 0U &&
         LL_USART_IsEnabledDMAReq_RX({{peripheral_symbol}}) == 0U &&
         ({{peripheral_symbol}}->CR1 & OP_UART_CR1_IRQS) == 0U &&
         ({{peripheral_symbol}}->CR2 & LL_USART_CR2_LBDIE) == 0U &&
         ({{peripheral_symbol}}->CR3 & OP_UART_CR3_IRQS) == 0U &&
         op_dma_stopped(&dma_tx) && op_dma_stopped(&dma_rx) ? BKPT_STOPPED : BKPT_STOP_PENDING;
}
static void op_stop_step(void)
{
  hal_dma_handle_t *channels[2] = {&dma_tx, &dma_rx};
  if (!operation_stop_started) {
    operation_stop_started = 1U;
    /* Terminal stop takes exclusive ownership from the UART HAL. Its combined
     * abort can strand one direction in ABORT when DMA TC wins over SUSP.
     * Disable the same interrupt/request sources first, then own DMA callbacks.
     * No synthetic HAL-IDLE state, callback or rearm is promised after failure. */
    LL_USART_DisableIT_CR1({{peripheral_symbol}}, OP_UART_CR1_IRQS);
    LL_USART_DisableIT_CR2({{peripheral_symbol}}, LL_USART_CR2_LBDIE);
    LL_USART_DisableIT_CR3({{peripheral_symbol}}, OP_UART_CR3_IRQS);
    LL_USART_DisableDMAReq_TX({{peripheral_symbol}});
    LL_USART_DisableDMAReq_RX({{peripheral_symbol}});
    for (unsigned i = 0; i < 2U; ++i) {
      channels[i]->p_xfer_cplt_cb = op_dma_event;
      channels[i]->p_xfer_halfcplt_cb = op_dma_event;
      channels[i]->p_xfer_error_cb = op_dma_event;
      channels[i]->p_xfer_abort_cb = op_dma_event;
      channels[i]->p_xfer_suspend_cb = op_dma_event;
    }
  }
  /* Inspect both physical channels even if software/gates already say idle.
   * A channel already in ABORT must finish its existing suspend IRQ. */
  for (unsigned i = 0; i < 2U; ++i) {
    hal_dma_state_t state = HAL_DMA_GetState(channels[i]);
    if (!op_dma_stopped(channels[i]) &&
        (state == HAL_DMA_STATE_IDLE || state == HAL_DMA_STATE_ACTIVE)) {
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
  if (LL_USART_IsEnabledFIFO({{peripheral_symbol}}) != 0U) { status = HAL_ERROR; return status; }
  if (HAL_UART_RegisterTxCpltCallback(uart, op_tx_complete) != HAL_OK ||
      HAL_UART_RegisterRxCpltCallback(uart, op_rx_complete) != HAL_OK ||
      HAL_UART_RegisterErrorCallback(uart, op_error) != HAL_OK ||
      HAL_UART_RegisterAbortCpltCallback(uart, op_aborted) != HAL_OK) { status = HAL_ERROR; }
  /* Enable without the HAL's acknowledgement wait. Start refuses until both
   * direction acknowledgements are visible, without taking buffer ownership. */
  if (status == HAL_OK) { LL_USART_Enable({{peripheral_symbol}}); }
  return status;
}
static int op_start(void *buffer, uint32_t bytes, uint32_t now,
                    bkpt_op_deadline deadline, uint32_t recovery, unsigned receive)
{
  if (!operation_initialized || status != HAL_OK || !op_context() || buffer == NULL ||
      bytes == 0U || bytes > {{length_bytes}}U ||
      !bkpt_operation_can_begin_with_deadline(&operation, deadline, recovery)) { return 1; }
  op_irq_state saved = op_lock();
  if (op_stopped() != BKPT_STOPPED || HAL_UART_GetTxState(uart) != HAL_UART_TX_STATE_IDLE ||
      HAL_UART_GetRxState(uart) != HAL_UART_RX_STATE_IDLE ||
      LL_USART_IsEnabled({{peripheral_symbol}}) == 0U ||
      LL_USART_IsActiveFlag_TEACK({{peripheral_symbol}}) == 0U ||
      LL_USART_IsActiveFlag_REACK({{peripheral_symbol}}) == 0U) { op_unlock(saved); return 1; }
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_tx), LL_DMA_FLAG_ALL);
  LL_DMA_ClearFlag(HAL_DMA_GetLLInstance(&dma_rx), LL_DMA_FLAG_ALL);
  HAL_CORTEX_NVIC_ClearPendingIRQ({{tx_dma_irq}});
  HAL_CORTEX_NVIC_ClearPendingIRQ({{rx_dma_irq}});
  operation_complete = operation_error = operation_stop_started = 0U;
  operation_bytes = bytes;
  operation_receive = receive;
  (void)bkpt_operation_begin_with_deadline(&operation, now, deadline, recovery);
  hal_status_t started = receive ? HAL_UART_Receive_DMA(uart, buffer, bytes) :
                                  HAL_UART_Transmit_DMA(uart, buffer, bytes);
  if (started != HAL_OK) { bkpt_operation_stop(&operation, now, BKPT_OP_START_ERROR); }
  op_unlock(saved);
  return 0;
}
int {{unit}}_start_tx(const void *buffer, uint32_t bytes, uint32_t now, uint32_t timeout, uint32_t recovery)
{
  bkpt_op_deadline deadline = {BKPT_DEADLINE_FINITE, timeout};
  return op_start((void *)buffer, bytes, now, deadline, recovery, 0U);
}
int {{unit}}_start_rx(void *buffer, uint32_t bytes, uint32_t now, uint32_t timeout, uint32_t recovery)
{
  bkpt_op_deadline deadline = {BKPT_DEADLINE_FINITE, timeout};
  return op_start(buffer, bytes, now, deadline, recovery, 1U);
}
int {{unit}}_arm_rx(void *buffer, uint32_t bytes, uint32_t now, uint32_t recovery)
{
  bkpt_op_deadline deadline = {BKPT_DEADLINE_NONE, 0U};
  return op_start(buffer, bytes, now, deadline, recovery, 1U);
}
bkpt_op_result {{unit}}_poll(uint32_t now)
{
  if (!operation_initialized || status != HAL_OK || !op_context()) { return bkpt_operation_result(&operation); }
  op_irq_state saved = op_lock();
  /* DMA completion may beat the UART error IRQ or its deferred abort callback.
   * Preserve error evidence before normal completion can release successful data. */
  if (HAL_UART_GetLastErrorCodes(uart) != 0U ||
      (operation_receive && (LL_USART_IsActiveFlag_PE({{peripheral_symbol}}) ||
       LL_USART_IsActiveFlag_FE({{peripheral_symbol}}) || LL_USART_IsActiveFlag_NE({{peripheral_symbol}}) ||
       LL_USART_IsActiveFlag_ORE({{peripheral_symbol}})))) { operation_error = 1U; }
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
