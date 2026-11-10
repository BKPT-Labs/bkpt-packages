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

## Licensing and third-party content

Files written by BKPT carry the SPDX identifier `LicenseRef-BKPT-Proprietary`.
Some component templates incorporate manufacturer-derived portions under
BSD-3-Clause; their original notices are retained and attribution is given per
file. Where a manifest calls files "owned", it means they are BKPT component
deliverables, not that BKPT holds exclusive copyright in every incorporated line.
