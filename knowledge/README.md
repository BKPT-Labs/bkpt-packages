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
in its manifest. It contains 14 scoped capability profiles (for example a managed
SPI DMA transfer or a periodic timer interrupt). That is not 14 fully supported
peripherals and not coverage of the whole STM32C5 family.

## What a package contains

Using `stm32c5/` as the example:

| Path | Contents |
| --- | --- |
| `knowledge-pack.json` | The package manifest: target identity, input files with their hashes, and declared coverage |
| `architecture/` | The reviewed capability graph (`accepted.json`), its review record, and the manifest of the project foundation it builds on |
| `catalog/` | BKPT-authored knowledge and import declarations, component source and templates |
| `imports/` | Declarations that pin the manufacturer device descriptors and driver packs the package was derived from |
| `sources/` | BKPT-authored source snapshots that the reviewed graph cites |
| `source-inputs.json` | Every file the package refers to, BKPT-authored or external, with its exact hash |
| `tests/` | Host-side checks of the component adapters |
| `candidates/` | A newer reviewed graph kept apart from the default one. Listing it in the manifest does not activate it |
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

The chip-specific inputs are `stm32c5/imports/device-inventory.json` and
`device-domains.json`, together with the policy text the domains configuration
cites line by line. A rule that is BKPT's own decision, such as which pads may be
allocated, lives in that policy text and is cited as policy, never as a
manufacturer statement.

`stm32c5/candidates/device-coverage/` is the output for the STM32C542 on the
NUCLEO-C542RC: 16 capability profiles on top of the reviewed workflow-efficiency
graph. It is a candidate. It has no review record and is not accepted.

## Licensing and third-party content

Files written by BKPT carry the SPDX identifier `LicenseRef-BKPT-Proprietary`.
Some component templates incorporate manufacturer-derived portions under
BSD-3-Clause; their original notices are retained and attribution is given per
file. Where a manifest calls files "owned", it means they are BKPT component
deliverables, not that BKPT holds exclusive copyright in every incorporated line.
