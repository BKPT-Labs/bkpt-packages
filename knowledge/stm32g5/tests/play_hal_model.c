// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* Host model behind play_hal_model.h. Each SDK function keeps the precondition and effect that
   stm32g5xx_hal_play.c, stm32g5xx_hal_rcc.c and stm32g5xx_hal_gpio.c of HAL2 2.0.0 state for it;
   a call outside a stated precondition counts as a violation. A lookup table output is computed
   with the SDK's documented encoding: bit (IN0 + 2*IN1 + 4*IN2 + 8*IN3) of the truth table.
   What the model leaves out: timing, filters (a filtered input reads its pad level), edge
   detection, registered outputs, interrupts. It is a test oracle, not the hardware. */
#include "play_hal_model.h"

#include <stdio.h>
#include <string.h>

#define PORTS 8
#define MUXES 16

static struct
{
  int play_clock, lsi_ready;
  hal_rcc_play1_clk_src_t kernel_source;
  hal_rcc_play1_clk_divider_t kernel_divider;
  int locked;
  const hal_play_handle_t *owner;
  uint32_t premux[MUXES], width[MUXES], mode[MUXES];
  uint32_t truth[MUXES], gate[MUXES];
  hal_play_lut_input_source_t lut_source[MUXES][4];
  uint32_t out_source[MUXES];
  uint32_t triggers;
  unsigned starts, stops, global_resets;
  int port_clock[PORTS];
  int af[PORTS][16], level[PORTS][16];
  hal_gpio_pull_t pull[PORTS][16];
  const model_route_t *routes;
  unsigned route_count;
  int fault_lsi, fault_truth, fault_busy, fault_kernel;
  unsigned violations;
  char last[200];
} m;

static void violation(const char *what)
{
  m.violations++;
  snprintf(m.last, sizeof m.last, "%s", what);
}

static int pin_index(uint32_t pin)
{
  for (int i = 0; i < 16; i++)
  {
    if (pin == (1UL << i)) { return i; }
  }
  return -1;
}

static const model_route_t *route_of_input(unsigned n)
{
  for (unsigned i = 0; i < m.route_count; i++)
  {
    if (m.routes[i].play_input == (int)n) { return &m.routes[i]; }
  }
  return NULL;
}

void model_reset(const model_route_t *routes, unsigned count)
{
  memset(&m, 0, sizeof m);
  m.kernel_source = HAL_RCC_PLAY1_CLK_SRC_LSE;     /* SDK: LSE is the PLAY kernel clock by default */
  m.kernel_divider = HAL_RCC_PLAY1_CLK_DIVIDER2;   /* not the unit's choice, so the unit must write it */
  m.routes = routes;
  m.route_count = count;
  for (unsigned i = 0; i < MUXES; i++)
  {
    for (unsigned j = 0; j < 4; j++) { m.lut_source[i][j] = HAL_PLAY_LUT_INPUT_DEFAULT; }
  }
}

void model_fault(const char *name, int on)
{
  if (strcmp(name, "lsi") == 0) { m.fault_lsi = on; }
  else if (strcmp(name, "truth_write") == 0) { m.fault_truth = on; }
  else if (strcmp(name, "write_busy") == 0) { m.fault_busy = on; }
  else if (strcmp(name, "kernel_clock") == 0) { m.fault_kernel = on; }
}

unsigned model_violations(void) { return m.violations; }
const char *model_last_violation(void) { return m.last; }
unsigned model_starts(void) { return m.starts; }
unsigned model_stops(void) { return m.stops; }
void model_set_pad(hal_gpio_t port, uint32_t pin, int level) { m.level[port][pin_index(pin)] = level; }
void model_force_trigger(unsigned index, int level)
{
  if (level) { m.triggers |= 1UL << index; } else { m.triggers &= ~(1UL << index); }
}

int model_pad_configured(hal_gpio_t port, uint32_t pin, hal_gpio_pull_t *pull)
{
  int i = pin_index(pin);
  if (pull != NULL) { *pull = m.pull[port][i]; }
  return m.af[port][i];
}

static int kernel_running(void)
{
  return !m.fault_kernel && m.kernel_source == HAL_RCC_PLAY1_CLK_SRC_LSI && m.lsi_ready;
}

static int lut_output(unsigned lut, int depth);

static int source_level(hal_play_lut_input_source_t source, int depth)
{
  unsigned s = (unsigned)source;
  if (s < 16U) { return lut_output(s, depth + 1); }
  if (s >= 48U) { return (int)((m.triggers >> (s - 48U)) & 1U); }
  if (s >= 32U)
  {
    unsigned mux = s - 32U;
    const model_route_t *route = route_of_input(mux);
    if (m.premux[mux] != 0U || route == NULL) { return -1; } /* not an external pad input */
    int i = pin_index(route->pin);
    if (m.af[route->port][i] != 14) { return -1; }
    return m.level[route->port][i];
  }
  return -1; /* registered outputs are not modeled */
}

static int lut_output(unsigned lut, int depth)
{
  unsigned index = 0;
  if (depth > 16) { return -1; }
  for (unsigned i = 0; i < 4; i++)
  {
    int bit = source_level(m.lut_source[lut][i], depth);
    if (bit < 0) { return -1; }
    index |= (unsigned)bit << i;
  }
  return (int)((m.truth[lut] >> index) & 1U);
}

int model_pad_output(hal_gpio_t port, uint32_t pin)
{
  for (unsigned i = 0; i < m.route_count; i++)
  {
    const model_route_t *r = &m.routes[i];
    if (r->port != port || r->pin != pin || r->play_output < 0) { continue; }
    if (m.af[port][pin_index(pin)] != 14 || m.owner == NULL || m.owner->global_state != HAL_PLAY_STATE_ACTIVE) { return -1; }
    uint32_t source = m.out_source[r->play_output];
    for (unsigned lut = 0; lut < 16; lut++)
    {
      if (source == (1UL << lut)) { return lut_output(lut, 0); }
    }
    return -1;
  }
  return -1;
}

/* RCC */
#define MODEL_GPIO_CLOCK(P, N) void HAL_RCC_GPIO##P##_EnableClock(void) { m.port_clock[N] = 1; }
MODEL_GPIO_CLOCK(A, 0) MODEL_GPIO_CLOCK(B, 1) MODEL_GPIO_CLOCK(C, 2) MODEL_GPIO_CLOCK(D, 3)
MODEL_GPIO_CLOCK(E, 4) MODEL_GPIO_CLOCK(F, 5) MODEL_GPIO_CLOCK(G, 6) MODEL_GPIO_CLOCK(H, 7)
unsigned model_global_resets(void) { return m.global_resets; }
void HAL_RCC_PLAY1_EnableClock(void) { m.play_clock = 1; }
void HAL_RCC_PLAY1_Reset(void)
{
  if (!m.play_clock || m.starts != 0U) { violation("global reset outside first initialization"); }
  m.global_resets++;
  m.locked = 0;
  m.triggers = 0U;
  memset(m.truth, 0, sizeof m.truth);
}

hal_status_t HAL_RCC_LSI_Enable(void)
{
  if (m.fault_lsi) { return HAL_ERROR; } /* SDK: HAL_ERROR when the ready flag does not rise in time */
  m.lsi_ready = 1;
  return HAL_OK;
}
hal_status_t HAL_RCC_PLAY1_SetKernelClkSource(hal_rcc_play1_clk_src_t clk_src) { m.kernel_source = clk_src; return HAL_OK; }
hal_status_t HAL_RCC_PLAY1_SetKernelClkDivider(hal_rcc_play1_clk_divider_t divider) { m.kernel_divider = divider; return HAL_OK; }
hal_rcc_play1_clk_src_t HAL_RCC_PLAY1_GetKernelClkSource(void) { return m.kernel_source; }
hal_rcc_play1_clk_divider_t HAL_RCC_PLAY1_GetKernelClkDivider(void) { return m.kernel_divider; }

/* GPIO */
hal_status_t HAL_GPIO_Init(hal_gpio_t gpiox, uint32_t pins, const hal_gpio_config_t *p_config)
{
  int i = pin_index(pins);
  if (i < 0 || p_config == NULL) { violation("GPIO_Init: one pin and a configuration expected"); return HAL_INVALID_PARAM; }
  if (!m.port_clock[gpiox]) { violation("GPIO_Init before the port clock is enabled"); }
  if (p_config->mode == HAL_GPIO_MODE_ALTERNATE)
  {
    for (unsigned r = 0; r < m.route_count; r++)
    {
      const model_route_t *route = &m.routes[r];
      if (route->port == gpiox && route->pin == pins && route->play_output >= 0
          && (m.owner == NULL || m.owner->global_state != HAL_PLAY_STATE_ACTIVE))
      {
        violation("PLAY output pad connected while PLAY1 is not started");
      }
    }
    m.af[gpiox][i] = (int)p_config->alternate;
  }
  m.pull[gpiox][i] = p_config->pull;
  if (m.level[gpiox][i] == 0 && p_config->pull == HAL_GPIO_PULL_UP) { m.level[gpiox][i] = 1; }
  return HAL_OK;
}

/* PLAY */
static int play_access(const hal_play_handle_t *h, const char *what)
{
  if (!m.play_clock) { violation(what); return 0; }
  if (h == NULL || (m.owner != NULL && m.owner != h)) { violation("PLAY1 used through a second handle"); return 0; }
  return 1;
}

static void need_state(const hal_play_handle_t *h, uint32_t states, const char *what)
{
  if (((uint32_t)h->global_state & states) == 0U) { violation(what); }
}

hal_status_t HAL_PLAY_Init(hal_play_handle_t *hplay, hal_play_t instance)
{
  if (!play_access(hplay, "PLAY_Init without the PLAY1 bus clock")) { return HAL_ERROR; }
  hplay->instance = instance;
  m.owner = hplay;
  hplay->global_state = HAL_PLAY_STATE_INIT;
  return HAL_OK;
}

hal_play_state_t HAL_PLAY_GetState(const hal_play_handle_t *hplay) { return hplay->global_state; }

#define CONFIG_STATES ((uint32_t)HAL_PLAY_STATE_INIT | (uint32_t)HAL_PLAY_STATE_IDLE)
#define READ_STATES (CONFIG_STATES | (uint32_t)HAL_PLAY_STATE_ACTIVE)

hal_status_t HAL_PLAY_INPUT_SetConfig(const hal_play_handle_t *hplay, const hal_play_in_config_t *p_config, uint32_t array_size)
{
  if (!play_access(hplay, "INPUT_SetConfig without clock")) { return HAL_ERROR; }
  need_state(hplay, CONFIG_STATES, "INPUT_SetConfig outside INIT/IDLE");
  if (p_config == NULL || array_size == 0U || array_size > MUXES) { return HAL_INVALID_PARAM; }
  m.locked = 0;
  for (uint32_t i = 0; i < array_size; i++)
  {
    uint32_t source = (uint32_t)p_config[i].source;
    uint32_t mux = (source >> HAL_PLAY_IN_MUX_POS) & 0x1FU;
    if ((source & (0x3UL << HAL_PLAY_INSTANCE_ID_POS)) != HAL_PLAY1_INSTANCE_ID || mux >= MUXES) { violation("bad input source"); continue; }
    if (p_config[i].min_pulse_width > 0xFFU) { violation("pulse width above 0xFF"); }
    const model_route_t *route = route_of_input(mux);
    if ((source & 0xFU) == 0U && (route == NULL || m.af[route->port][pin_index(route->pin)] != 14))
    {
      violation("external PLAY input configured before its pad");
    }
    m.premux[mux] = source & 0xFU;
    m.mode[mux] = (uint32_t)p_config[i].mode;
    m.width[mux] = p_config[i].min_pulse_width;
  }
  return HAL_OK;
}

hal_status_t HAL_PLAY_LUT_SetConfig(const hal_play_handle_t *hplay, const hal_play_lut_config_t *p_config, uint32_t array_size)
{
  if (!play_access(hplay, "LUT_SetConfig without clock")) { return HAL_ERROR; }
  need_state(hplay, CONFIG_STATES, "LUT_SetConfig outside INIT/IDLE");
  if (p_config == NULL || array_size == 0U || array_size > MUXES) { return HAL_INVALID_PARAM; }
  m.locked = 0;
  for (uint32_t i = 0; i < array_size; i++)
  {
    unsigned lut = (unsigned)p_config[i].lut;
    m.truth[lut] = (p_config[i].truth_table & 0xFFFFU) ^ (m.fault_truth ? 1U : 0U);
    for (unsigned j = 0; j < 4; j++) { m.lut_source[lut][j] = p_config[i].input_source[j]; }
    m.gate[lut] = (uint32_t)p_config[i].clk_gate_source;
  }
  return HAL_OK;
}

hal_status_t HAL_PLAY_OUTPUT_SetConfig(hal_play_handle_t *hplay, const hal_play_out_config_t *p_config, uint32_t array_size)
{
  if (!play_access(hplay, "OUTPUT_SetConfig without clock")) { return HAL_ERROR; }
  need_state(hplay, CONFIG_STATES, "OUTPUT_SetConfig outside INIT/IDLE");
  if (p_config == NULL || array_size == 0U || array_size > MUXES) { return HAL_INVALID_PARAM; }
  m.locked = 0;
  for (uint32_t i = 0; i < array_size; i++) { m.out_source[p_config[i].output_mux] = p_config[i].lut_output; }
  hplay->global_state = HAL_PLAY_STATE_IDLE;
  return HAL_OK;
}

hal_status_t HAL_PLAY_Start(hal_play_handle_t *hplay, const hal_play_edge_trigger_config_t *p_config)
{
  if (!play_access(hplay, "Start without clock")) { return HAL_ERROR; }
  need_state(hplay, (uint32_t)HAL_PLAY_STATE_IDLE, "Start outside IDLE");
  if (p_config == NULL) { return HAL_INVALID_PARAM; }
  if ((p_config->lut_out_falling_mask & p_config->lut_out_rising_mask) != 0U) { violation("edge masks overlap"); }
  hplay->global_state = HAL_PLAY_STATE_ACTIVE; /* the SDK changes state before the edge trigger write */
  m.locked = 1;
  m.starts++;
  if (m.fault_busy) { return HAL_BUSY; }
  return kernel_running() ? HAL_OK : HAL_TIMEOUT; /* the write-busy flag needs the kernel clock */
}

hal_status_t HAL_PLAY_Stop(hal_play_handle_t *hplay)
{
  if (!play_access(hplay, "Stop without clock")) { return HAL_ERROR; }
  need_state(hplay, (uint32_t)HAL_PLAY_STATE_ACTIVE, "Stop outside ACTIVE");
  m.locked = 0;
  m.stops++;
  m.triggers = 0U; /* SDK: kernel-clock features are held in reset; the model forgets trigger levels */
  hplay->global_state = HAL_PLAY_STATE_IDLE;
  return HAL_OK;
}

hal_play_in_source_t HAL_PLAY_INPUT_GetSource(const hal_play_handle_t *hplay, hal_play_in_t input_mux)
{
  need_state(hplay, READ_STATES, "INPUT_GetSource in RESET");
  return (hal_play_in_source_t)(HAL_PLAY1_INSTANCE_ID | ((uint32_t)input_mux << HAL_PLAY_IN_MUX_POS) | m.premux[input_mux]);
}
uint32_t HAL_PLAY_INPUT_GetMinPulseWidth(const hal_play_handle_t *hplay, hal_play_in_t input_mux)
{
  need_state(hplay, READ_STATES, "GetMinPulseWidth in RESET");
  return m.width[input_mux];
}
hal_play_edge_detection_mode_t HAL_PLAY_INPUT_GetEdgeDetectionMode(const hal_play_handle_t *hplay, hal_play_in_t input_mux)
{
  need_state(hplay, READ_STATES, "GetEdgeDetectionMode in RESET");
  return (hal_play_edge_detection_mode_t)m.mode[input_mux];
}
uint32_t HAL_PLAY_LUT_GetTruthTable(const hal_play_handle_t *hplay, hal_play_lut_t lut)
{
  need_state(hplay, READ_STATES, "GetTruthTable in RESET");
  return m.truth[lut];
}
hal_play_lut_input_source_t HAL_PLAY_LUT_GetSource(const hal_play_handle_t *hplay, hal_play_lut_t lut, hal_play_lut_input_t lut_input)
{
  need_state(hplay, READ_STATES, "LUT_GetSource in RESET");
  return m.lut_source[lut][lut_input];
}
hal_play_lut_clk_gate_source_t HAL_PLAY_LUT_GetClockGateSource(const hal_play_handle_t *hplay, hal_play_lut_t lut)
{
  need_state(hplay, READ_STATES, "GetClockGateSource in RESET");
  return (hal_play_lut_clk_gate_source_t)m.gate[lut];
}
uint32_t HAL_PLAY_OUTPUT_GetSource(const hal_play_handle_t *hplay, hal_play_out_t output_mux)
{
  need_state(hplay, READ_STATES, "OUTPUT_GetSource in RESET");
  return m.out_source[output_mux];
}

hal_status_t HAL_PLAY_WriteSWTrigger(const hal_play_handle_t *hplay, uint32_t sw_triggers, hal_play_sw_trigger_state_t state,
                                     uint32_t timeout_ms)
{
  (void)timeout_ms;
  if (!play_access(hplay, "WriteSWTrigger without clock")) { return HAL_ERROR; }
  need_state(hplay, (uint32_t)HAL_PLAY_STATE_ACTIVE, "WriteSWTrigger outside ACTIVE");
  if (m.fault_busy) { return HAL_BUSY; }
  if (!kernel_running()) { return HAL_TIMEOUT; }
  if (state == HAL_PLAY_SW_TRIGGER_RESET) { m.triggers &= ~sw_triggers; } else { m.triggers |= sw_triggers; }
  return HAL_OK;
}

hal_play_sw_trigger_state_t HAL_PLAY_ReadSWTrigger(const hal_play_handle_t *hplay, uint32_t sw_trig)
{
  need_state(hplay, READ_STATES, "ReadSWTrigger in RESET");
  return (m.triggers & sw_trig) != 0U ? HAL_PLAY_SW_TRIGGER_SET : HAL_PLAY_SW_TRIGGER_RESET;
}
