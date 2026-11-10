# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Compile the actual SPI/UART operation adapters against adversarial HAL events.

These are ownership/order tests, not a peripheral or hardware simulation.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMMON = r'''
// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef enum {HAL_OK,HAL_ERROR,HAL_BUSY} hal_status_t;
typedef enum {HAL_DMA_STATE_IDLE,HAL_DMA_STATE_ACTIVE,HAL_DMA_STATE_ABORT} hal_dma_state_t;
typedef struct dma {
 hal_dma_state_t state; unsigned idle,en;
 void(*p_xfer_cplt_cb)(struct dma*);void(*p_xfer_halfcplt_cb)(struct dma*);
 void(*p_xfer_error_cb)(struct dma*);void(*p_xfer_abort_cb)(struct dma*);void(*p_xfer_suspend_cb)(struct dma*);
} hal_dma_handle_t;
static hal_dma_handle_t dma_tx,dma_rx;
static unsigned context[4],irqs[3],pending[3],abort_calls,starts;
static hal_status_t status=HAL_ERROR,init_result=HAL_OK,start_result=HAL_OK;
#define __get_IPSR() context[0]
#define __get_PRIMASK() context[1]
#define __get_BASEPRI() context[2]
#define __get_FAULTMASK() context[3]
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#define NVIC_GetEnableIRQ(n) irqs[n]
#define HAL_CORTEX_NVIC_DisableIRQ(n) (irqs[n]=0)
#define HAL_CORTEX_NVIC_EnableIRQ(n) (irqs[n]=1)
#define HAL_CORTEX_NVIC_ClearPendingIRQ(n) (pending[n]=0)
#define HAL_DMA_GetState(d) ((d)->state)
#define HAL_DMA_GetLLInstance(d) (d)
#define LL_DMA_IsActiveFlag_IDLE(d) ((d)->idle)
#define LL_DMA_IsEnabledChannel(d) ((d)->en)
#define LL_DMA_ClearFlag(d,f) ((void)(d),(void)(f))
#define LL_DMA_FLAG_ALL 255U
static hal_status_t operation_hardware_init(void){return init_result;}
static hal_status_t HAL_DMA_Abort_IT(hal_dma_handle_t*d){
 assert(!irqs[0]&&!irqs[1]&&!irqs[2]);
 assert(d->state==HAL_DMA_STATE_IDLE || d->state==HAL_DMA_STATE_ACTIVE);
 ++abort_calls;if(d->idle)return HAL_ERROR;d->state=HAL_DMA_STATE_ABORT;return HAL_OK;
}
static void active(hal_dma_handle_t*d){d->state=HAL_DMA_STATE_ACTIVE;d->idle=0;d->en=1;}
static void settle(hal_dma_handle_t*d){d->state=HAL_DMA_STATE_IDLE;d->idle=1;d->en=0;if(d->p_xfer_abort_cb)d->p_xfer_abort_cb(d);}
'''
SPI = r'''
#define USE_HAL_SPI_REGISTER_CALLBACKS 1
#define USE_HAL_SPI_DMA 1
#define USE_HAL_SPI_GET_LAST_ERRORS 1
#define LL_SPI_IT_EOT 1U
#define LL_SPI_IT_DXP 2U
#define LL_SPI_IT_TXP 4U
#define LL_SPI_IT_RXP 8U
#define LL_SPI_IT_OVR 16U
#define LL_SPI_IT_UDR 32U
#define LL_SPI_IT_TIFRE 64U
#define LL_SPI_IT_MODF 128U
#define LL_SPI_FLAG_EOT 1U
#define LL_SPI_FLAG_TXTF 2U
#define LL_SPI_FLAG_OVR 4U
#define LL_SPI_FLAG_UDR 8U
#define LL_SPI_FLAG_TIFRE 16U
#define LL_SPI_FLAG_MODF 32U
#define LL_SPI_FLAG_SUSP 64U
typedef enum {HAL_SPI_STATE_IDLE,HAL_SPI_STATE_TX_RX_ACTIVE,HAL_SPI_STATE_ABORT}hal_spi_state_t;
typedef struct{unsigned IER,cstart,en,tx,rx,susp,eot,txc,fifo,reads,suspend_requests;}SPI_TypeDef;
typedef struct spi {uintptr_t instance;hal_spi_state_t global_state;unsigned tx_xfer_count,rx_xfer_count,errors;
 void(*done)(struct spi*);void(*error)(struct spi*);void(*abort)(struct spi*);}hal_spi_handle_t;
static SPI_TypeDef hw;static hal_spi_handle_t peripheral;
#define HAL_SPI_GetState(p) ((p)->global_state)
#define HAL_SPI_GetLastErrorCodes(p) ((p)->errors)
#define LL_SPI_IsActiveMasterTransfer(p) ((p)->cstart)
#define LL_SPI_IsEnabled(p) ((p)->en)
#define LL_SPI_IsEnabledDMAReq_TX(p) ((p)->tx)
#define LL_SPI_IsEnabledDMAReq_RX(p) ((p)->rx)
#define LL_SPI_DisableDMAReq_TX(p) ((p)->tx=0)
#define LL_SPI_DisableDMAReq_RX(p) ((p)->rx=0)
#define LL_SPI_IsActiveFlag_SUSP(p) ((p)->susp)
#define LL_SPI_IsActiveFlag_EOT(p) ((p)->eot)
#define LL_SPI_IsActiveFlag_TXC(p) ((p)->txc)
#define LL_SPI_IsActiveFlag_RXP(p) ((p)->fifo!=0)
#define LL_SPI_IsActiveFlag_RXWNE(p) ((p)->fifo>=4)
#define LL_SPI_GetRxFIFOPackingLevel(p) ((p)->fifo%4)
#define LL_SPI_DisableIT(p,m) ((p)->IER&=~(m))
#define LL_SPI_SuspendMasterTransfer(p) (++(p)->suspend_requests)
#define LL_SPI_Disable(p) (assert(!(p)->susp && !(p)->fifo),(p)->en=0)
#define LL_SPI_ClearFlag(p,m) ((void)(m),(p)->susp=0,(p)->eot=0)
static uint8_t LL_SPI_ReceiveData8(SPI_TypeDef*p){assert(!dma_rx.en&&!dma_tx.en);assert(p->fifo);--p->fifo;++p->reads;return 0;}
#define HAL_SPI_RegisterTxRxCpltCallback(p,c) ((p)->done=(c),HAL_OK)
#define HAL_SPI_RegisterErrorCallback(p,c) ((p)->error=(c),HAL_OK)
#define HAL_SPI_RegisterAbortCpltCallback(p,c) ((p)->abort=(c),HAL_OK)
static hal_status_t HAL_SPI_TransmitReceive_DMA(hal_spi_handle_t*p,const void*tx,void*rx,uint32_t n){
 (void)tx;(void)rx;(void)n;assert(!irqs[0]&&!irqs[1]&&!irqs[2]);++starts;
 active(&dma_rx);if(start_result!=HAL_OK)return start_result;
 active(&dma_tx);p->global_state=HAL_SPI_STATE_TX_RX_ACTIVE;hw.en=hw.cstart=hw.tx=hw.rx=1;hw.IER=255;return HAL_OK;
}
'''
RESET = r'''
static void reset(void){
 memset(&operation,0,sizeof(operation));operation_complete=operation_error=operation_initialized=operation_stop_started=0;
 memset(&hw,0,sizeof(hw));dma_tx=dma_rx=(hal_dma_handle_t){.idle=1};
 memset(context,0,sizeof(context));for(unsigned i=0;i<3;i++){irqs[i]=1;pending[i]=0;}
 status=HAL_ERROR;init_result=start_result=HAL_OK;abort_calls=starts=0;
 RESET_PERIPHERAL
}
'''
SPI_TEST = r'''
static uint8_t txbuf[32],rxbuf[32];
static int start(uint32_t now){return test_start(txbuf,rxbuf,24,now,5,10);}
static void complete(void){settle(&dma_tx);settle(&dma_rx);hw.en=hw.cstart=hw.tx=hw.rx=hw.IER=0;peripheral.global_state=HAL_SPI_STATE_IDLE;peripheral.done(&peripheral);}
int main(void){
 reset();init_result=HAL_ERROR;assert(test_init()==HAL_ERROR);assert(start(0)!=0);
 reset();context[0]=1;assert(test_init()==HAL_BUSY);assert(!operation_initialized);context[0]=0;assert(test_init()==HAL_OK);
 assert(test_start(txbuf,txbuf,24,0,5,10)!=0);assert(test_start(txbuf,rxbuf,0,0,5,10)!=0);
 assert(test_start(txbuf,rxbuf,33,0,5,10)!=0);assert(starts==0);
 assert(start(0)==0);assert(start(1)!=0);peripheral.done(&peripheral);
 assert(test_poll(1).state==BKPT_OP_ACTIVE && !test_poll(1).buffers_released);
 complete();assert(test_poll(2).state==BKPT_OP_DONE);assert(start(3)==0);
 assert(test_cancel(4).state==BKPT_OP_STOPPING);assert(hw.suspend_requests && !hw.reads);
 assert(abort_calls==0);hw.cstart=0;hw.susp=1;hw.fifo=9;
 assert(test_poll(5).state==BKPT_OP_STOPPING);assert(abort_calls==2 && !hw.reads);
 settle(&dma_tx);settle(&dma_rx);assert(test_poll(6).state==BKPT_OP_STOPPING && hw.reads==4);
 assert(test_poll(7).state==BKPT_OP_STOPPING && hw.reads==8);
 assert(test_poll(8).state==BKPT_OP_FAILED && hw.reads==9 && test_poll(8).buffers_released);assert(start(9)!=0);
 reset();assert(test_init()==HAL_OK);start_result=HAL_ERROR;assert(start(0)==0);
 assert(test_poll(0).state==BKPT_OP_STOPPING && abort_calls==1);settle(&dma_rx);
 assert(test_poll(1).state==BKPT_OP_FAILED && test_poll(1).reason==BKPT_OP_START_ERROR);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);irqs[1]=0;
 assert(test_cancel(1).state==BKPT_OP_STOPPING && !irqs[1]);
 assert(test_poll(11).state==BKPT_OP_UNSAFE);complete();assert(!test_poll(12).buffers_released);assert(start(12)!=0);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);complete();peripheral.error(&peripheral);
 assert(test_poll(1).state==BKPT_OP_FAILED && test_poll(1).reason==BKPT_OP_IO_ERROR);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);complete();peripheral.errors=1;
 assert(test_poll(1).state==BKPT_OP_FAILED && test_poll(1).reason==BKPT_OP_IO_ERROR);
 reset();assert(test_init()==HAL_OK);assert(start(UINT32_MAX-2)==0);complete();
 assert(test_poll(2).state==BKPT_OP_FAILED && test_poll(2).reason==BKPT_OP_TIMEOUT);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);context[2]=1;
 assert(test_cancel(1).state==BKPT_OP_ACTIVE && !hw.suspend_requests);context[2]=0;
 assert(test_cancel(1).state==BKPT_OP_STOPPING);
 for(unsigned idle=0;idle<2;idle++){
  reset();assert(test_init()==HAL_OK);assert(start(0)==0);
  hal_dma_handle_t*finished=idle?&dma_rx:&dma_tx;
  hal_dma_handle_t*pending_dma=idle?&dma_tx:&dma_rx;
  settle(finished);pending_dma->p_xfer_abort_cb=NULL;
  assert(test_cancel(1).state==BKPT_OP_STOPPING);
  assert(pending_dma->p_xfer_abort_cb!=NULL);hw.cstart=0;hw.susp=1;
  assert(test_poll(2).state==BKPT_OP_STOPPING && abort_calls==1);
  pending_dma->state=HAL_DMA_STATE_IDLE;pending_dma->idle=1;pending_dma->en=0;
  pending_dma->p_xfer_cplt_cb(pending_dma); /* TC precedes SUSP */
  assert(test_poll(3).state==BKPT_OP_FAILED && test_poll(3).buffers_released);
  assert(peripheral.global_state==HAL_SPI_STATE_TX_RX_ACTIVE);assert(start(4)!=0);
 }
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);hw.en=0;
 assert(test_cancel(1).state==BKPT_OP_STOPPING && !hw.suspend_requests && abort_calls==2);
 settle(&dma_tx);settle(&dma_rx);
 assert(test_poll(11).state==BKPT_OP_UNSAFE && !test_poll(11).buffers_released); /* unproven CSTART settling */
 return 0;
}
'''
UART = r'''
#define USE_HAL_UART_REGISTER_CALLBACKS 1
#define USE_HAL_UART_DMA 1
#define USE_HAL_UART_GET_LAST_ERRORS 1
typedef enum{HAL_UART_TX_STATE_IDLE,HAL_UART_TX_STATE_ACTIVE,HAL_UART_TX_STATE_ABORT}hal_uart_tx_state_t;
typedef enum{HAL_UART_RX_STATE_IDLE,HAL_UART_RX_STATE_ACTIVE,HAL_UART_RX_STATE_ABORT}hal_uart_rx_state_t;
typedef enum{HAL_UART_RX_EVENT_TC,HAL_UART_RX_EVENT_IDLE}hal_uart_rx_event_types_t;
#define LL_USART_CR1_RXNEIE_RXFNEIE 1U
#define LL_USART_CR1_PEIE 2U
#define LL_USART_CR1_TXEIE_TXFNFIE 4U
#define LL_USART_CR1_TCIE 8U
#define LL_USART_CR1_RXFFIE 16U
#define LL_USART_CR1_TXFEIE 32U
#define LL_USART_CR1_IDLEIE 64U
#define LL_USART_CR1_RTOIE 128U
#define LL_USART_CR1_CMIE 256U
#define LL_USART_CR2_LBDIE 1U
#define LL_USART_CR3_EIE 1U
#define LL_USART_CR3_RXFTIE 2U
#define LL_USART_CR3_TXFTIE 4U
#define LL_USART_CR3_CTSIE 8U
typedef struct{unsigned tx,rx,CR1,CR2,CR3,en,teack,reack,fifo,errors;}USART_TypeDef;
typedef struct uart {hal_uart_tx_state_t tx;hal_uart_rx_state_t rx;unsigned errors;
 void(*txdone)(struct uart*);void(*rxdone)(struct uart*,uint32_t,hal_uart_rx_event_types_t);
 void(*error)(struct uart*);void(*abort)(struct uart*);}hal_uart_handle_t;
static USART_TypeDef hw;static hal_uart_handle_t peripheral,*uart=&peripheral;
#define HAL_UART_GetTxState(p) ((p)->tx)
#define HAL_UART_GetRxState(p) ((p)->rx)
#define HAL_UART_GetLastErrorCodes(p) ((p)->errors)
#define LL_USART_IsActiveFlag_PE(p) ((p)->errors&1U)
#define LL_USART_IsActiveFlag_FE(p) ((p)->errors&2U)
#define LL_USART_IsActiveFlag_NE(p) ((p)->errors&4U)
#define LL_USART_IsActiveFlag_ORE(p) ((p)->errors&8U)
#define LL_USART_IsEnabledDMAReq_TX(p) ((p)->tx)
#define LL_USART_IsEnabledDMAReq_RX(p) ((p)->rx)
#define LL_USART_DisableDMAReq_TX(p) ((p)->tx=0)
#define LL_USART_DisableDMAReq_RX(p) ((p)->rx=0)
#define LL_USART_DisableIT_CR1(p,m) ((p)->CR1&=~(m))
#define LL_USART_DisableIT_CR2(p,m) ((p)->CR2&=~(m))
#define LL_USART_DisableIT_CR3(p,m) ((p)->CR3&=~(m))
#define LL_USART_Enable(p) ((p)->en=1)
#define LL_USART_IsEnabled(p) ((p)->en)
#define LL_USART_IsEnabledFIFO(p) ((p)->fifo)
#define LL_USART_IsActiveFlag_TEACK(p) ((p)->teack)
#define LL_USART_IsActiveFlag_REACK(p) ((p)->reack)
#define HAL_UART_RegisterTxCpltCallback(p,c) ((p)->txdone=(c),HAL_OK)
#define HAL_UART_RegisterRxCpltCallback(p,c) ((p)->rxdone=(c),HAL_OK)
#define HAL_UART_RegisterErrorCallback(p,c) ((p)->error=(c),HAL_OK)
#define HAL_UART_RegisterAbortCpltCallback(p,c) ((p)->abort=(c),HAL_OK)
static hal_status_t HAL_UART_Transmit_DMA(hal_uart_handle_t*p,const void*b,uint32_t n){
 (void)b;(void)n;assert(!irqs[0]&&!irqs[1]&&!irqs[2]);++starts;
 if(start_result!=HAL_OK){return start_result;}active(&dma_tx);p->tx=HAL_UART_TX_STATE_ACTIVE;hw.tx=1;return HAL_OK;
}
static hal_status_t HAL_UART_Receive_DMA(hal_uart_handle_t*p,void*b,uint32_t n){
 (void)b;(void)n;assert(!irqs[0]&&!irqs[1]&&!irqs[2]);++starts;
 if(start_result!=HAL_OK){return start_result;}active(&dma_rx);p->rx=HAL_UART_RX_STATE_ACTIVE;hw.rx=1;return HAL_OK;
}
'''
UART_TEST = r'''
static uint8_t buf[32];
static int start(uint32_t now){return test_start_tx(buf,24,now,5,10);}
static void test_armed_receive(void){
 reset();assert(test_init()==HAL_OK);
 assert(test_arm_rx(NULL,12,0,10)!=0);assert(test_arm_rx(buf,0,0,10)!=0);
 assert(test_arm_rx(buf,33,0,10)!=0);assert(test_arm_rx(buf,12,0,0)!=0);
 assert(test_arm_rx(buf,12,0,UINT32_MAX)!=0);assert(!starts);
 assert(test_start_rx(buf,12,0,0,10)!=0);assert(test_start_tx(buf,12,0,0,10)!=0);
 assert(test_arm_rx(buf,12,0,10)==0);assert(operation.deadline_kind==BKPT_DEADLINE_NONE);
 assert(start(1)!=0 && test_arm_rx(buf,12,1,10)!=0 && starts==1);
 for(unsigned wrap=0;wrap<3;wrap++){
  assert(test_poll(UINT32_MAX).state==BKPT_OP_ACTIVE);
  assert(test_poll(1).state==BKPT_OP_ACTIVE && !test_poll(1).buffers_released);
 }
 uart->rxdone(uart,12,HAL_UART_RX_EVENT_TC);
 assert(test_poll(2).state==BKPT_OP_ACTIVE); /* Callback alone does not free DMA. */
 settle(&dma_rx);uart->rx=HAL_UART_RX_STATE_IDLE;hw.rx=0;
 assert(test_poll(3).state==BKPT_OP_DONE && test_poll(3).buffers_released);
 assert(test_arm_rx(buf,7,3,10)==0 && operation_bytes==7 && !operation_complete && !operation_error);
 assert(test_poll(UINT32_MAX).state==BKPT_OP_ACTIVE);
 settle(&dma_rx);uart->rx=HAL_UART_RX_STATE_IDLE;hw.rx=0;uart->rxdone(uart,7,HAL_UART_RX_EVENT_TC);
 assert(test_poll(3).state==BKPT_OP_DONE);
 assert(start(4)==0 && operation.deadline_kind==BKPT_DEADLINE_FINITE);
 assert(test_poll(9).state==BKPT_OP_STOPPING && operation.reason==BKPT_OP_TIMEOUT);
 settle(&dma_tx);assert(test_poll(10).state==BKPT_OP_FAILED);
 for(unsigned error=0;error<2;error++){
  reset();assert(test_init()==HAL_OK);assert(test_arm_rx(buf,12,0,10)==0);
  if(error){hw.errors=1;assert(test_poll(UINT32_MAX-5).reason==BKPT_OP_IO_ERROR);}
  else{assert(test_cancel(UINT32_MAX-5).reason==BKPT_OP_CANCELLED);}
  assert(operation.state==BKPT_OP_STOPPING && !test_poll(0).buffers_released);
  settle(&dma_rx);assert(test_poll(1).state==BKPT_OP_FAILED && test_poll(1).buffers_released);
  assert(test_arm_rx(buf,12,2,10)!=0 && start(2)!=0);
 }
 reset();assert(test_init()==HAL_OK);assert(test_arm_rx(buf,12,0,10)==0);
 assert(test_cancel(UINT32_MAX-5).state==BKPT_OP_STOPPING);
 assert(test_poll(4).state==BKPT_OP_UNSAFE);settle(&dma_rx);
 assert(!test_poll(5).buffers_released && test_arm_rx(buf,12,6,10)!=0);
 reset();assert(test_init()==HAL_OK);start_result=HAL_ERROR;
 assert(test_arm_rx(buf,12,0,10)==0);
 assert(test_poll(0).state==BKPT_OP_FAILED && operation.reason==BKPT_OP_START_ERROR);
 reset();assert(test_init()==HAL_OK);assert(test_arm_rx(buf,12,0,10)==0);
 settle(&dma_rx);uart->rx=HAL_UART_RX_STATE_IDLE;hw.rx=0;uart->rxdone(uart,12,HAL_UART_RX_EVENT_TC);
 hw.errors=1;
 assert(test_poll(UINT32_MAX).state==BKPT_OP_FAILED && operation.reason==BKPT_OP_IO_ERROR);
}
int main(void){
 test_armed_receive();
 reset();hw.fifo=1;assert(test_init()==HAL_ERROR);assert(start(0)!=0 && !starts);
 reset();assert(test_init()==HAL_OK);hw.teack=0;assert(start(0)!=0 && !starts);hw.teack=1;
 hw.reack=0;assert(start(0)!=0 && !starts);hw.reack=1;assert(test_start_tx(buf,0,0,5,10)!=0);
 assert(test_start_rx(buf,33,0,5,10)!=0);assert(start(0)==0);assert(test_start_rx(buf,24,0,5,10)!=0);
 uart->txdone(uart);assert(test_poll(1).state==BKPT_OP_ACTIVE);settle(&dma_tx);uart->tx=HAL_UART_TX_STATE_IDLE;hw.tx=0;
 assert(test_poll(1).state==BKPT_OP_DONE);assert(test_cancel(2).state==BKPT_OP_DONE && !abort_calls);
 assert(test_start_rx(buf,12,2,5,10)==0);uart->rxdone(uart,11,HAL_UART_RX_EVENT_TC);
 assert(test_poll(3).state==BKPT_OP_STOPPING && abort_calls==1);settle(&dma_rx);
 assert(test_poll(4).state==BKPT_OP_FAILED && test_poll(4).buffers_released);assert(start(5)!=0);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);
 hw.tx=0;uart->tx=HAL_UART_TX_STATE_IDLE;uart->error(uart); /* orphan DMA after peripheral error */
 assert(test_poll(1).state==BKPT_OP_STOPPING && abort_calls==1);settle(&dma_tx);assert(test_poll(2).state==BKPT_OP_FAILED);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);irqs[0]=0;
 assert(test_cancel(1).state==BKPT_OP_STOPPING && !irqs[0]);assert(test_poll(11).state==BKPT_OP_UNSAFE);
 settle(&dma_tx);assert(!test_poll(12).buffers_released);assert(start(12)!=0);
 reset();assert(test_init()==HAL_OK);start_result=HAL_ERROR;assert(start(0)==0);
 assert(test_poll(0).state==BKPT_OP_FAILED && test_poll(0).reason==BKPT_OP_START_ERROR);
 reset();context[0]=1;assert(test_init()==HAL_BUSY && !operation_initialized);context[0]=0;assert(test_init()==HAL_OK);
 assert(start(0)==0);context[1]=1;assert(test_cancel(1).state==BKPT_OP_ACTIVE && !abort_calls);
 context[1]=0;uart->error(uart);uart->txdone(uart);assert(test_poll(1).reason==BKPT_OP_IO_ERROR);
 for(unsigned rx=0;rx<2;rx++){
  reset();assert(test_init()==HAL_OK);assert((rx?test_start_rx(buf,12,0,5,10):start(0))==0);
  hw.CR1=511;hw.CR2=1;hw.CR3=15;
  assert(test_cancel(1).state==BKPT_OP_STOPPING && !(hw.CR1|hw.CR2|hw.CR3));
  hal_dma_handle_t*d=rx?&dma_rx:&dma_tx;assert(d->state==HAL_DMA_STATE_ABORT);
  d->state=HAL_DMA_STATE_IDLE;d->idle=1;d->en=0;d->p_xfer_cplt_cb(d); /* TC precedes SUSP */
  assert(test_poll(2).state==BKPT_OP_FAILED && test_poll(2).buffers_released);
  assert(rx?uart->rx==HAL_UART_RX_STATE_ACTIVE:uart->tx==HAL_UART_TX_STATE_ACTIVE); /* no invented HAL idle */
  uart->txdone(uart);assert(test_poll(3).state==BKPT_OP_FAILED);assert(start(4)!=0);
 }
 for(unsigned error=0;error<5;error++){
  reset();assert(test_init()==HAL_OK);assert(test_start_rx(buf,12,0,5,10)==0);
  settle(&dma_rx);uart->rx=HAL_UART_RX_STATE_IDLE;hw.rx=0;uart->rxdone(uart,12,HAL_UART_RX_EVENT_TC);
  if(error==0){uart->errors=1;}else{hw.errors=1U<<(error-1);}
  assert(test_poll(1).state==BKPT_OP_FAILED && test_poll(1).reason==BKPT_OP_IO_ERROR);
 }
 return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='bkpt-bus-adapter-tests-') as folder:
        for name, mock, reset, test in [
            ('spi', SPI, 'peripheral=(hal_spi_handle_t){.instance=(uintptr_t)&hw};', SPI_TEST),
            ('uart', UART, 'peripheral=(hal_uart_handle_t){0};operation_bytes=operation_receive=0;hw.teack=hw.reack=1;', UART_TEST),
        ]:
            bindings = {'unit': 'test', 'length_bytes': '32', 'tx_dma_irq': '0', 'rx_dma_irq': '1',
                        'peripheral_irq': '2', 'peripheral_symbol': '(&hw)'}
            fragment = (ROOT / 'catalog/components' / f'{name}-operation.inc').read_text()
            fragment = re.sub(r'\{\{([a-z_]+)\}\}', lambda m: bindings[m[1]], fragment)
            source = COMMON + (ROOT / 'catalog/components/operation.h').read_text() + mock + fragment + RESET.replace('RESET_PERIPHERAL', reset) + test
            c = Path(folder) / f'{name}.c'
            exe = Path(folder) / f'{name}.exe'
            c.write_text(source)
            subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
            print(f'{name}: ownership, cancellation, partial starts, deadlines and IRQ-context checks passed')

if __name__ == '__main__':
    main()
