# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Compile real EXTI templates against a scripted HAL boundary, not silicon.

Independent translation units check that private state/callbacks cannot collide.
The mock deliberately reproduces HAL2's previous-state-dependent IRQ masking.
"""
from pathlib import Path
import os, re, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#ifndef MOCK_HAL_H
#define MOCK_HAL_H
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define USE_HAL_EXTI_MODULE 1
#define USE_HAL_EXTI_REGISTER_CALLBACKS 1
typedef enum {HAL_OK,HAL_ERROR,HAL_BUSY} hal_status_t;
typedef enum {HAL_EXTI_STATE_INIT,HAL_EXTI_STATE_IDLE,HAL_EXTI_STATE_ACTIVE} hal_exti_state_t;
typedef enum {HAL_EXTI_TRIGGER_RISING=1,HAL_EXTI_TRIGGER_FALLING=2,HAL_EXTI_TRIGGER_RISING_FALLING=3} hal_exti_trigger_t;
typedef struct exti hal_exti_handle_t;
struct exti {unsigned line; hal_exti_state_t state,previous; void(*cb)(hal_exti_handle_t*,hal_exti_trigger_t);};
typedef struct {hal_exti_trigger_t trigger; unsigned gpio_port;} hal_exti_config_t;
typedef struct {unsigned mode,pull;} hal_gpio_config_t;
enum {HAL_GPIO_MODE_INPUT=5,HAL_EXTI_MODE_INTERRUPT=1};
extern unsigned context[4],irq[2],pending[2],it[2],event[2],edge[2],clock_on,init_count;
extern int fail;
#define __get_IPSR() context[0]
#define __get_PRIMASK() context[1]
#define __get_BASEPRI() context[2]
#define __get_FAULTMASK() context[3]
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#define HAL_CORTEX_NVIC_DisableIRQ(n) (irq[n]=0)
#define HAL_CORTEX_NVIC_ClearPendingIRQ(n) (pending[n]=0)
void enable_irq(unsigned n);
#define HAL_CORTEX_NVIC_EnableIRQ(n) enable_irq(n)
#define HAL_CORTEX_NVIC_SetPriority(n,p,s) (assert((p)==5 && (s)==0),(void)(n))
#define pin_clock() (++clock_on)
#define LL_EXTI_DisableIT_0_31(n) (it[n]=0)
#define LL_EXTI_DisableEvent_0_31(n) (event[n]=0)
#define HAL_EXTI_GetLLInstance(h) ((h)->line)
#define HAL_EXTI_GetState(h) ((h)->state)
hal_status_t HAL_GPIO_Init(unsigned,unsigned,const hal_gpio_config_t*);
hal_status_t HAL_EXTI_Init(hal_exti_handle_t*,unsigned);
hal_status_t HAL_EXTI_SetConfig(hal_exti_handle_t*,const hal_exti_config_t*);
hal_status_t HAL_EXTI_RegisterTriggerCallback(hal_exti_handle_t*,void(*)(hal_exti_handle_t*,hal_exti_trigger_t));
hal_status_t HAL_EXTI_Enable(hal_exti_handle_t*,unsigned);
hal_status_t HAL_EXTI_Disable(hal_exti_handle_t*);
void HAL_EXTI_ClearPending(hal_exti_handle_t*,hal_exti_trigger_t);
void HAL_EXTI_IRQHandler(hal_exti_handle_t*);
#endif
'''
TEST = r'''// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#include "first.h"
#include "second.h"
#include <stdlib.h>
unsigned context[4],irq[2]={1,1},pending[2]={3,3},it[2]={1,1},event[2]={1,1},edge[2],clock_on,init_count;
int fail;
static unsigned inject_enable,inject_disable;
static unsigned calls[2],seen[2];
void irq_first(void);void irq_second(void);
static void cb_first(unsigned edges){++calls[0];seen[0]|=edges;}
static void cb_second(unsigned edges){++calls[1];seen[1]|=edges;}
void enable_irq(unsigned n){
 assert(it[n]);irq[n]=1;
 if(pending[n]){context[0]=1;if(n)irq_second();else irq_first();context[0]=0;}
}
hal_status_t HAL_GPIO_Init(unsigned port,unsigned pin,const hal_gpio_config_t*c){
 assert(clock_on && port==8 && (pin==16 || pin==32));
 assert(c->mode==HAL_GPIO_MODE_INPUT && c->pull==(pin==16?1U:2U));
 ++init_count;return fail==1?HAL_ERROR:HAL_OK;
}
hal_status_t HAL_EXTI_Init(hal_exti_handle_t*h,unsigned line){h->line=line;h->state=HAL_EXTI_STATE_INIT;return fail==2?HAL_ERROR:HAL_OK;}
hal_status_t HAL_EXTI_SetConfig(hal_exti_handle_t*h,const hal_exti_config_t*c){
 assert(!irq[h->line] && !it[h->line] && !event[h->line] && c->gpio_port==9);
 edge[h->line]=c->trigger;h->state=HAL_EXTI_STATE_IDLE;return fail==3?HAL_ERROR:HAL_OK;
}
hal_status_t HAL_EXTI_RegisterTriggerCallback(hal_exti_handle_t*h,void(*cb)(hal_exti_handle_t*,hal_exti_trigger_t)){
 h->cb=cb;return fail==4?HAL_ERROR:HAL_OK;
}
hal_status_t HAL_EXTI_Enable(hal_exti_handle_t*h,unsigned mode){
 assert(!irq[h->line] && !pending[h->line] && mode==HAL_EXTI_MODE_INTERRUPT);
 if(fail==5)return HAL_ERROR;
 h->state=h->previous=HAL_EXTI_STATE_ACTIVE;it[h->line]=1;
 if(inject_enable)pending[h->line]|=3;
 return HAL_OK;
}
hal_status_t HAL_EXTI_Disable(hal_exti_handle_t*h){
 assert(!irq[h->line]);if(fail==6)return HAL_ERROR;
 if(inject_disable)pending[h->line]|=3;
 it[h->line]=event[h->line]=0;h->state=HAL_EXTI_STATE_IDLE;return HAL_OK;
}
void HAL_EXTI_ClearPending(hal_exti_handle_t*h,hal_exti_trigger_t t){assert(t==3);pending[h->line]=0;}
void HAL_EXTI_IRQHandler(hal_exti_handle_t*h){
 unsigned p=pending[h->line];pending[h->line]=0;
 if(h->previous!=HAL_EXTI_STATE_ACTIVE)it[h->line]=0;
 if(p)h->cb(h,(hal_exti_trigger_t)p);
}
static void deliver(unsigned line,unsigned flags){
 pending[line]|=flags;
 if(irq[line] && it[line]){context[0]=1;if(line)irq_second();else irq_first();context[0]=0;}
}
int main(int argc,char**argv){
 fail=argc>1?atoi(argv[1]):0;
 if(fail>=1 && fail<=4){
  assert(first_init()==HAL_ERROR && !irq[0]);unsigned n=init_count;
  fail=0;assert(first_init()==HAL_ERROR && first_start(cb_first)==HAL_ERROR && first_stop()==HAL_ERROR);
  assert(init_count==n);return 0;
 }
 for(unsigned i=0;i<4;++i){context[i]=1;assert(first_init()==HAL_BUSY);context[i]=0;}
 assert(init_count==0);assert(first_start(cb_first)==HAL_ERROR && first_stop()==HAL_ERROR);
 assert(first_init()==HAL_OK && second_init()==HAL_OK && init_count==2);
 assert(first_init()==HAL_OK && init_count==2 && !irq[0] && !irq[1]);
 assert(!pending[0] && !pending[1] && edge[0]==3 && edge[1]==2);
 assert(first_start(NULL)==HAL_ERROR);
 for(unsigned i=0;i<4;++i){context[i]=1;assert(first_start(cb_first)==HAL_BUSY && first_stop()==HAL_BUSY);context[i]=0;}
 deliver(0,3);assert(first_start(cb_first)==(fail==5?HAL_ERROR:HAL_OK));
 if(fail==5){assert(!irq[0]);fail=0;assert(first_start(cb_first)==HAL_ERROR);return 0;}
 assert(!calls[0] && !pending[0]);assert(first_start(cb_first)==HAL_ERROR);
 assert(second_start(cb_second)==HAL_OK);
 deliver(0,1);deliver(0,2);deliver(1,2);
 assert(calls[0]==2 && seen[0]==3 && calls[1]==1 && seen[1]==2 && it[0] && it[1]);
 pending[0]=3;assert(first_stop()==(fail==6?HAL_ERROR:HAL_OK));
 if(fail==6){assert(!irq[0]);fail=0;assert(first_start(cb_first)==HAL_ERROR && first_stop()==HAL_ERROR);return 0;}
 assert(!irq[0] && !it[0] && !event[0] && !pending[0] && edge[0]==3);
 assert(first_stop()==HAL_OK);deliver(0,3);deliver(1,2);
 assert(calls[0]==2 && calls[1]==2);
 assert(first_start(cb_first)==HAL_OK && !pending[0]);deliver(0,3);
 assert(calls[0]==3 && seen[0]==3 && it[0]);
 assert(first_stop()==HAL_OK && second_stop()==HAL_OK);
 inject_enable=1;assert(first_start(cb_first)==HAL_OK);
 assert(calls[0]==4 && !pending[0]); /* immediate IRQ sees installed callback + started */
 inject_disable=1;assert(first_stop()==HAL_OK);
 assert(calls[0]==4 && !pending[0] && !irq[0]); /* edge while stopping is drained */
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='bkpt-exti-host-') as temporary:
    tmp = Path(temporary)
    (tmp/'stm32_hal.h').write_text(HEADER, encoding='utf-8')
    for unit, line in [('first',0), ('second',1)]:
        bindings = dict(unit=unit,irq=str(line),clock_enable='pin_clock',pull=str(line+1),
                        port='8',pin_mask=str(16<<line),line=str(line),
                        trigger='HAL_EXTI_TRIGGER_RISING_FALLING' if line==0 else 'HAL_EXTI_TRIGGER_FALLING',
                        exti_port='9',priority='5',handler='irq_'+unit)
        for ext in ['h','c']:
            source=(ROOT/f'catalog/components/gpio-input-interrupt.{ext}.in').read_text(encoding='utf-8')
            rendered=re.sub(r'\{\{(\w+)\}\}',lambda m:bindings[m[1]],source)
            (tmp/f'{unit}.{ext}').write_text(rendered,encoding='utf-8')
    (tmp/'test.c').write_text(TEST,encoding='utf-8')
    executable=tmp/'test.exe'
    subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
                    str(tmp/'first.c'),str(tmp/'second.c'),str(tmp/'test.c'),'-o',str(executable)],check=True)
    for failure in range(7):
        subprocess.run([str(executable),str(failure)],check=True)
print('EXTI host adapter: lifecycle, independent instances, second event, restart and failure paths passed')
