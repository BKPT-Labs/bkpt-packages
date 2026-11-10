# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Compile the actual operation fragments against a scripted host HAL boundary.

These tests exercise ownership/order/error paths, not peripheral simulation.
Run with a host C compiler on PATH; outputs are temporary and removed.
"""
from pathlib import Path
import os, re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
COMMON=r'''// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define USE_HAL_I2C_REGISTER_CALLBACKS 1
#define USE_HAL_I2C_DMA 1
typedef enum {HAL_OK,HAL_ERROR,HAL_BUSY} hal_status_t;
typedef enum {HAL_DMA_STATE_IDLE,HAL_DMA_STATE_ACTIVE,HAL_DMA_STATE_ABORT,HAL_DMA_STATE_SUSPEND} hal_dma_state_t;
typedef struct dma {hal_dma_state_t state; unsigned idle,en; void(*done)(struct dma*);void(*error)(struct dma*);void(*abort)(struct dma*);} hal_dma_handle_t;
#define p_xfer_abort_cb abort
static unsigned context[4],irqs[4]={1,1,1,1},pending[4],abort_calls;
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
#define LL_DMA_FLAG_ALL 255
static hal_status_t operation_hardware_init(void){return init_result;}
static hal_status_t HAL_DMA_Abort_IT(hal_dma_handle_t *d){
 assert(d->state==HAL_DMA_STATE_IDLE || d->state==HAL_DMA_STATE_ACTIVE);++abort_calls;
 if(d->idle){d->state=HAL_DMA_STATE_IDLE;return HAL_ERROR;}
 d->state=HAL_DMA_STATE_ABORT;return HAL_OK;
}
static void suspend_irq(hal_dma_handle_t*d){assert(d->state==HAL_DMA_STATE_ABORT);d->state=HAL_DMA_STATE_IDLE;d->idle=1;d->en=0;assert(d->abort);d->abort(d);}
'''
DMA=r'''
static hal_dma_handle_t dma;
#define HAL_DMA_RegisterXferCpltCallback(d,cb) ((d)->done=(cb),HAL_OK)
#define HAL_DMA_RegisterXferErrorCallback(d,cb) ((d)->error=(cb),HAL_OK)
#define HAL_DMA_RegisterXferAbortCallback(d,cb) ((d)->abort=(cb),HAL_OK)
static hal_status_t HAL_DMA_StartDirectXfer_IT(hal_dma_handle_t*d,uint32_t s,uint32_t t,uint32_t n){
 (void)s;(void)t;(void)n;assert(!irqs[0]);if(start_result!=HAL_OK)return start_result;
 d->state=HAL_DMA_STATE_ACTIVE;d->en=1;d->idle=0;return HAL_OK;
}
'''
DMA_TEST=r'''
static void reset(void){memset(&operation,0,sizeof(operation));operation_complete=operation_error=operation_initialized=0;dma=(hal_dma_handle_t){.idle=1};memset(context,0,sizeof(context));irqs[0]=1;status=HAL_ERROR;init_result=start_result=HAL_OK;abort_calls=0;}
static int start(uint32_t now){return test_start((void*)0x20000000,(void*)0x20001000,now,5,10);}
int main(void){
 reset();init_result=HAL_BUSY;assert(test_init()==HAL_BUSY);assert(test_init()==HAL_BUSY);assert(start(0)!=0);
 reset();context[0]=1;assert(test_init()==HAL_BUSY);context[0]=0;assert(test_init()==HAL_OK);
 assert(test_start((void*)0x20000001,(void*)0x20001000,0,5,10)!=0);
 assert(test_start((void*)0x20000000,(void*)0x20000004,0,5,10)!=0);
 assert(test_start((void*)0xfffffff8,(void*)0x20001000,0,5,10)!=0);
 assert(start(0)==0);assert(start(1)!=0);assert(test_poll(4).state==BKPT_OP_ACTIVE);
 dma.done(&dma);assert(!test_poll(4).buffers_released);dma.state=HAL_DMA_STATE_IDLE;dma.idle=1;dma.en=0;
 assert(test_poll(4).state==BKPT_OP_DONE);assert(start(4)==0);dma.error(&dma);
 assert(test_poll(5).state==BKPT_OP_STOPPING && abort_calls==1);suspend_irq(&dma);
 assert(test_poll(6).state==BKPT_OP_FAILED && test_poll(6).buffers_released);assert(start(7)!=0);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);irqs[0]=0;
 assert(test_cancel(1).state==BKPT_OP_STOPPING);assert(irqs[0]==0);assert(test_poll(10).state==BKPT_OP_STOPPING);
 assert(test_poll(11).state==BKPT_OP_UNSAFE);suspend_irq(&dma);assert(!test_poll(12).buffers_released);
 reset();assert(test_init()==HAL_OK);start_result=HAL_ERROR;assert(start(0)==0);assert(test_poll(0).state==BKPT_OP_FAILED);
 reset();assert(test_init()==HAL_OK);assert(start(0)==0);dma.idle=1; /* idle but EN set is not proof */
 assert(test_cancel(1).state==BKPT_OP_STOPPING);assert(test_poll(11).state==BKPT_OP_UNSAFE);
 return 0;
}
'''
I2C=r'''
#define I2C_CR1_TXIE 1U
#define I2C_CR1_RXIE 2U
#define I2C_CR1_ADDRIE 4U
#define I2C_CR1_NACKIE 8U
#define I2C_CR1_STOPIE 16U
#define I2C_CR1_TCIE 32U
#define I2C_CR1_ERRIE 64U
typedef struct {unsigned CR1,tx,rx,busy;} I2C_TypeDef;
typedef enum {HAL_I2C_STATE_IDLE,HAL_I2C_STATE_TX,HAL_I2C_STATE_RX,HAL_I2C_STATE_ABORT}hal_i2c_state_t;
enum {HAL_I2C_MODE_NONE,HAL_I2C_MODE_MASTER};
typedef struct i2c {uintptr_t instance;hal_i2c_state_t state;unsigned mode;void*xfer_isr;void(*done)(struct i2c*);void(*error)(struct i2c*);void(*abort)(struct i2c*);}hal_i2c_handle_t;
static I2C_TypeDef hw;static hal_i2c_handle_t peripheral;static hal_dma_handle_t dma_tx,dma_rx;
#define HAL_I2C_GetState(h) ((h)->state)
#define LL_I2C_DisableIT(h,m) ((h)->CR1&=~(m))
#define LL_I2C_IsEnabledDMAReq_TX(h) ((h)->tx)
#define LL_I2C_IsEnabledDMAReq_RX(h) ((h)->rx)
#define LL_I2C_DisableDMAReq_TX(h) ((h)->tx=0)
#define LL_I2C_DisableDMAReq_RX(h) ((h)->rx=0)
#define LL_I2C_IsActiveFlag_BUSY(h) ((h)->busy)
#define HAL_I2C_MASTER_RegisterTxCpltCallback(h,cb) ((h)->done=(cb),HAL_OK)
#define HAL_I2C_MASTER_RegisterRxCpltCallback(h,cb) ((h)->done=(cb),HAL_OK)
#define HAL_I2C_RegisterErrorCallback(h,cb) ((h)->error=(cb),HAL_OK)
#define HAL_I2C_RegisterAbortCpltCallback(h,cb) ((h)->abort=(cb),HAL_OK)
static hal_status_t HAL_I2C_MASTER_Abort_IT(hal_i2c_handle_t*h,unsigned addr){(void)addr;assert(h->state==HAL_I2C_STATE_TX || h->state==HAL_I2C_STATE_RX);h->state=HAL_I2C_STATE_ABORT;return HAL_OK;}
static hal_status_t mock_start(hal_i2c_handle_t*h,unsigned addr,void*b,unsigned n,unsigned rx){
 (void)addr;(void)b;(void)n;assert(!irqs[0]&&!irqs[1]&&!irqs[2]&&!irqs[3]);
 if(start_result==HAL_BUSY)return HAL_BUSY;
 h->xfer_isr=(void*)1;
 if(start_result!=HAL_OK)return start_result;
 h->state=rx?HAL_I2C_STATE_RX:HAL_I2C_STATE_TX;h->mode=HAL_I2C_MODE_MASTER;
 hal_dma_handle_t*d=rx?&dma_rx:&dma_tx;d->state=HAL_DMA_STATE_ACTIVE;d->en=1;d->idle=0;
 if(rx)hw.rx=1;else hw.tx=1;return HAL_OK;
}
#define HAL_I2C_MASTER_Receive_DMA(h,a,b,n) mock_start(h,a,(void*)b,n,1)
#define HAL_I2C_MASTER_Transmit_DMA(h,a,b,n) mock_start(h,a,(void*)b,n,0)
'''
I2C_TEST=r'''
static void reset(void){memset(&operation,0,sizeof(operation));operation_complete=operation_error=operation_initialized=0;memset(&hw,0,sizeof(hw));peripheral=(hal_i2c_handle_t){.instance=(uintptr_t)&hw};dma_tx=dma_rx=(hal_dma_handle_t){.idle=1};memset(context,0,sizeof(context));for(int i=0;i<4;++i)irqs[i]=1;status=HAL_ERROR;init_result=start_result=HAL_OK;abort_calls=0;}
static unsigned char buffer[12];
static int start(void){return test_start_rx(buffer,12,0,25,10);}
/* Script the source-documented STOP path: peripheral owns the first DMA abort. */
static void i2c_abort_done(hal_dma_handle_t*d){(void)d;peripheral.state=HAL_I2C_STATE_IDLE;peripheral.abort(&peripheral);}
static void stop_irq(void){assert(peripheral.state==HAL_I2C_STATE_ABORT);dma_rx.abort=i2c_abort_done;(void)HAL_DMA_Abort_IT(&dma_rx);peripheral.mode=HAL_I2C_MODE_NONE;peripheral.xfer_isr=NULL;operation_irq_settle();}
int main(void){
 reset();init_result=HAL_BUSY;assert(test_init()==HAL_BUSY && start()!=0);
 reset();assert(test_init()==HAL_OK);assert(start()==0);assert(start()!=0);
 assert(test_cancel(1).state==BKPT_OP_STOPPING);assert(abort_calls==0 && hw.rx==1);
 /* No direct abort before STOP; STOP-before-SUSP invokes it exactly once. */
 stop_irq();assert(abort_calls==1);assert(test_poll(2).state==BKPT_OP_STOPPING);assert(abort_calls==1);
 suspend_irq(&dma_rx);operation_irq_settle();assert(test_poll(3).state==BKPT_OP_FAILED);assert(test_poll(3).buffers_released);
 reset();assert(test_init()==HAL_OK);assert(start()==0);assert(test_cancel(1).state==BKPT_OP_STOPPING);
 assert(test_poll(11).state==BKPT_OP_UNSAFE && hw.rx==1 && abort_calls==0);
 reset();assert(test_init()==HAL_OK);assert(start()==0);
 /* ARLO/error releases the peripheral software while DMA is still active. */
 peripheral.state=HAL_I2C_STATE_IDLE;peripheral.mode=HAL_I2C_MODE_NONE;peripheral.xfer_isr=NULL;peripheral.error(&peripheral);
 hw.CR1=I2C_CR1_STOPIE|I2C_CR1_TCIE;operation_irq_settle();assert(hw.CR1==0 && hw.rx==0);
 assert(dma_rx.abort==NULL);assert(test_poll(1).state==BKPT_OP_STOPPING && abort_calls==1);suspend_irq(&dma_rx);
 assert(test_poll(2).state==BKPT_OP_FAILED && test_poll(2).buffers_released);
 reset();assert(test_init()==HAL_OK);assert(start()==0);
 dma_rx.state=HAL_DMA_STATE_IDLE;dma_rx.idle=1;dma_rx.en=0;
 peripheral.state=HAL_I2C_STATE_IDLE;peripheral.mode=HAL_I2C_MODE_NONE;peripheral.xfer_isr=NULL;
 peripheral.done(&peripheral);operation_irq_settle();assert(test_poll(1).state==BKPT_OP_DONE);
 assert(test_start_tx(buffer,4,2,25,10)==0);
 reset();assert(test_init()==HAL_OK);start_result=HAL_BUSY;assert(start()==0);assert(test_poll(0).state==BKPT_OP_FAILED);
 reset();assert(test_init()==HAL_OK);start_result=HAL_ERROR;assert(start()==0);assert(test_poll(10).state==BKPT_OP_UNSAFE);
 reset();assert(test_init()==HAL_OK);assert(test_start_rx(buffer,0,0,25,10)!=0);assert(test_start_rx(buffer,13,0,25,10)!=0);
 assert(start()==0);irqs[1]=0;context[1]=1;assert(test_poll(25).state==BKPT_OP_ACTIVE);context[1]=0;
 assert(test_poll(25).state==BKPT_OP_STOPPING && irqs[1]==0);
 return 0;
}
'''
def main():
    bindings=dict(unit='test',width_bytes='4',length_bytes='12',device_address_7bit='80',dma_irq='0',tx_dma_irq='0',rx_dma_irq='1',event_irq='2',error_irq='3')
    with tempfile.TemporaryDirectory(prefix='bkpt-operation-tests-') as folder:
        out=Path(folder)
        for name,mock,test in [('dma-copy',DMA,DMA_TEST),('i2c',I2C,I2C_TEST)]:
            fragment=(ROOT/'catalog/components'/f'{name}-operation.inc').read_text()
            fragment=re.sub(r'\{\{(\w+)\}\}',lambda m:bindings[m[1]],fragment)
            source=COMMON+(ROOT/'catalog/components/operation.h').read_text()+mock+fragment+test
            c=out/(name+'.c');c.write_text(source)
            exe=out/(name+'.exe')
            subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
            print(name+': scripted adapter checks passed')
        exe=out/'lifecycle.exe'
        subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/operation_lifecycle.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        print('generic lifecycle boundary checks passed')
if __name__=='__main__':main()
