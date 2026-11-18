// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
/* Drives generated PLAY units against play_hal_model.c. scenario.h is written by
   play_units_host.py: the units rendered from graph.json, their pads and the expected output of
   each function, computed from its expression and not from the unit's truth table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scenario.h"

static unsigned failures;

#define CHECK(cond, ...)                                                   \
  do                                                                       \
  {                                                                        \
    if (!(cond))                                                           \
    {                                                                      \
      failures++;                                                          \
      printf("FAIL %s:%d: ", __func__, __LINE__);                          \
      printf(__VA_ARGS__);                                                 \
      printf("\n");                                                        \
    }                                                                      \
  } while (0)

static void reset(void) { model_reset(ROUTES, (unsigned)(sizeof ROUTES / sizeof ROUTES[0])); }

static void no_violation(const char *where)
{
  CHECK(model_violations() == 0U, "%s: %u SDK precondition violations, last: %s", where, model_violations(), model_last_violation());
}

/* Every input combination of one unit, the others held still: the output pad follows the
   function, and the default source of the unused table inputs (software trigger 15) does not
   change it. */
static void exhaust(const unit_t *u)
{
  for (unsigned combo = 0; combo < 4U; combo++)
  {
    unsigned a = combo & 1U, b = (combo >> 1) & 1U;
    if (u->inputs == 1 && u->set == NULL && b != 0U) { continue; }
    model_set_pad(u->in_port[0], u->in_pin[0], (int)a);
    if (u->inputs == 2) { model_set_pad(u->in_port[1], u->in_pin[1], (int)b); }
    if (u->set != NULL) { CHECK(u->set(b) == HAL_OK, "%s: set_trigger(%u)", u->name, b); }
    if (u->get != NULL)
    {
      uint32_t level = 9U;
      CHECK(u->get(&level) == HAL_OK && level == b, "%s: get_trigger reads %u, wrote %u", u->name, (unsigned)level, b);
    }
    unsigned expected = (u->expected >> combo) & 1U;
    for (int t15 = 0; t15 < 2; t15++)
    {
      if (u->trigger != 15U) { model_force_trigger(15U, t15); }
      int out = model_pad_output(u->out_port, u->out_pin);
      CHECK(out == (int)expected, "%s: A=%u B=%u T15=%d gives %d, expected %u", u->name, a, b, t15, out, expected);
    }
  }
}

static void init_all(void)
{
  for (unsigned i = 0; i < UNIT_COUNT; i++) { CHECK(UNITS[i].init() == HAL_OK, "%s: init", UNITS[i].name); }
}

static void compose(void)
{
  reset();
  init_all();
  no_violation("compose init");
  CHECK(model_global_resets() == 1U, "global reset must run once for the composition");
  CHECK(model_starts() == UNIT_COUNT && model_stops() == UNIT_COUNT - 1U, "starts %u stops %u", model_starts(), model_stops());
  for (unsigned i = 0; i < UNIT_COUNT; i++)
  {
    const unit_t *u = &UNITS[i];
    for (int k = 0; k < u->inputs; k++)
    {
      hal_gpio_pull_t pull;
      CHECK(model_pad_configured(u->in_port[k], u->in_pin[k], &pull) == 14 && (int)pull == u->in_pull[k], "%s: input pad %d", u->name, k);
    }
    CHECK(model_pad_configured(u->out_port, u->out_pin, NULL) == 14, "%s: output pad", u->name);
  }
  for (unsigned pass = 0; pass < 2; pass++)
  {
    for (unsigned i = 0; i < UNIT_COUNT; i++) { exhaust(&UNITS[i]); }
  }
  no_violation("compose run");
}

static void twice(void)
{
  reset();
  CHECK(UNITS[0].init() == HAL_OK, "first init");
  CHECK(UNITS[0].init() == HAL_ERROR, "second init must fail");
  no_violation("twice");
}

static void fault_case(const char *fault)
{
  reset();
  model_fault(fault, 1);
  CHECK(UNITS[0].init() != HAL_OK, "init must fail under fault %s", fault);
  CHECK(model_pad_configured(UNITS[0].out_port, UNITS[0].out_pin, NULL) != 14, "output pad connected despite fault %s", fault);
  if (UNIT_COUNT > 1U) { CHECK(UNITS[1].init() != HAL_OK, "a later unit must fail too under fault %s", fault); }
  if (UNITS[0].set != NULL) { CHECK(UNITS[0].set(1U) == HAL_ERROR, "set_trigger after a failed init"); }
  no_violation(fault);
}

static void triggers(void)
{
  for (unsigned i = 0; i < UNIT_COUNT; i++)
  {
    const unit_t *u = &UNITS[i];
    if (u->set == NULL) { continue; }
    reset();
    uint32_t level;
    CHECK(u->set(1U) == HAL_ERROR, "%s: set_trigger before init", u->name);
    CHECK(u->get(&level) == HAL_ERROR, "%s: get_trigger before init", u->name);
    init_all();
    CHECK(u->set(2U) == HAL_ERROR, "%s: level 2 accepted", u->name);
    CHECK(u->get(NULL) == HAL_ERROR, "%s: NULL accepted", u->name);
    model_fault("write_busy", 1);
    CHECK(u->set(1U) == HAL_BUSY, "%s: busy write not reported", u->name);
    model_fault("write_busy", 0);
    CHECK(u->set(1U) == HAL_OK, "%s: write after busy", u->name);
    no_violation("triggers");
    return; /* the units' own state is now initialized; one software unit per process */
  }
}

/* A unit initialized later stops and restarts PLAY1; what the earlier units configured stays. */
static void restart(void)
{
  if (UNIT_COUNT < 2U) { return; }
  reset();
  CHECK(UNITS[0].init() == HAL_OK, "first unit");
  exhaust(&UNITS[0]);
  for (unsigned i = 1; i < UNIT_COUNT; i++) { CHECK(UNITS[i].init() == HAL_OK, "%s: init", UNITS[i].name); }
  exhaust(&UNITS[0]);
  no_violation("restart");
}

/* One scenario per process: a unit's own state is static and survives only within one image run. */
int main(int argc, char **argv)
{
  const char *name = argc > 1 ? argv[1] : "";
  if (strcmp(name, "compose") == 0) { compose(); }
  else if (strcmp(name, "twice") == 0) { twice(); }
  else if (strcmp(name, "lsi") == 0 || strcmp(name, "truth_write") == 0 || strcmp(name, "kernel_clock") == 0) { fault_case(name); }
  else if (strcmp(name, "triggers") == 0) { triggers(); }
  else if (strcmp(name, "restart") == 0) { restart(); }
  else { printf("unknown scenario %s\n", name); return 2; }
  printf("%s %s: %u units, %u failures\n", SCENARIO, name, (unsigned)UNIT_COUNT, failures);
  return failures == 0U ? 0 : 1;
}
