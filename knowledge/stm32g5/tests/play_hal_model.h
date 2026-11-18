// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* Host stand-in for stm32_hal.h: the STM32G5 HAL2 2.0.0 PLAY, RCC and GPIO interface the generated
   PLAY units use, with a model behind it (play_hal_model.c). Names and the encoding of the PLAY input
   source enumerators follow stm32g5xx_hal_play.h; other values are arbitrary. The model is not the
   hardware: it states the SDK functions' documented preconditions and the truth table encoding. */
#ifndef BKPT_PLAY_HAL_MODEL_H
#define BKPT_PLAY_HAL_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define USE_HAL_PLAY_MODULE 1U
#define USE_HAL_GPIO_MODULE 1U

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_INVALID_PARAM = 3, HAL_TIMEOUT = 4 } hal_status_t;

/* GPIO */
typedef enum { HAL_GPIOA = 0, HAL_GPIOB, HAL_GPIOC, HAL_GPIOD, HAL_GPIOE, HAL_GPIOF, HAL_GPIOG, HAL_GPIOH } hal_gpio_t;
#define MODEL_PIN(n) (1UL << (n))
#define HAL_GPIO_PIN_0 MODEL_PIN(0)
#define HAL_GPIO_PIN_1 MODEL_PIN(1)
#define HAL_GPIO_PIN_2 MODEL_PIN(2)
#define HAL_GPIO_PIN_3 MODEL_PIN(3)
#define HAL_GPIO_PIN_4 MODEL_PIN(4)
#define HAL_GPIO_PIN_5 MODEL_PIN(5)
#define HAL_GPIO_PIN_6 MODEL_PIN(6)
#define HAL_GPIO_PIN_7 MODEL_PIN(7)
#define HAL_GPIO_PIN_8 MODEL_PIN(8)
#define HAL_GPIO_PIN_9 MODEL_PIN(9)
#define HAL_GPIO_PIN_10 MODEL_PIN(10)
#define HAL_GPIO_PIN_11 MODEL_PIN(11)
#define HAL_GPIO_PIN_12 MODEL_PIN(12)
#define HAL_GPIO_PIN_13 MODEL_PIN(13)
#define HAL_GPIO_PIN_14 MODEL_PIN(14)
#define HAL_GPIO_PIN_15 MODEL_PIN(15)
typedef enum { HAL_GPIO_MODE_INPUT, HAL_GPIO_MODE_OUTPUT, HAL_GPIO_MODE_ALTERNATE, HAL_GPIO_MODE_ANALOG } hal_gpio_mode_t;
typedef enum { HAL_GPIO_OUTPUT_PUSHPULL, HAL_GPIO_OUTPUT_OPENDRAIN } hal_gpio_output_t;
typedef enum { HAL_GPIO_SPEED_FREQ_LOW, HAL_GPIO_SPEED_FREQ_MEDIUM, HAL_GPIO_SPEED_FREQ_HIGH, HAL_GPIO_SPEED_FREQ_VERY_HIGH } hal_gpio_speed_freq_t;
typedef enum { HAL_GPIO_PULL_NO, HAL_GPIO_PULL_UP, HAL_GPIO_PULL_DOWN } hal_gpio_pull_t;
typedef enum { HAL_GPIO_AF_0 = 0, HAL_GPIO_AF_14 = 14 } hal_gpio_af_t;
typedef enum { HAL_GPIO_PIN_RESET, HAL_GPIO_PIN_SET } hal_gpio_pin_state_t;
typedef struct
{
  hal_gpio_mode_t mode;
  hal_gpio_pull_t pull;
  hal_gpio_speed_freq_t speed;
  hal_gpio_output_t output_type;
  hal_gpio_af_t alternate;
  hal_gpio_pin_state_t init_state;
} hal_gpio_config_t;
hal_status_t HAL_GPIO_Init(hal_gpio_t gpiox, uint32_t pins, const hal_gpio_config_t *p_config);
void HAL_RCC_GPIOA_EnableClock(void);
void HAL_RCC_GPIOB_EnableClock(void);
void HAL_RCC_GPIOC_EnableClock(void);
void HAL_RCC_GPIOD_EnableClock(void);
void HAL_RCC_GPIOE_EnableClock(void);
void HAL_RCC_GPIOF_EnableClock(void);
void HAL_RCC_GPIOG_EnableClock(void);
void HAL_RCC_GPIOH_EnableClock(void);

/* RCC */
typedef enum { HAL_RCC_PLAY1_CLK_SRC_LSE = 0, HAL_RCC_PLAY1_CLK_SRC_LSI, HAL_RCC_PLAY1_CLK_SRC_PLL2P,
               HAL_RCC_PLAY1_CLK_SRC_HSE, HAL_RCC_PLAY1_CLK_SRC_HSI, HAL_RCC_PLAY1_CLK_SRC_HSI48 } hal_rcc_play1_clk_src_t;
typedef enum { HAL_RCC_PLAY1_CLK_DIVIDER1 = 0, HAL_RCC_PLAY1_CLK_DIVIDER2, HAL_RCC_PLAY1_CLK_DIVIDER4 } hal_rcc_play1_clk_divider_t;
void HAL_RCC_PLAY1_EnableClock(void);
void HAL_RCC_PLAY1_Reset(void);
hal_status_t HAL_RCC_LSI_Enable(void);
hal_status_t HAL_RCC_PLAY1_SetKernelClkSource(hal_rcc_play1_clk_src_t clk_src);
hal_status_t HAL_RCC_PLAY1_SetKernelClkDivider(hal_rcc_play1_clk_divider_t divider);
hal_rcc_play1_clk_src_t HAL_RCC_PLAY1_GetKernelClkSource(void);
hal_rcc_play1_clk_divider_t HAL_RCC_PLAY1_GetKernelClkDivider(void);

/* PLAY */
typedef enum { HAL_PLAY1 = 0x40016800 } hal_play_t;
typedef enum
{
  HAL_PLAY_STATE_RESET = 0, HAL_PLAY_STATE_INIT = (-2147483647 - 1), HAL_PLAY_STATE_IDLE = (1 << 30), HAL_PLAY_STATE_ACTIVE = (1 << 29)
} hal_play_state_t;
typedef struct hal_play_handle_s { hal_play_t instance; volatile hal_play_state_t global_state; } hal_play_handle_t;

#define HAL_PLAY_INSTANCE_ID_POS (29U)
#define HAL_PLAY_IN_MUX_POS (6U)
#define HAL_PLAY1_INSTANCE_ID (0x1UL << HAL_PLAY_INSTANCE_ID_POS)
typedef enum
{
  HAL_PLAY_IN0, HAL_PLAY_IN1, HAL_PLAY_IN2, HAL_PLAY_IN3, HAL_PLAY_IN4, HAL_PLAY_IN5, HAL_PLAY_IN6, HAL_PLAY_IN7,
  HAL_PLAY_IN8, HAL_PLAY_IN9, HAL_PLAY_IN10, HAL_PLAY_IN11, HAL_PLAY_IN12, HAL_PLAY_IN13, HAL_PLAY_IN14, HAL_PLAY_IN15
} hal_play_in_t;
#define MODEL_EXTERNAL(n) (HAL_PLAY1_INSTANCE_ID | ((uint32_t)(n) << HAL_PLAY_IN_MUX_POS) | 0U)
typedef enum
{
  HAL_PLAY1_IN_IN0_MUX0 = MODEL_EXTERNAL(0), HAL_PLAY1_IN_IN1_MUX1 = MODEL_EXTERNAL(1), HAL_PLAY1_IN_IN2_MUX2 = MODEL_EXTERNAL(2),
  HAL_PLAY1_IN_IN3_MUX3 = MODEL_EXTERNAL(3), HAL_PLAY1_IN_IN4_MUX4 = MODEL_EXTERNAL(4), HAL_PLAY1_IN_IN5_MUX5 = MODEL_EXTERNAL(5),
  HAL_PLAY1_IN_IN6_MUX6 = MODEL_EXTERNAL(6), HAL_PLAY1_IN_IN7_MUX7 = MODEL_EXTERNAL(7), HAL_PLAY1_IN_IN8_MUX8 = MODEL_EXTERNAL(8),
  HAL_PLAY1_IN_IN9_MUX9 = MODEL_EXTERNAL(9), HAL_PLAY1_IN_IN10_MUX10 = MODEL_EXTERNAL(10), HAL_PLAY1_IN_IN11_MUX11 = MODEL_EXTERNAL(11),
  HAL_PLAY1_IN_IN12_MUX12 = MODEL_EXTERNAL(12), HAL_PLAY1_IN_IN13_MUX13 = MODEL_EXTERNAL(13), HAL_PLAY1_IN_IN14_MUX14 = MODEL_EXTERNAL(14),
  HAL_PLAY1_IN_IN15_MUX15 = MODEL_EXTERNAL(15), HAL_PLAY_IN_SOURCE_INVALID = 0U
} hal_play_in_source_t;
typedef enum { HAL_PLAY_EDGE_DETECTION_BYPASSED, HAL_PLAY_EDGE_DETECTION_RISING, HAL_PLAY_EDGE_DETECTION_FALLING,
               HAL_PLAY_EDGE_DETECTION_RISING_FALLING } hal_play_edge_detection_mode_t;
typedef struct { uint32_t min_pulse_width; hal_play_edge_detection_mode_t mode; hal_play_in_source_t source; } hal_play_in_config_t;

typedef enum
{
  HAL_PLAY_LUT0, HAL_PLAY_LUT1, HAL_PLAY_LUT2, HAL_PLAY_LUT3, HAL_PLAY_LUT4, HAL_PLAY_LUT5, HAL_PLAY_LUT6, HAL_PLAY_LUT7,
  HAL_PLAY_LUT8, HAL_PLAY_LUT9, HAL_PLAY_LUT10, HAL_PLAY_LUT11, HAL_PLAY_LUT12, HAL_PLAY_LUT13, HAL_PLAY_LUT14, HAL_PLAY_LUT15
} hal_play_lut_t;
typedef enum { HAL_PLAY_LUT_INPUT0, HAL_PLAY_LUT_INPUT1, HAL_PLAY_LUT_INPUT2, HAL_PLAY_LUT_INPUT3 } hal_play_lut_input_t;
#define HAL_PLAY_LUT_INPUT_SOURCE_NBR 4U
/* Model encoding of lookup table input sources: 0-15 direct outputs, 16-31 registered outputs,
   32-47 filtered inputs, 48-63 software triggers. */
#define MODEL_LUT_SOURCES(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
typedef enum
{
#define MODEL_DIRECT(n) HAL_PLAY_LUT_INPUT_LUT##n##_OUT_DIRECT = (n),
#define MODEL_REG(n) HAL_PLAY_LUT_INPUT_LUT##n##_OUT_REGISTERED = 16 + (n),
#define MODEL_FILTER(n) HAL_PLAY_LUT_INPUT_FILTER##n = 32 + (n),
#define MODEL_SWTRIG(n) HAL_PLAY_LUT_INPUT_SWTRIG##n = 48 + (n),
  MODEL_LUT_SOURCES(MODEL_DIRECT) MODEL_LUT_SOURCES(MODEL_REG) MODEL_LUT_SOURCES(MODEL_FILTER) MODEL_LUT_SOURCES(MODEL_SWTRIG)
  HAL_PLAY_LUT_INPUT_DEFAULT = HAL_PLAY_LUT_INPUT_SWTRIG15
} hal_play_lut_input_source_t;
typedef enum { HAL_PLAY_LUT_CLK_GATE_OFF = 0, HAL_PLAY_LUT_CLK_GATE_ON = 1, HAL_PLAY_LUT_CLK_GATE_SW_ACK = 2 } hal_play_lut_clk_gate_source_t;
typedef struct
{
  hal_play_lut_t lut;
  uint32_t truth_table;
  hal_play_lut_input_source_t input_source[HAL_PLAY_LUT_INPUT_SOURCE_NBR];
  hal_play_lut_clk_gate_source_t clk_gate_source;
} hal_play_lut_config_t;

#define HAL_PLAY_LUT0_OUT_DIRECT (1UL << 0U)
#define HAL_PLAY_LUT0_OUT_REGISTERED (1UL << 16U)
#define HAL_PLAY_LUT1_OUT_DIRECT (1UL << 1U)
#define HAL_PLAY_LUT1_OUT_REGISTERED (1UL << 17U)
#define HAL_PLAY_LUT2_OUT_DIRECT (1UL << 2U)
#define HAL_PLAY_LUT2_OUT_REGISTERED (1UL << 18U)
#define HAL_PLAY_LUT3_OUT_DIRECT (1UL << 3U)
#define HAL_PLAY_LUT3_OUT_REGISTERED (1UL << 19U)
#define HAL_PLAY_LUT4_OUT_DIRECT (1UL << 4U)
#define HAL_PLAY_LUT4_OUT_REGISTERED (1UL << 20U)
#define HAL_PLAY_LUT5_OUT_DIRECT (1UL << 5U)
#define HAL_PLAY_LUT5_OUT_REGISTERED (1UL << 21U)
#define HAL_PLAY_LUT6_OUT_DIRECT (1UL << 6U)
#define HAL_PLAY_LUT6_OUT_REGISTERED (1UL << 22U)
#define HAL_PLAY_LUT7_OUT_DIRECT (1UL << 7U)
#define HAL_PLAY_LUT7_OUT_REGISTERED (1UL << 23U)
#define HAL_PLAY_LUT8_OUT_DIRECT (1UL << 8U)
#define HAL_PLAY_LUT8_OUT_REGISTERED (1UL << 24U)
#define HAL_PLAY_LUT9_OUT_DIRECT (1UL << 9U)
#define HAL_PLAY_LUT9_OUT_REGISTERED (1UL << 25U)
#define HAL_PLAY_LUT10_OUT_DIRECT (1UL << 10U)
#define HAL_PLAY_LUT10_OUT_REGISTERED (1UL << 26U)
#define HAL_PLAY_LUT11_OUT_DIRECT (1UL << 11U)
#define HAL_PLAY_LUT11_OUT_REGISTERED (1UL << 27U)
#define HAL_PLAY_LUT12_OUT_DIRECT (1UL << 12U)
#define HAL_PLAY_LUT12_OUT_REGISTERED (1UL << 28U)
#define HAL_PLAY_LUT13_OUT_DIRECT (1UL << 13U)
#define HAL_PLAY_LUT13_OUT_REGISTERED (1UL << 29U)
#define HAL_PLAY_LUT14_OUT_DIRECT (1UL << 14U)
#define HAL_PLAY_LUT14_OUT_REGISTERED (1UL << 30U)
#define HAL_PLAY_LUT15_OUT_DIRECT (1UL << 15U)
#define HAL_PLAY_LUT15_OUT_REGISTERED (1UL << 31U)

typedef enum
{
  HAL_PLAY_OUT0, HAL_PLAY_OUT1, HAL_PLAY_OUT2, HAL_PLAY_OUT3, HAL_PLAY_OUT4, HAL_PLAY_OUT5, HAL_PLAY_OUT6, HAL_PLAY_OUT7,
  HAL_PLAY_OUT8, HAL_PLAY_OUT9, HAL_PLAY_OUT10, HAL_PLAY_OUT11, HAL_PLAY_OUT12, HAL_PLAY_OUT13, HAL_PLAY_OUT14, HAL_PLAY_OUT15
} hal_play_out_t;
typedef struct { hal_play_out_t output_mux; uint32_t lut_output; } hal_play_out_config_t;
typedef struct { uint32_t lut_out_falling_mask; uint32_t lut_out_rising_mask; } hal_play_edge_trigger_config_t;

#define HAL_PLAY_SWTRIG0 (1UL << 0U)
#define HAL_PLAY_SWTRIG1 (1UL << 1U)
#define HAL_PLAY_SWTRIG2 (1UL << 2U)
#define HAL_PLAY_SWTRIG3 (1UL << 3U)
#define HAL_PLAY_SWTRIG4 (1UL << 4U)
#define HAL_PLAY_SWTRIG5 (1UL << 5U)
#define HAL_PLAY_SWTRIG6 (1UL << 6U)
#define HAL_PLAY_SWTRIG7 (1UL << 7U)
#define HAL_PLAY_SWTRIG8 (1UL << 8U)
#define HAL_PLAY_SWTRIG9 (1UL << 9U)
#define HAL_PLAY_SWTRIG10 (1UL << 10U)
#define HAL_PLAY_SWTRIG11 (1UL << 11U)
#define HAL_PLAY_SWTRIG12 (1UL << 12U)
#define HAL_PLAY_SWTRIG13 (1UL << 13U)
#define HAL_PLAY_SWTRIG14 (1UL << 14U)
#define HAL_PLAY_SWTRIG15 (1UL << 15U)
typedef enum { HAL_PLAY_SW_TRIGGER_RESET = 0U, HAL_PLAY_SW_TRIGGER_SET = 0xFFFFU } hal_play_sw_trigger_state_t;

hal_status_t HAL_PLAY_Init(hal_play_handle_t *hplay, hal_play_t instance);
hal_play_state_t HAL_PLAY_GetState(const hal_play_handle_t *hplay);
hal_status_t HAL_PLAY_INPUT_SetConfig(const hal_play_handle_t *hplay, const hal_play_in_config_t *p_config, uint32_t array_size);
hal_status_t HAL_PLAY_LUT_SetConfig(const hal_play_handle_t *hplay, const hal_play_lut_config_t *p_config, uint32_t array_size);
hal_status_t HAL_PLAY_OUTPUT_SetConfig(hal_play_handle_t *hplay, const hal_play_out_config_t *p_config, uint32_t array_size);
hal_status_t HAL_PLAY_Start(hal_play_handle_t *hplay, const hal_play_edge_trigger_config_t *p_config);
hal_status_t HAL_PLAY_Stop(hal_play_handle_t *hplay);
hal_play_in_source_t HAL_PLAY_INPUT_GetSource(const hal_play_handle_t *hplay, hal_play_in_t input_mux);
uint32_t HAL_PLAY_INPUT_GetMinPulseWidth(const hal_play_handle_t *hplay, hal_play_in_t input_mux);
hal_play_edge_detection_mode_t HAL_PLAY_INPUT_GetEdgeDetectionMode(const hal_play_handle_t *hplay, hal_play_in_t input_mux);
uint32_t HAL_PLAY_LUT_GetTruthTable(const hal_play_handle_t *hplay, hal_play_lut_t lut);
hal_play_lut_input_source_t HAL_PLAY_LUT_GetSource(const hal_play_handle_t *hplay, hal_play_lut_t lut, hal_play_lut_input_t lut_input);
hal_play_lut_clk_gate_source_t HAL_PLAY_LUT_GetClockGateSource(const hal_play_handle_t *hplay, hal_play_lut_t lut);
uint32_t HAL_PLAY_OUTPUT_GetSource(const hal_play_handle_t *hplay, hal_play_out_t output_mux);
hal_status_t HAL_PLAY_WriteSWTrigger(const hal_play_handle_t *hplay, uint32_t sw_triggers, hal_play_sw_trigger_state_t state,
                                     uint32_t timeout_ms);
hal_play_sw_trigger_state_t HAL_PLAY_ReadSWTrigger(const hal_play_handle_t *hplay, uint32_t sw_trig);

/* Model control and observation (not SDK) */
typedef struct { hal_gpio_t port; uint32_t pin; int play_input; int play_output; } model_route_t;
void model_reset(const model_route_t *routes, unsigned count);
void model_set_pad(hal_gpio_t port, uint32_t pin, int level);
void model_force_trigger(unsigned index, int level);
int model_pad_output(hal_gpio_t port, uint32_t pin); /* 0 or 1; -1 when the pad is not driven by PLAY1 */
int model_pad_configured(hal_gpio_t port, uint32_t pin, hal_gpio_pull_t *pull);
unsigned model_violations(void);
const char *model_last_violation(void);
void model_fault(const char *name, int on); /* "lsi", "truth_write", "write_busy", "kernel_clock" */
unsigned model_global_resets(void);
unsigned model_starts(void);
unsigned model_stops(void);

#endif /* BKPT_PLAY_HAL_MODEL_H */
