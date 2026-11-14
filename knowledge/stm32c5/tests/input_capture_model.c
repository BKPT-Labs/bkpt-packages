// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* Host model for the input capture unit.
 *
 * The unit under test is the template itself, rendered by
 * input_capture_model.py and compiled next to this file. Nothing of its
 * algorithm is repeated here. This file is the other side: a timer that
 * behaves as the reference manual describes the parts the unit uses, a pad
 * signal, a scheduler that delays the caller, and an oracle.
 *
 * Time is counted in quarters of a timer count (a count is one microsecond).
 * Every access the unit makes to the timer or the pad is one call into the
 * model. The call first lets time pass, a fixed cost plus any delay the
 * scheduler places there, and applies what the hardware did meanwhile, in
 * order of time:
 *   - the counter wraps every 65536 counts and sets the update flag;
 *   - the compare channel sets its flag when the count reaches its register;
 *   - an edge of the signal toggles the pad and, with the capture channel
 *     on, stores the count in the capture register and sets the capture
 *     flag, or the overcapture flag as well when the capture flag was set.
 * Reading the capture register clears the capture flag. Writing the status
 * register clears the flags written as 0 and leaves the ones written as 1.
 *
 * A delay sits between two accesses. Statements between two accesses see
 * neither the timer nor the time, so a delay at any statement boundary is
 * the same as a delay at the access after it; the scheduler can therefore
 * hold the caller at every boundary by choosing the access. A delay that an
 * interrupt causes is not placed while the unit has interrupts masked; a
 * case can also ask for delays that ignore the mask (a stopped processor).
 *
 * The oracle knows every edge. For the first rising edge the channel
 * captures, the falling edge after it and the next rising edge it knows the
 * true counts. A result is judged against the one property the unit states:
 *   HAL_OK      the period and high time are the true ones and the third
 *               edge lies before the time limit;
 *   HAL_TIMEOUT the three edges did not all occur before the limit;
 *   HAL_ERROR   always allowed, except in the strict mode, which keeps to
 *               the unit's stated operating condition and expects none.
 * Anything else, including a call that does not return, is a violation.
 */
#include "stm32_hal.h"
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hal_status_t unit_init(void);
hal_status_t unit_measure(uint32_t timeout_ms, uint32_t *period_us, uint32_t *high_us);

#ifndef MODEL_CAPTURE_CHANNEL
#error "MODEL_CAPTURE_CHANNEL must be 1 or 2"
#endif
#define Q 4ULL                 /* time units in one count */
#define COST 1ULL              /* time one access takes */
#define TURN (65536ULL * Q)
#define US(value) ((uint64_t)(value) * Q)
#define CAPTURE_FLAG (TIM_SR_CC1IF << (MODEL_CAPTURE_CHANNEL - 1))
#define OVER_FLAG (TIM_SR_CC1OF << (MODEL_CAPTURE_CHANNEL - 1))
#define HALF_CHANNEL ((MODEL_CAPTURE_CHANNEL == 1) ? 2 : 1)
#define HALF_FLAG (TIM_SR_CC1IF << (HALF_CHANNEL - 1))
#define MAX_EDGES 12
#define MAX_HOLDS 6

struct hold {
  uint64_t after;     /* time since the counter started from which the delay is due */
  unsigned skip;      /* accesses to let pass first, so that every boundary can be chosen */
  uint64_t length;
  int unmaskable;
  int done;
};
struct scenario {
  uint32_t timeout_ms;
  int level;                   /* pad level before the first edge */
  unsigned edges;
  uint64_t edge[MAX_EDGES];    /* time since the counter started, ascending */
  unsigned holds;
  struct hold hold[MAX_HOLDS];
  unsigned background;         /* short delays per million accesses */
  uint64_t background_seed;
};

/* ---- the timer, the pad and the scheduler ---- */
static TIM_TypeDef reg;
static uint32_t status_now;        /* the status register as the hardware holds it */
static uint32_t status_shown;      /* what the unit last saw in reg.SR */
static uint32_t capture_register;
static uint64_t now;
static uint64_t origin;            /* when the counter started */
static uint64_t channel_since;     /* when the capture channel was switched on */
static int running;
static int channel_on;
static int ever_started;
static int masked;
static int level;
static unsigned next_edge_index;
static struct scenario current;
static uint64_t background_state;
static uint64_t give_up_at = UINT64_MAX;
static jmp_buf gave_up;
static uint64_t accesses;

static uint64_t mix(uint64_t *state)
{
  uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
static uint64_t below(uint64_t *state, uint64_t bound) { return bound == 0 ? 0 : mix(state) % bound; }
static uint64_t between(uint64_t *state, uint64_t low, uint64_t high) { return low + below(state, high - low + 1); }

static volatile uint32_t *half_register(void) { return &(&reg.CCR1)[HALF_CHANNEL - 1]; }
static volatile uint32_t *capture_field(void) { return &(&reg.CCR1)[MODEL_CAPTURE_CHANNEL - 1]; }

static void pass_time(uint64_t length)
{
  const uint64_t end = now + length;
  while (running) {
    const uint64_t since = now - origin;
    const uint64_t wrap = origin + (since / TURN + 1) * TURN;
    uint64_t half = origin + (since / TURN) * TURN + (uint64_t)(*half_register() & 0xFFFFU) * Q;
    if (half <= now) { half += TURN; }
    uint64_t edge = UINT64_MAX;
    if (next_edge_index < current.edges) {
      edge = origin + current.edge[next_edge_index];
      if (edge < now) { edge = now; }
    }
    uint64_t first = wrap < half ? wrap : half;
    if (edge < first) { first = edge; }
    if (first > end) { break; }
    now = first;
    /* At one instant the counter events come first: the capture then holds the new count. */
    if (first == wrap) { status_now |= TIM_SR_UIF; }
    if (first == half) { status_now |= HALF_FLAG; }
    if (first == edge) {
      level ^= 1;
      if (channel_on) {
        if ((status_now & CAPTURE_FLAG) != 0U) { status_now |= OVER_FLAG; }
        capture_register = (uint32_t)(((now - origin) / Q) & 0xFFFFU);
        status_now |= CAPTURE_FLAG;
      }
      ++next_edge_index;
    }
  }
  now = end;
}
static uint64_t delay_here(void)
{
  if (running) {
    const uint64_t since = now - origin;
    for (unsigned index = 0; index < current.holds; ++index) {
      struct hold *hold = &current.hold[index];
      if (hold->done || since < hold->after) { continue; }
      if (masked && !hold->unmaskable) { continue; }
      if (hold->skip != 0U) { --hold->skip; continue; }
      hold->done = 1;
      return hold->length;
    }
  }
  if (current.background != 0U && !masked && below(&background_state, 1000000ULL) < current.background) {
    return between(&background_state, 1, US(60));
  }
  return 0;
}
static void show(void)
{
  reg.SR = status_shown = status_now;
  if (running) { reg.CNT = (uint32_t)(((now - origin) / Q) & 0xFFFFU); }
  *capture_field() = capture_register;
}
/* One access: what the unit wrote since the last one, then time, then what it can read now. */
static void access_point(void)
{
  if (reg.SR != status_shown) { status_now &= reg.SR; }
  ++accesses;
  pass_time(COST + delay_here());
  show();
  if (now > give_up_at) { longjmp(gave_up, 1); }
}
TIM_TypeDef *model_timer(void) { access_point(); return &reg; }
uint32_t model_get_primask(void) { access_point(); return (uint32_t)masked; }
void model_disable_irq(void) { masked = 1; }
void model_set_primask(uint32_t mask) { masked = (mask != 0U); }

uint32_t HAL_RCC_TIM_GetKernelClkFreq(const TIM_TypeDef *timer) { (void)timer; return 144000000U; }
uint32_t LL_TIM_GetPrescaler(const TIM_TypeDef *timer) { return timer->PSC; }
uint32_t LL_TIM_GetAutoReload(const TIM_TypeDef *timer) { return timer->ARR; }
uint32_t LL_TIM_IsEnabledCounter(const TIM_TypeDef *timer) { return timer->CR1 & 1U; }
void LL_TIM_GenerateEvent_UPDATE(TIM_TypeDef *timer)
{
  (void)timer;
  status_now |= TIM_SR_UIF;
  if (running) { origin = now; }
  reg.CNT = 0U;
  show();
}
hal_status_t HAL_TIM_Init(hal_tim_handle_t *handle, int instance) { (void)handle; (void)instance; return HAL_OK; }
hal_status_t HAL_TIM_SetConfig(hal_tim_handle_t *handle, const hal_tim_config_t *config)
{
  (void)handle;
  reg.PSC = config->prescaler;
  reg.ARR = config->period;
  return HAL_OK;
}
hal_status_t HAL_TIM_IC_SetConfigChannel(hal_tim_handle_t *handle, int channel, const hal_tim_ic_channel_config_t *config)
{
  (void)handle; (void)channel; (void)config;
  return HAL_OK;
}
hal_status_t HAL_TIM_IC_SetConfigCaptureUnit(hal_tim_handle_t *handle, int unit, const hal_tim_ic_capture_unit_config_t *config)
{
  (void)handle; (void)unit; (void)config;
  return HAL_OK;
}
hal_status_t HAL_GPIO_Init(int port, int pin, const hal_gpio_config_t *config) { (void)port; (void)pin; (void)config; return HAL_OK; }
hal_tim_state_t HAL_TIM_GetState(const hal_tim_handle_t *handle) { (void)handle; return running ? HAL_TIM_STATE_ACTIVE : HAL_TIM_STATE_IDLE; }
hal_status_t HAL_TIM_Start(hal_tim_handle_t *handle)
{
  (void)handle;
  access_point();
  running = 1;
  ever_started = 1;
  origin = now;
  reg.CR1 |= 1U;
  give_up_at = now + US((uint64_t)current.timeout_ms * 1000U) + US(30000000U);
  return HAL_OK;
}
hal_status_t HAL_TIM_Stop(hal_tim_handle_t *handle)
{
  (void)handle;
  access_point();
  running = 0;
  reg.CR1 &= ~1U;
  return HAL_OK;
}
hal_status_t HAL_TIM_IC_StartChannel(hal_tim_handle_t *handle, int channel)
{
  (void)handle; (void)channel;
  access_point();
  channel_on = 1;
  channel_since = now;
  return HAL_OK;
}
hal_status_t HAL_TIM_IC_StopChannel(hal_tim_handle_t *handle, int channel)
{
  (void)handle; (void)channel;
  access_point();
  channel_on = 0;
  return HAL_OK;
}
uint32_t HAL_TIM_IC_ReadChannelCapturedValue(const hal_tim_handle_t *handle, int channel)
{
  (void)handle; (void)channel;
  access_point();
  status_now &= ~CAPTURE_FLAG;
  show();
  return capture_register;
}
hal_gpio_pin_state_t HAL_GPIO_ReadPin(int port, int pin)
{
  (void)port; (void)pin;
  access_point();
  return level ? HAL_GPIO_PIN_SET : HAL_GPIO_PIN_RESET;
}
void model_clock(void) {}
void model_reset(void) { memset(&reg, 0, sizeof reg); status_now = status_shown = 0U; capture_register = 0U; }

/* ---- one case and its verdict ---- */
enum verdict { EXACT, TIMEOUT_TRUE, ERRORED, WRONG_VALUE, LATE_SUCCESS, FALSE_TIMEOUT, NO_RETURN, OTHER, VERDICTS };
static const char *const verdict_name[VERDICTS] = {
  "exact", "timeout_true", "error", "wrong_value", "late_success", "false_timeout", "no_return", "other"};
struct outcome {
  enum verdict verdict;
  hal_status_t status;
  uint32_t period, high;
  int have_three;
  uint64_t true_period, true_high, third;
};
static void run_case(const struct scenario *scenario, struct outcome *outcome)
{
  current = *scenario;
  background_state = scenario->background_seed;
  now = 0;
  origin = 0;
  channel_since = 0;
  running = channel_on = ever_started = masked = 0;
  level = scenario->level;
  next_edge_index = 0;
  status_now = status_shown = 0U;
  capture_register = 0U;
  reg.CR1 = reg.SR = reg.CNT = 0U;
  give_up_at = UINT64_MAX;
  memset(outcome, 0, sizeof *outcome);
  uint32_t period = 0xFFFFFFFFU;
  uint32_t high = 0xFFFFFFFFU;
  volatile int returned = 0;
  hal_status_t status = HAL_ERROR;
  if (setjmp(gave_up) == 0) {
    status = unit_measure(scenario->timeout_ms, &period, &high);
    returned = 1;
  }
  /* The oracle: the first rising edge after the channel was on, and the two edges after it. */
  uint64_t count[3] = {0, 0, 0};
  unsigned found = 0;
  int pad = scenario->level;
  for (unsigned index = 0; index < scenario->edges; ++index) {
    pad ^= 1;
    if (!ever_started || !(origin + scenario->edge[index] > channel_since)) { continue; }
    if (found == 0 && pad == 0) { continue; }
    if (found < 3) { count[found++] = scenario->edge[index] / Q; }
  }
  const uint64_t limit = (uint64_t)scenario->timeout_ms * 1000U;
  const int complete = found == 3 && count[2] < limit;
  outcome->status = status;
  outcome->period = period;
  outcome->high = high;
  outcome->have_three = found == 3;
  outcome->true_period = count[2] - count[0];
  outcome->true_high = count[1] - count[0];
  outcome->third = count[2];
  if (!returned) { outcome->verdict = NO_RETURN; }
  else if (status == HAL_OK) {
    if (found != 3 || period != outcome->true_period || high != outcome->true_high) { outcome->verdict = WRONG_VALUE; }
    else if (!complete) { outcome->verdict = LATE_SUCCESS; }
    else { outcome->verdict = EXACT; }
  } else if (status == HAL_TIMEOUT) { outcome->verdict = complete ? FALSE_TIMEOUT : TIMEOUT_TRUE; }
  else if (status == HAL_ERROR) { outcome->verdict = ERRORED; }
  else { outcome->verdict = OTHER; }
  if (status != HAL_OK && returned && (period != 0xFFFFFFFFU || high != 0xFFFFFFFFU)) { outcome->verdict = OTHER; }
  running = channel_on = masked = 0;
  reg.CR1 = 0U;
}

/* ---- tallies and the report ---- */
struct tally {
  uint64_t cases, verdicts[VERDICTS], strict_error, accesses;
  unsigned shown;
  char first[6][320];
  unsigned wrong_kinds;
  uint64_t wrong[8][5];   /* period, high, true period, true high, how often */
};
static void describe(const struct scenario *scenario, const struct outcome *outcome, uint64_t id, char *text, size_t size)
{
  int used = snprintf(text, size, "{\"case\":%" PRIu64 ",\"verdict\":\"%s\",\"timeout_ms\":%u,\"period\":%u,\"high\":%u,"
                      "\"true_period\":%" PRIu64 ",\"true_high\":%" PRIu64 ",\"third_edge\":%" PRIu64 ",\"edges_us\":[",
                      id, verdict_name[outcome->verdict], scenario->timeout_ms, outcome->period, outcome->high,
                      outcome->true_period, outcome->true_high, outcome->third);
  for (unsigned index = 0; index < scenario->edges && used < (int)size - 40; ++index) {
    used += snprintf(text + used, size - (size_t)used, "%s%" PRIu64, index ? "," : "", scenario->edge[index] / Q);
  }
  used += snprintf(text + used, size - (size_t)used, "],\"holds_us\":[");
  for (unsigned index = 0; index < scenario->holds && used < (int)size - 60; ++index) {
    used += snprintf(text + used, size - (size_t)used, "%s[%" PRIu64 ",%u,%" PRIu64 "]", index ? "," : "",
                     scenario->hold[index].after / Q, scenario->hold[index].skip, scenario->hold[index].length / Q);
  }
  snprintf(text + used, size - (size_t)used, "]}");
}
static void count_case(struct tally *tally, const struct scenario *scenario, int strict, uint64_t id)
{
  struct outcome outcome;
  const uint64_t before = accesses;
  run_case(scenario, &outcome);
  tally->accesses += accesses - before;
  ++tally->cases;
  ++tally->verdicts[outcome.verdict];
  int violation = outcome.verdict >= WRONG_VALUE;
  if (strict && outcome.verdict == ERRORED) { ++tally->strict_error; violation = 1; }
  if (violation && tally->shown < 6) { describe(scenario, &outcome, id, tally->first[tally->shown++], sizeof tally->first[0]); }
  if (outcome.verdict == WRONG_VALUE || outcome.verdict == LATE_SUCCESS) {
    unsigned kind = 0;
    while (kind < tally->wrong_kinds && !(tally->wrong[kind][0] == outcome.period && tally->wrong[kind][1] == outcome.high &&
                                          tally->wrong[kind][2] == outcome.true_period && tally->wrong[kind][3] == outcome.true_high)) { ++kind; }
    if (kind == tally->wrong_kinds && kind < 8) {
      tally->wrong[kind][0] = outcome.period; tally->wrong[kind][1] = outcome.high;
      tally->wrong[kind][2] = outcome.true_period; tally->wrong[kind][3] = outcome.true_high;
      tally->wrong[kind][4] = 0; ++tally->wrong_kinds;
    }
    if (kind < 8) { ++tally->wrong[kind][4]; }
  }
}
static void report(const char *mode, const struct tally *tally)
{
  uint64_t violations = tally->strict_error;
  for (int verdict = WRONG_VALUE; verdict < VERDICTS; ++verdict) { violations += tally->verdicts[verdict]; }
  printf("{\"mode\":\"%s\",\"channel\":%d,\"cases\":%" PRIu64 ",\"accesses\":%" PRIu64 ",\"violations\":%" PRIu64,
         mode, MODEL_CAPTURE_CHANNEL, tally->cases, tally->accesses, violations);
  for (int verdict = 0; verdict < VERDICTS; ++verdict) { printf(",\"%s\":%" PRIu64, verdict_name[verdict], tally->verdicts[verdict]); }
  printf(",\"strict_error\":%" PRIu64 ",\"wrong_results\":[", tally->strict_error);
  for (unsigned kind = 0; kind < tally->wrong_kinds; ++kind) {
    printf("%s{\"period\":%" PRIu64 ",\"high\":%" PRIu64 ",\"true_period\":%" PRIu64 ",\"true_high\":%" PRIu64 ",\"cases\":%" PRIu64 "}",
           kind ? "," : "", tally->wrong[kind][0], tally->wrong[kind][1], tally->wrong[kind][2], tally->wrong[kind][3], tally->wrong[kind][4]);
  }
  printf("],\"first\":[");
  for (unsigned index = 0; index < tally->shown; ++index) { printf("%s%s", index ? "," : "", tally->first[index]); }
  printf("]}\n");
}

/* ---- the reviewers' traces ---- */
static void three_edges(struct scenario *scenario, uint32_t timeout_ms, uint64_t rise, uint64_t fall, uint64_t next)
{
  memset(scenario, 0, sizeof *scenario);
  scenario->timeout_ms = timeout_ms;
  scenario->edges = 3;
  scenario->edge[0] = US(rise);
  scenario->edge[1] = US(fall);
  scenario->edge[2] = US(next);
}
/* One delay, tried at every boundary around a moment: from two counts before it to four
 * after it in quarter counts, and at each of the next ten accesses. */
static void around(struct tally *tally, struct scenario *scenario, uint64_t moment_us, uint64_t until_us, uint64_t length_us)
{
  uint64_t id = tally->cases;
  for (uint64_t offset = 0; offset <= 24; ++offset) {
    for (unsigned skip = 0; skip < 10; ++skip) {
      const uint64_t after = US(moment_us) - 8 + offset;
      scenario->holds = 1;
      scenario->hold[0] = (struct hold){after, skip, until_us ? US(until_us) - after : US(length_us), 0, 0};
      count_case(tally, scenario, 0, id++);
    }
  }
}
static void replay_codex(void)
{
  /* Rising edge at 1,000; the poll at 32,768 sees the half-turn mark; the thread is held
   * until 164,000; falling edge at 150,000, rising edge at 180,000. True 179,000 and 149,000. */
  struct tally tally = {0};
  struct scenario scenario;
  three_edges(&scenario, 300, 1000, 150000, 180000);
  around(&tally, &scenario, 32768, 164000, 0);
  three_edges(&scenario, 120, 1000, 150000, 180000);
  around(&tally, &scenario, 32768, 164000, 0);
  report("replay_codex", &tally);
}
static void replay_claude(void)
{
  /* Rising edge at 1,000; the look at 65,538 sees the overflow mark; the thread is held for
   * 70,000; falling edge at 140,000, rising edge at 150,000. True 149,000 and 139,000. */
  struct tally tally = {0};
  struct scenario scenario;
  three_edges(&scenario, 300, 1000, 140000, 150000);
  around(&tally, &scenario, 65538, 0, 70000);
  report("replay_claude", &tally);
}
/* A grid: one delay at every boundary around each of the first marks, in lengths that are
 * near half a turn, a turn and more. */
static void sweep(void)
{
  static const uint64_t lengths[] = {20000, 32758, 32768, 32778, 50000, 65526, 65536, 65546, 70000, 98304, 131067, 131077, 131232, 196611};
  static const uint64_t traces[2][3] = {{1000, 150000, 180000}, {40000, 90000, 200000}};
  struct tally tally = {0};
  struct scenario scenario;
  uint64_t id = 0;
  for (unsigned trace = 0; trace < 2; ++trace) {
    three_edges(&scenario, 300, traces[trace][0], traces[trace][1], traces[trace][2]);
    for (uint64_t mark = 1; mark <= 4; ++mark) {
      for (uint64_t offset = 0; offset <= 12; offset += 2) {
        for (unsigned skip = 0; skip < 8; ++skip) {
          for (unsigned length = 0; length < sizeof lengths / sizeof lengths[0]; ++length) {
            scenario.holds = 1;
            scenario.hold[0] = (struct hold){US(mark * 32768) - 6 + offset, skip, US(lengths[length]), 0, 0};
            count_case(&tally, &scenario, 0, id++);
          }
        }
      }
    }
  }
  report("sweep", &tally);
}

/* ---- seeded cases ---- */
static uint32_t some_timeout(uint64_t *state)
{
  static const uint32_t usual[] = {1, 2, 3, 10, 50, 100, 120, 200, 300};
  return below(state, 4) == 0 ? (uint32_t)between(state, 1, 300) : usual[below(state, sizeof usual / sizeof usual[0])];
}
static uint64_t some_hold(uint64_t *state)
{
  uint64_t us;
  switch (below(state, 8)) {
    case 0: us = between(state, 1, 100); break;
    case 1: us = between(state, 1000, 32000); break;
    case 2: us = 32768 - 40 + between(state, 0, 80); break;
    case 3: us = between(state, 33000, 65000); break;
    case 4: us = 65536 * between(state, 1, 3) - 60 + between(state, 0, 120); break;
    case 5: us = 65536 * between(state, 1, 2) + 32768 - 60 + between(state, 0, 120); break;
    case 6: us = between(state, 66000, 200000); break;
    default: us = between(state, 100, 1000); break;
  }
  return US(us) + below(state, Q);
}
/* Edges and delays without any restraint: the property has to hold whatever happens. */
static void any_case(struct scenario *scenario, uint64_t seed, int unmaskable)
{
  uint64_t state = seed;
  memset(scenario, 0, sizeof *scenario);
  scenario->timeout_ms = some_timeout(&state);
  const uint64_t limit = US((uint64_t)scenario->timeout_ms * 1000U);
  scenario->level = (int)below(&state, 2);
  scenario->edges = below(&state, 7) == 0 ? (unsigned)below(&state, 3) : (unsigned)between(&state, 3, 8);
  uint64_t at = 0;
  for (unsigned index = 0; index < scenario->edges; ++index) {
    uint64_t gap;
    if (index == 0) {
      switch (below(&state, 4)) {
        case 0: gap = between(&state, 1, US(200)); break;
        case 1: gap = between(&state, 1, US(3000)); break;
        case 2: gap = between(&state, 1, US(70000)); break;
        default: gap = between(&state, 1, limit + limit / 3); break;
      }
    } else {
      switch (below(&state, 12)) {
        case 0: gap = between(&state, 1, US(8)); break;
        case 1: case 2: gap = between(&state, US(10), US(300)); break;
        case 3: case 4: case 5: gap = between(&state, US(300), US(30000)); break;
        case 6: case 7: case 8: gap = between(&state, US(30000), US(140000)); break;
        case 9: gap = US(65536) * between(&state, 1, 2) - US(30) + between(&state, 0, US(60)); break;
        default: gap = between(&state, US(1000), limit / 2 + US(1000)); break;
      }
    }
    at += gap;
    scenario->edge[index] = at;
  }
  /* Delays are due around the moments that matter: a mark, an edge, the limit, the start. */
  scenario->holds = (unsigned)between(&state, 0, 4);
  for (unsigned index = 0; index < scenario->holds; ++index) {
    uint64_t moment;
    switch (below(&state, 4)) {
      case 0: moment = US(32768) * between(&state, 1, 8); break;
      case 1: moment = scenario->edges ? scenario->edge[below(&state, scenario->edges)] : 0; break;
      case 2: moment = limit; break;
      default: moment = below(&state, limit + US(1000)); break;
    }
    const uint64_t jitter = below(&state, 33);
    const unsigned skip = (unsigned)below(&state, 10);
    const uint64_t length = some_hold(&state);
    const int ignores_mask = unmaskable && below(&state, 2) == 0;
    scenario->hold[index] = (struct hold){moment + jitter > 16 ? moment + jitter - 16 : 0, skip, length, ignores_mask, 0};
  }
  scenario->background = below(&state, 3) == 0 ? (unsigned)between(&state, 1, 200) : 0U;
  scenario->background_seed = mix(&state);
}
/* Inside the operating condition the unit states: no delay of 32 ms or more, one delay at
 * a time, and every edge read before the next one arrives. The result must be the true
 * values, or a time-out that is true; an error is a violation here. */
static void strict_case(struct scenario *scenario, uint64_t seed)
{
  static const uint64_t longest_us[] = {0, 20, 500, 5000, 20000, 31900};
  uint64_t state = seed;
  memset(scenario, 0, sizeof *scenario);
  scenario->timeout_ms = some_timeout(&state);
  const uint64_t limit = US((uint64_t)scenario->timeout_ms * 1000U);
  const uint64_t longest = US(longest_us[below(&state, sizeof longest_us / sizeof longest_us[0])]);
  scenario->level = (int)below(&state, 2);
  scenario->edges = below(&state, 7) == 0 ? (unsigned)below(&state, 3) : (unsigned)between(&state, 3, 8);
  uint64_t at = 0;
  for (unsigned index = 0; index < scenario->edges; ++index) {
    uint64_t gap = longest + US(60);
    switch (below(&state, 6)) {
      case 0: gap += between(&state, 0, US(100)); break;
      case 1: case 2: gap += between(&state, 0, US(20000)); break;
      case 3: gap += between(&state, 0, limit / 3 + US(1000)); break;
      case 4: gap += US(65536) * between(&state, 1, 2) - US(30) + between(&state, 0, US(60)); break;
      default: gap += between(&state, 0, limit / 8 + US(200)); break;
    }
    at += gap;
    scenario->edge[index] = at;
  }
  scenario->holds = longest ? (unsigned)between(&state, 0, MAX_HOLDS) : 0U;
  uint64_t due = 0;
  for (unsigned index = 0; index < scenario->holds; ++index) {
    /* The next delay is due only after the last one is over and the loop has gone round. */
    uint64_t moment = due + between(&state, 0, limit / 3 + US(40000));
    if (below(&state, 3) == 0) {
      const uint64_t mark = US(32768) * ((moment / US(32768)) + 1) - 16 + below(&state, 33);
      if (mark >= due) { moment = mark; }
    }
    const uint64_t length = between(&state, 1, longest);
    const unsigned skip = (unsigned)below(&state, 10);
    scenario->hold[index] = (struct hold){moment, skip, length, 0, 0};
    due = moment + length + US(300);
  }
}
static void seeded(const char *mode, uint64_t cases, uint64_t seed)
{
  struct tally tally = {0};
  struct scenario scenario;
  for (uint64_t id = 0; id < cases; ++id) {
    const uint64_t case_seed = seed * 0x100000001B3ULL + id * 0x9E3779B97F4A7C15ULL;
    if (strcmp(mode, "strict") == 0) { strict_case(&scenario, case_seed); }
    else { any_case(&scenario, case_seed, strcmp(mode, "unmaskable") == 0); }
    count_case(&tally, &scenario, strcmp(mode, "strict") == 0, id);
  }
  report(mode, &tally);
}

int main(int count, char **argument)
{
  if (unit_init() != HAL_OK) { printf("{\"mode\":\"init\",\"violations\":1}\n"); return 2; }
  if (count >= 2 && strcmp(argument[1], "replay_codex") == 0) { replay_codex(); return 0; }
  if (count >= 2 && strcmp(argument[1], "replay_claude") == 0) { replay_claude(); return 0; }
  if (count >= 2 && strcmp(argument[1], "sweep") == 0) { sweep(); return 0; }
  if (count >= 4) { seeded(argument[1], strtoull(argument[2], NULL, 10), strtoull(argument[3], NULL, 10)); return 0; }
  fprintf(stderr, "usage: model replay_codex | replay_claude | sweep | any CASES SEED | unmaskable CASES SEED | strict CASES SEED\n");
  return 2;
}
