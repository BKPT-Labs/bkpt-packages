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
in its manifest. It contains 17 scoped capability profiles (for example a managed
SPI DMA transfer, a PWM output or a periodic timer interrupt). That is not 17
fully supported peripherals and not coverage of the whole STM32C5 family.

## What a package contains

Using `stm32c5/` as the example:

| Path | Contents |
| --- | --- |
| `knowledge-pack.json` | The package manifest: target identity, input files with their hashes, and declared coverage |
| `architecture/` | The first reviewed capability graph (`accepted.json`), its review record and foundation manifest. The manifest's `inputs.architecture` names the graph that is active now |
| `catalog/` | BKPT-authored knowledge and import declarations, component source and templates |
| `imports/` | Declarations that pin the manufacturer device descriptors and driver packs the package was derived from |
| `sources/` | BKPT-authored source snapshots that the reviewed graph cites |
| `source-inputs.json` | Every file the package refers to, BKPT-authored or external, with its exact hash |
| `tests/` | Host-side checks of the component adapters, and a host model of the input capture unit |
| `candidates/` | Newer reviewed graphs, each in its own folder. One becomes the default only when the manifest's `inputs.architecture` names it |
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
are published as projections with the source excerpts removed, each recording
the hash of its original. They are not accepted claims and the tooling does not
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

Reviewed files and their review records are immutable: changing a reviewed graph
means publishing a new one with a new review, never editing in place.

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
| `package_candidate.py` | Places a generated graph in a package as a candidate, with the list of every source it pins. BKPT-authored sources are copied in; manufacturer sources are listed by hash only |
| `test_author_device.py` | Checks for the tools on small synthetic sources |

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
a published graph therefore has to change that hash in the same commit.

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
tool's corpus accepts from the build that keys a pin by position and pad.

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
that widens roles of an accepted graph carries `earlier_through` with that
pack revision. The generator then lists on each widened role, as `earlier`,
the resources the accepted graph searched for it, in that graph's search
order. The firmware tool searches that domain before it admits a resource
added since, so a request the accepted graph could plan keeps its plan. A
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
supplies the name, or by the card. `definition_rule.accepted_before` in the
family configuration lists the cards that were accepted before the rule and
stay outside it until they are changed; the policy rule has to name each.

Capacity counts the holders of a shared instance; it cannot keep two ways of
holding it apart. An allocation that shares its instance may therefore name
an `instance_mode`, which becomes the `mode` of its role in schema 5: the
firmware tool lets holders that name modes share a resource only when the
modes are equal. The policy rule on ownership modes has to name the mode, and
an instance kind that one capability holds with a mode and another without is
refused, because a role that names none shares with everyone.

A profile that widens a capability the accepted graph already published can
say `reviewed_routes`. Its links then keep the relation they were accepted
with, so a plan names the same routes as before. A pad admitted since gets a
route of that relation, named as the accepted ones are, and every such route
states the alternate function of the descriptor route between the same two
ends. Without it a profile selects the descriptor routes themselves.

The chip-specific inputs are `stm32c5/imports/device-inventory.json` and
`device-domains.json`, together with the policy text the domains configuration
cites line by line. A rule that is BKPT's own decision, such as which pads may be
allocated, lives in that policy text and is cited as policy, never as a
manufacturer statement.

`stm32c5/candidates/device-coverage-r2/` is the output for the STM32C542 on the
NUCLEO-C542RC: 17 capability profiles on top of the reviewed workflow-efficiency
graph. It is accepted: `accepted.json` holds the reviewed bytes and
`accepted.review.json` binds them to an independent data review and an
independent regression review. Since pack revision 2 it is the pack's active
architecture input: `inputs.architecture` in the manifest names it, and the
graph of revision 1 stays at `architecture/accepted.json`, listed under
`previous`.

`stm32c5/candidates/device-coverage-r3/` is the next graph for the same target:
23 capability profiles. It adds I2C on every allowed Fm+ pad, analog reading,
input capture, the periodic interrupt on the channel timers, a CRC-32 unit,
the independent watchdog and GPIO input, under policy version 6, and applies
the review items of the accepted graph that were generator or data matters.
It is a candidate, not accepted: it has no review record, nothing in it was
run on a board, and the manifest's `inputs.architecture` does not name it.
It is written in graph schema 5, which needs a firmware tool build that reads
that schema.

`stm32c5/candidates/device-coverage-r4/` replaces it before any acceptance. The
first run of round 3 on the NUCLEO-C542RC showed that the analog reading unit
could not start: it enabled the converter's bus clock and then asked the SDK
for the ADC and DAC kernel clock, which the SDK refuses while that bus clock is
on. Round 4 cites version 2 of the unit, which sets the kernel clock first and
reads the configuration back, and the SDK lines that state the order. Nothing
else differs from round 3: six records, all of the analog reading component.
On that board the four self-check applications of the round then passed:
analog reading on PA0 and PA1, input capture on PA0 with TIM2, periodic
interrupts on TIM1, TIM2 and TIM6, the CRC-32 unit against zlib and a watchdog
reset. The checks stimulate the pads from inside the chip and are not a
measurement of accuracy. Round 4 is a candidate too: it has no review record.

`stm32c5/candidates/device-coverage-r5/` answers the two independent reviews
of round 3, which both rejected it. Rounds 3 and 4 stay as packaged. It is
written under policy version 7 and changes this:

- SCL and SDA of the two I2C capabilities list the four pads the accepted
  graph gave them as their earlier domain. A fixed suite of 555 requests was
  solved on the accepted graph and on round 5 with one firmware tool build:
  all 405 requests the accepted graph plans get the same resource for every
  role.
- Input capture, version 2 of the unit, counts counter wraps with a second
  compare mark at half a turn, so a caller that polls late gets the right
  duration or an error and never a wrong one, and it keeps its time limit on
  the timer's own count.
- Analog reading, version 3, checks on every read that the converter is
  still enabled and still set to the unit's input. Its header states the
  bounds the SDK's own timeouts give and that they need the HAL tick.
  `analog_input_pin` and `analog_reading` must not be requested for one
  converter: the policy and both cards say so, and the unit reports a foreign
  stop as an error. The solver does not refuse the combination.
- The independent watchdog, version 2, reports a start whose outcome is
  unknown as a state of its own, in which refresh works and no timeout is
  promised.
- The SDK implementation files of the converter, the CRC unit and the
  watchdog are pinned, and every precondition cites the function body.
  Every source states its scope and authority.

On the NUCLEO-C542RC the four self-check applications passed on the round 5
builds. Input capture returned 119000 and 59000 counts for every one of eight
measurements polled 20 ms late, an error when polled 70 ms late, and kept a
1 ms limit within 1 ms. Analog reading corrected a foreign channel selection
and reported a foreign stop. The checks stimulate the pads from inside the
chip; the failed start of the watchdog cannot be provoked there.

Round 5 needs a firmware tool build that reads the `earlier` field. It is a
candidate: it has no review record and the manifest does not name it.

`stm32c5/candidates/device-coverage-r6/` replaces round 5 before any
acceptance, under policy version 8. Two things differ, 21 records in all:

- `analog_input_pin` and `analog_reading` name different ownership modes on
  the converter. A request that takes both on one converter is refused in
  either order, and the refusal names the converter and the two modes under
  `ownership_conflicts`. Several reading units still share the converter, and
  allocations alone are planned as on the accepted graph. The unit keeps its
  own check of the converter, which is what stands against code outside the
  graph.
- The I2C capabilities select their routes by `scl_on` and `sda_on` again.
  Since round 3 they had selected the descriptor routes, so every plan with
  I2C named other routes than the accepted graph did, for the same pads. The
  four accepted routes keep their identifiers and gain the alternate function
  of their descriptor route; the five pads admitted since get routes named
  the same way.

A fixed suite of 662 requests was solved on the accepted graph and on round 6
with one firmware tool build: all 476 requests the accepted graph plans get
the same resource for every role, the same route identifiers and the same
generated files; 12 of 12 combinations of the two analog capabilities are
refused. No component template changed, and the firmware built from round 6
for the four self-check applications is byte for byte the firmware that
passed on the NUCLEO-C542RC in round 5, so those runs stand and none was
repeated. Round 6 needs a firmware tool build that reads `earlier` and
`mode`. It is a candidate: it has no review record.

`stm32c5/candidates/device-coverage-r7/` answers the two follow-up reviews
of round 6, under policy version 9. Rounds 3 to 6 stay as packaged. 38
records differ from round 6:

- Input capture, version 3. Version 2 read the timer's status and cleared
  the marks it had seen in two statements; a thread held for a turn of the
  counter or more between them lost a turn, and the unit returned a wrong
  period and high time as a success. Version 3 takes one look at the timer
  as one step with interrupts masked, ends the measurement when a mark is
  set behind its acknowledgement, and places captures and counts around a
  wrap by their value. Header, card and policy state what a result means,
  the conditions it rests on and when a measurement completes.
  `stm32c5/tests/input_capture_model.py` runs the unit's own template
  against a simulated timer that delays the caller at every register
  access: version 3 holds its property in 43,772 cases, and version 2 fails
  there with the values the reviews reported.
- Analog reading, header version 4, text only: the limit the unit gives the
  SDK, the tick the SDK's waits count, and the cleanup after a time-out are
  stated apart from time for the caller.
- Independent watchdog, version 3: a refresh is refused while the SDK's
  start call runs, because the reference manual lets any key but the access
  key write-protect the watchdog's registers again.
- The definition rule is checked on every finished card. The channel timer
  card, which takes the basic timer's unit whole, cites seven definitions
  it lacked. Twelve cards of the accepted graph are named as accepted
  before the rule.

The replay against the accepted graph, now 2,222 requests with those of the
round 6 review: all 1,682 requests the accepted graph plans keep their
resources, route identifiers and generated files. On the NUCLEO-C542RC the
capture, system and analog self-check applications passed on the round 7
builds; the capture application also held its thread between half a turn
and two turns of the counter and got the true value or an error each time.
The periodic interrupt application is byte for byte the firmware that passed
in round 5. A delay between two chosen statements cannot be placed on a
board; that part rests on the model. Round 7 is a candidate: it has no
review record.

## Licensing and third-party content

Files written by BKPT carry the SPDX identifier `LicenseRef-BKPT-Proprietary`.
Some component templates incorporate manufacturer-derived portions under
BSD-3-Clause; their original notices are retained and attribution is given per
file. Where a manifest calls files "owned", it means they are BKPT component
deliverables, not that BKPT holds exclusive copyright in every incorporated line.
