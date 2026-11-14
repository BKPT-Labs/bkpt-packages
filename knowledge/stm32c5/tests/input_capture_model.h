// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* Stands in for stm32_hal.h when the input capture unit is compiled for the
 * host model (input_capture_model.c). It declares only what the unit's
 * template uses. The timer is reached through model_timer(), which the test
 * binds to the unit's peripheral symbol: every access to a timer register is
 * then a call into the model, where time advances and the caller can be
 * delayed. */
#ifndef INPUT_CAPTURE_MODEL_H
#define INPUT_CAPTURE_MODEL_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
  HAL_OK = 0x3C3C0001,
  HAL_ERROR = 0x3C3C0002,
  HAL_BUSY = 0x3C3C0003,
  HAL_INVALID_PARAM = 0x3C3C0004,
  HAL_TIMEOUT = 0x3C3C0005
} hal_status_t;

/* The registers the unit reads and writes, in the order the reference
 * manual gives the four compare registers. */
typedef struct {
  volatile uint32_t CR1;
  volatile uint32_t SR;
  volatile uint32_t CNT;
  volatile uint32_t PSC;
  volatile uint32_t ARR;
  volatile uint32_t CCR1;
  volatile uint32_t CCR2;
  volatile uint32_t CCR3;
  volatile uint32_t CCR4;
} TIM_TypeDef;
#define TIM_SR_UIF 0x0001U
#define TIM_SR_CC1IF 0x0002U
#define TIM_SR_CC1OF 0x0200U

TIM_TypeDef *model_timer(void);

typedef struct { int unused; } hal_tim_handle_t;
typedef struct {
  uint32_t prescaler;
  uint32_t period;
  uint32_t repetition_counter;
  int counter_mode;
  struct { int clock_source; } clock_sel;
} hal_tim_config_t;
typedef struct { int source; int polarity; int filter; } hal_tim_ic_channel_config_t;
typedef struct { int source; int prescaler; } hal_tim_ic_capture_unit_config_t;
typedef struct { int mode; int output_type; int pull; int speed; int alternate; } hal_gpio_config_t;
typedef enum { HAL_TIM_STATE_IDLE, HAL_TIM_STATE_ACTIVE } hal_tim_state_t;
typedef enum { HAL_GPIO_PIN_RESET, HAL_GPIO_PIN_SET } hal_gpio_pin_state_t;
#define HAL_TIM_COUNTER_UP 0
#define HAL_TIM_CLK_INTERNAL 0
#define HAL_TIM_IC_RISING_FALLING 0
#define HAL_TIM_FDIV1 0
#define HAL_TIM_IC_DIRECT 0
#define HAL_TIM_IC_DIV1 0
#define HAL_GPIO_MODE_ALTERNATE 0
#define HAL_GPIO_OUTPUT_PUSHPULL 0
#define HAL_GPIO_SPEED_FREQ_LOW 0

uint32_t HAL_RCC_TIM_GetKernelClkFreq(const TIM_TypeDef *timer);
uint32_t LL_TIM_GetPrescaler(const TIM_TypeDef *timer);
uint32_t LL_TIM_GetAutoReload(const TIM_TypeDef *timer);
uint32_t LL_TIM_IsEnabledCounter(const TIM_TypeDef *timer);
void LL_TIM_GenerateEvent_UPDATE(TIM_TypeDef *timer);
hal_status_t HAL_TIM_Init(hal_tim_handle_t *handle, int instance);
hal_status_t HAL_TIM_SetConfig(hal_tim_handle_t *handle, const hal_tim_config_t *config);
hal_status_t HAL_TIM_IC_SetConfigChannel(hal_tim_handle_t *handle, int channel, const hal_tim_ic_channel_config_t *config);
hal_status_t HAL_TIM_IC_SetConfigCaptureUnit(hal_tim_handle_t *handle, int unit, const hal_tim_ic_capture_unit_config_t *config);
hal_status_t HAL_GPIO_Init(int port, int pin, const hal_gpio_config_t *config);
hal_tim_state_t HAL_TIM_GetState(const hal_tim_handle_t *handle);
hal_status_t HAL_TIM_Start(hal_tim_handle_t *handle);
hal_status_t HAL_TIM_Stop(hal_tim_handle_t *handle);
hal_status_t HAL_TIM_IC_StartChannel(hal_tim_handle_t *handle, int channel);
hal_status_t HAL_TIM_IC_StopChannel(hal_tim_handle_t *handle, int channel);
uint32_t HAL_TIM_IC_ReadChannelCapturedValue(const hal_tim_handle_t *handle, int channel);
hal_gpio_pin_state_t HAL_GPIO_ReadPin(int port, int pin);
void model_clock(void);
void model_reset(void);

/* The processor's interrupt mask. The model delays the caller only where an
 * interrupt could, unless a case says the delay cannot be masked. */
uint32_t model_get_primask(void);
void model_disable_irq(void);
void model_set_primask(uint32_t mask);
#define __get_PRIMASK() model_get_primask()
#define __disable_irq() model_disable_irq()
#define __set_PRIMASK(mask) model_set_primask(mask)
#define __get_IPSR() 0U
#endif
