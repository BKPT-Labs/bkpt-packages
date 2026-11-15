# BKPT firmware knowledge packages

A knowledge package describes a microcontroller target as connected resources
instead of prose: which peripherals, pins, DMA requests and channels, interrupts
and clocks exist, how they route to each other, which combinations conflict, and
which reviewed software components implement a capability on top of them. Every
statement points to the manufacturer source it came from.

BKPT's firmware CLI, `bkpt-fw`, reads these packages to answer questions such as
"which resources does SPI1 with DMA need on this board, and are they still free",
to allocate resources for a set of requirements, and to assemble a buildable
project from reviewed components. The CLI is distributed separately as part of
BKPT's tools. This directory holds the package sources only.

`index.json` lists the packages in this directory. It is separate from the
package index at the repository root, which serves the older project scaffolds
and trace components and is unchanged.

## Available packages

| Package | Target | State |
| --- | --- | --- |
| [`stm32c5`](stm32c5/knowledge-pack.json) | STM32C542RCT6, LQFP64, NUCLEO-C542RC, STM32 HAL2 2.1.0 | Development |

The STM32C5 package covers the exact part, package, board and SDK version named
in its manifest. It contains 23 scoped capability profiles (for example a managed
SPI DMA transfer, a PWM output or a periodic timer interrupt). That is not 23
fully supported peripherals and not coverage of the whole STM32C5 family.

## What a package contains

Using `stm32c5/` as the example:

| Path | Contents |
| --- | --- |
| `knowledge-pack.json` | The package manifest: target identity, input files with their hashes, and declared coverage |
| `graph.json` | The capability graph of the chip: one file, updated in place. An older graph is in the repository's history, not in the tree |
| `graph.review.json` | The review record that binds the graph's exact bytes |
| `foundation-manifest.json` | The manifest of the project foundation the graph builds on |
| `catalog/` | BKPT-authored knowledge and import declarations, component source and templates |
| `imports/` | Declarations that pin the manufacturer device descriptors and driver packs the package was derived from |
| `sources/` | BKPT-authored source snapshots that the graph cites |
| `source-inputs.json` | Every file the graph pins, BKPT-authored or external, with its exact hash |
| `tests/` | Host-side checks of the component adapters, and a host model of the input capture unit |
| `pending/` | Paused, incomplete research. See below |

## How much to trust what you read

A package keeps four things separate, and none of them implies the next:

1. **A source exists.** The manufacturer document or file is identified by
   version and hash.
2. **A claim was reviewed.** A reviewer compared the claim with that source. The
   review record binds the exact bytes that were reviewed. It is an attribution,
   not a cryptographic identity and not a certification of the whole device.
3. **A component implements it.** Reviewed component code exists for a capability.
4. **It was verified on hardware.** Nothing in these packages claims this.

Anything not stated is unknown. A capability missing from a package means it has
no reviewed coverage here, not that the silicon lacks it. Names of symbols,
registers or pins are never treated as proof of behavior.

Files under `pending/` are research that was paused before review finished. They
are kept as projections with the source excerpts removed, each recording
the hash of its original. They are not reviewed claims and the tooling does not
load them. The helper scripts in `pending/tools/` were written for the original
research workspace and expect its layout; they are not needed to use a package
and are not meant to be run from this checkout.

## What is not included

Manufacturer SDKs, PDFs, headers, raw document extracts and project foundation
files that contain vendor-generated code are not distributed here. The package
refers to them by exact version and hash, and you obtain them from the
manufacturer. Generated databases are not committed either: they are built
locally from these sources.

## Using a package

`bkpt-fw` builds its local catalogs from a package's inputs together with your
own copies of the manufacturer sources, and checks every hash while doing so. A
configuration file on your machine then maps catalog names to the files it built
and to the locations of the SDK. The package manifest names the catalogs it is
meant to produce. It does not install anything by itself, and there is no
automatic package installer yet.

The graph changes in place and the branch `main` holds the latest one. A review
record binds exact bytes, so a changed graph needs a new review before it is
merged, and the record beside it is replaced with it. A source the graph cites
is never edited: a changed text is a new file under a new name.

## Generating a whole-device graph

`tools/` holds the generators that widen a reviewed graph to a whole device. They
read only pinned sources and a configuration, and every record they write cites
the lines it came from.

| Tool | What it does |
| --- | --- |
| `author_device_inventory.py` | Transcribes pins, signal routes and interrupts from the manufacturer's pinout descriptor, device header and vector table. An inventory records what is declared, not what is usable |
| `author_device_domains.py` | Decides what a capability may use: allocation rules from the board file, instances checked against the device description, DMA request lines checked against the reference manual and the SDK, and the capabilities that are widened or added. It has no command line: `compose_target.py` runs it for the board a recipe names |
| `compose_target.py` | Builds one target from layers that vary independently: the reviewed graph, the part and its package descriptor, and the board as its own file (from a netlist, or declared by its owner). Facts tied to the old part, package or board are relocated, replaced or dropped by a decision the recipe states |
| `target_support.py` | Gives a composed target what it needs besides its graph: a project foundation derived from the reviewed base under the identity of its own part and board, and an entry for the board in the knowledge corpus. Both come from the recipe; the foundation's files are the base's, checked by hash while they are copied |
| `descriptor.py` | Reads a manufacturer JSON descriptor while keeping the lines of every value, so a derived record can cite them |
| `package_graph.py` | Writes a generated graph into a package as its `graph.json`, with the list of every source it pins, and updates the manifest. BKPT-authored sources are copied in; manufacturer sources are listed by hash only |
| `test_author_device.py`, `test_compose_target.py`, `test_package_graph.py` | Checks for the tools on small synthetic sources |

A capability covers a peripheral class, not a list of instance names. A class is a
peripheral type and IP version as the manufacturer's peripherals descriptor names
them. The descriptor also gives each instance its bus, interrupt, DMA requests and
the role of each signal, and the limitations descriptor says what a given part
lacks. A second part or package of the same die is therefore a recipe under
`stm32c5/targets/` and, when the board differs, a board file under
`stm32c5/boards/`, with no new chip configuration.

The family configuration states nothing about a board. Every target is a
recipe that names a board file, the manufacturer's own board included:
`stm32c5/targets/nucleo-c542rc.json` names `stm32c5/boards/nucleo-c542rc.json`.
A board file holds the board's netlist or its owner's description, the nets or
pads it reserves and excludes, the pads it names for one use (a button, a
console), and the lines of the policy text that are rules about this board.
A family configuration that carries any of these is refused, as is one that
labels pads with a `board_` attribute or rests a chip field on a board's rule
or source, so what a graph says about a pad can always be traced to either a
chip source or a board file.

    python tools/compose_target.py --recipe stm32c5/targets/nucleo-c542rc.json \
        --source-root ROOT --out NEW.json

With `BKPT_KNOWLEDGE_SOURCE_ROOT` set to the folder that holds the pinned
sources, `test_compose_target.py` also composes every checked-in recipe and
compares the result with the hash recorded in the test. A change that alters
a graph therefore has to change that hash in the same commit.

A target that shares the base's start-up files gets its foundation from a
derivation record under `stm32c5/foundations/`. The record states the new
identity and pins two hashes: the base manifest's and the derived manifest's.
`compose_target.py` computes the derived manifest from the base the reviewed
graph names, so a foundation with other bytes cannot stand in for it. The
record also lists what the board's owner must declare for that start-up to
work, such as the clock source and its frequency; a description that does not
say so is refused. A board description names its board, exact part and package.
The foundation files themselves contain manufacturer code and stay outside the
package: `target_support.py` installs them from your copy of the base. Give it
`--recipe` once per target to put several targets into one foundation index
and one corpus. `stm32c5/targets/` holds three: the NUCLEO-C542RC, and the
STM32C542CCT6 in LQFP48 and the STM32C542KCT6 in LQFP32 on owner-declared
boards. The 32-pin package bonds two pads to one pin, which the firmware
tool's corpus takes from the build that keys a pin by position and pad.

An allocation in `device-domains.json` can carry a generated component: its
parameters, the bindings that fill a template from the allocated instance, pad
and route, and the name of a reviewed card whose integration edits it shares
instead of writing them again. `pwm_output` is the first one: a PWM output on
any covered channel timer and routed pad, with exact frequency choices from a
rate table and duty set at run time. On the NUCLEO-C542RC it was checked on
the board for TIM2 and TIM1 at 1 kHz and TIM2 at 100 kHz by firmware that
measures its own output pad: the frequency agreed within a quarter of a
percent and the duty within twelve thousandths, with every lifecycle call
returning what the interface states.

A capability does not have to be an instance with one routed pad. The family
configuration can also state:

- a capability on a whole instance without a pad, with the one interrupt the
  instance raises when it needs it: a periodic interrupt on a channel timer, a
  CRC-32 unit, the independent watchdog. A class used this way exists on every
  package of the die, so the peripherals descriptor's declaration is its
  record;
- a capability on one eligible pad that generates nothing, such as a GPIO
  input, whose card says how the application uses the pad through the SDK;
- a reviewed component taken whole for another class of instance, which is how
  the periodic interrupt of TIM6 and TIM7 serves the channel timers;
- an integration edit no reviewed card has written yet, for example switching
  on an SDK module nothing used before. It is pinned to the same foundation
  file as the reviewed edits of that path and checked against it;
- a pad structure a profile requires, read from the pinout descriptor together
  with the descriptor's own description of the option. The I2C profiles take
  SCL and SDA on any allowed routed pad the descriptor declares Fm+ capable;
- the clock descriptor element that feeds each covered instance, cited as
  what can be configured, never as what is configured.

Several units may share one instance when its ownership allows it: every unit
of `analog_reading` on a converter uses one handle. The engine generates a
file per unit, so the shared handle is a weak definition each unit carries
and the linker keeps once.

A family configuration may ask for a later graph schema with `graph_schema`.
The bounds the firmware tool puts on a graph are mirrored in one table per
schema in `author_device_inventory.py` and set for one run; the STM32C5 family
asks for schema 5, because its graph pins more than 128 sources.

In schema 5 a role can state the domain an earlier revision gave it. A profile
that widens roles of an earlier graph carries `earlier_through` with that
pack revision. The generator then lists on each widened role, as `earlier`,
the resources the earlier graph searched for it, in that graph's search
order. The firmware tool searches that domain before it admits a resource
added since, so a request the earlier graph could plan keeps its plan. A
role that gained nothing lists nothing.

Every source of a schema 5 graph carries a `scope`: `device`, `board`, `sdk`,
`policy` or `implementation`. The family configuration states it per source
and a board file states it for its own sources. Nothing is inferred, and a
source without one is refused. `source_authorities`, `source_revisions` and
`source_labels` in the configuration give the sources one vocabulary for who
stands behind a file and for how a citation names it, so that a captured
configuration file is not called SDK source.

A component card cites the definition of every SDK function its unit calls,
not only the prototype. A symbol entry marked `definition` pins the function
body in the SDK's C file, and the generator refuses a unit that calls a
`HAL_` function whose definition its card does not cite. A statement about
what a call does or how long it can take cites the lines of the body that
say so.

That rule is checked once more when the graph is finished, on every card
with a generated unit, whether the unit was written for the card or taken
whole from another one: a citation of the card must contain the whole
definition of each `HAL_` function the templates call by name, and a `HAL_`
function they call through a binding must be cited so by the resource that
supplies the name, or by the card. `definition_rule.exempt` in the
family configuration lists the cards that were reviewed before the rule and
stay outside it until they are changed; the policy rule has to name each.

Capacity counts the holders of a shared instance; it cannot keep two ways of
holding it apart. An allocation that shares its instance may therefore name
an `instance_mode`, which becomes the `mode` of its role in schema 5: the
firmware tool lets holders that name modes share a resource only when the
modes are equal. The policy rule on ownership modes has to name the mode, and
an instance kind that one capability holds with a mode and another without is
refused, because a role that names none shares with everyone.

A profile that widens a capability an earlier graph already held can
say `reviewed_routes`. Its links then keep the relation they were reviewed
with, so a plan names the same routes as before. A pad admitted since gets a
route of that relation, named as the earlier ones are, and every such route
states the alternate function of the descriptor route between the same two
ends. Without it a profile selects the descriptor routes themselves.

The chip-specific inputs are `stm32c5/imports/device-inventory.json` and
`device-domains.json`, together with the policy text the domains configuration
cites line by line. A rule that is BKPT's own decision, such as which pads may be
allocated, lives in that policy text and is cited as policy, never as a
manufacturer statement.

`stm32c5/graph.json` is the graph for the STM32C542 on the NUCLEO-C542RC:
23 capability profiles at pack revision 3, written in graph schema 5 under
policy version 9. `graph.review.json` binds its bytes to two independent
reviews, and `inputs.architecture` in the manifest names it with its hash.
The graphs of revision 1 (14 profiles) and revision 2 (17 profiles) are in
the repository's history. The pack revision is a plain count: a role's
`earlier` domain names the last revision that gave it that domain.

On top of the revision 2 graph it adds:

- I2C1 SCL and SDA on every allowed routed pad the pinout descriptor declares
  Fm+ capable. The four pads of the earlier graph are the roles' `earlier`
  domain, so a request that graph planned keeps its pads.
- `analog_reading`: one unit per input on the shared converter, with
  calibration, a 12-bit count as a fraction of VREF+, and a check on every
  read that the converter is still enabled and set to the unit's input.
  `analog_input_pin` (the application configures the converter) and
  `analog_reading` name different ownership modes on the converter, so a
  request for both on one converter is refused in either order.
- `input_capture`: period and high time in microseconds on a channel timer.
  One look at the timer is one step with interrupts masked; a mark set behind
  its acknowledgement ends the measurement with an error. The header states
  what a result means and the conditions it rests on.
- `periodic_interrupt_channel_timer` on TIM1, TIM2, TIM8, TIM12 and TIM15,
  `crc32_checksum`, `independent_watchdog` and `gpio_input`.
- A declared scope on every source (device, board, sdk, policy,
  implementation), so a fact about the board can be told from a fact about
  the chip.
- Citations of SDK function definitions for every precondition a new unit
  relies on. Twelve cards reviewed before that rule are named as exempt.

How it was checked:

- Two independent reviews rejected the first version of these additions and one
  rejected a later one; their findings were a start-up order the SDK refuses
  for the converter's kernel clock, an input capture that could return a
  wrong value after a delayed poll, a default allocation that changed in a
  crowded request, and statements a unit could not keep. Both passed these
  bytes.
- A fixed suite of 2,222 requests was solved on the revision 2 graph and on
  this one with one firmware tool build: all 1,682 requests the earlier graph
  plans keep their resources, route identifiers and generated files. The
  suite and its tool are kept with the evaluation data, and a replay is a
  step of every later revision.
- `stm32c5/tests/input_capture_model.py` runs the input capture unit's own
  template against a simulated timer that delays the caller at every register
  access: 43,772 cases without a violation, and the previous version of the
  unit fails there with the values the reviews reported.
- On the NUCLEO-C542RC four self-check applications passed: analog reading on
  PA0 and PA1, input capture on PA0 with TIM2 including held polls from half
  a turn to two turns of the counter, periodic interrupts on TIM1, TIM2 and
  TIM6, the CRC-32 unit against zlib and a watchdog reset. The checks
  stimulate the pads from inside the chip and are not a measurement of
  accuracy. A failed watchdog start and a delay between two chosen statements
  cannot be provoked on a board; those rest on the SDK source and the model.

The STM32C542CCT6 (LQFP48) and STM32C542KCT6 (LQFP32) recipes compose from the
same family data, but their graphs were not reviewed, built or run.

## Licensing and third-party content

Files written by BKPT carry the SPDX identifier `LicenseRef-BKPT-Proprietary`.
Some component templates incorporate manufacturer-derived portions under
BSD-3-Clause; their original notices are retained and attribution is given per
file. Where a manifest calls files "owned", it means they are BKPT component
deliverables, not that BKPT holds exclusive copyright in every incorporated line.
