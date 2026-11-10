# Firmware knowledge package sources

This directory contains BKPT-authored inputs for `bkpt-fw`, which is built from
BKPT-Toolkit/crates/bkpt-firmware. It is separate from the legacy scaffold index
at the repository root. `knowledge/index.json` describes this source format;
it is not yet consumed by an automatic package installer.

The [STM32C5 package](stm32c5/knowledge-pack.json) is a development package for
the exact part, board, package and SDK declared in that manifest. It does not
claim full STM32C5-family coverage.

## Source and installation boundaries

- `architecture/accepted.json` and its review retain the original reviewed bytes.
  They describe 14 scoped capability profiles, not 14 fully supported peripherals.
- `catalog/` contains BKPT-authored knowledge/import declarations, component code
  and templates. `imports/` pins external manufacturer descriptors and driver packs.
- `sources/` preserves the BKPT-authored snapshots referenced by the accepted graph.
  `source-inputs.json` maps these files and all external references to exact hashes.
  Some BKPT component templates incorporate manufacturer-derived portions under
  BSD-3-Clause; their original notices are retained. The `owned` inventory means
  BKPT component deliverables, not exclusive copyright over every incorporated
  line. Per-file attribution identifies these mixed-provenance templates.
- `tests/` contains host adapter checks. These do not establish hardware support.
- `candidates/workflow-efficiency/` preserves the newer reviewed evaluation graph,
  including source-backed timebase metadata and optional-peripheral integration.
  It is separate from the operator-default graph and the unfinished research.
  Its manifest does not activate it. Foundation manifests are preserved as BKPT
  assembly declarations; the vendor/generated files they reference stay external.
- `pending/` saves paused, incomplete research as excerpt-free claim projections.
  It is not read by the runtime. Each projection identifies the original hash and
  removed fields; none acquires acceptance by being committed here. Resume reviews
  against the original source-bound workspace, not these transformed files.
  `pending/tools/` preserves eight original Python authoring helpers. Copy them
  into that external research workspace before use: they expect the original
  evidence layout and may write vendor extracts beside themselves. Do not run
  them inside this package checkout. They are development tools, not a Python
  dependency of `bkpt-fw`, BKPT-AI or the Claude plugin.
  The account-specific Claude review runner remains in the external workspace.

Vendor SDKs, PDFs, headers, raw extracted documents and foundation files containing
vendor-generated code are external. They are not distributed in this package.
SQLite files are generated installed artifacts and are ignored by Git.

Build catalogs with `bkpt-fw architecture build`, `devices build`,
`components build` and `ingest`, using the manifest inputs and explicitly supplied
source roots. The accepted architecture requires its matching review. Resolve
source-input paths beneath an external source root; never rewrite an accepted
bundle in place. Rebuild derived catalogs when the firmware engine identity changes.
The knowledge input uses its own original source-root layout and must also resolve
the pinned documents, descriptors and example sources locally.

An operator configuration maps stable catalog IDs to installed SQLite paths and
sets foundation paths and SDK asset roots. The executable, packages and installed
data need not be adjacent. `bkpt-fw routes` and `target-context` expose the installed
selection. The package manifest lists the intended artifact names; it does not
install dependencies or grant reviewed coverage to extracted records.

Paused originals remain in the external `coverage/c5-reference` research
workspace, tracked in BKPAI-14. Keep those originals until the pending corrections
and reviews are complete. Local source URIs in immutable manifests record the
original provenance; installation uses explicit source roots.
