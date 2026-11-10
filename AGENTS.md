# Package source rules

Follow the parent workspace AGENTS.md. Keep the existing scaffold/component
index compatible with its current consumers. New firmware knowledge packs use
`knowledge/index.json`; they are not legacy scaffold packages.

Only BKPT-authored deliverables belong in new knowledge packages: package
definitions, source-backed claims/graphs, import maps, owned component code and
their provenance references. Do not add vendor PDFs, SDKs, headers, extracted
source/page dumps, generated SQLite databases, review transcripts or benchmark
outputs. Those are external inputs or installed artifacts, referenced by exact
hash and version. Preserve third-party notices already present in existing packages.

Reviewed input bytes and their attestations are immutable. Never normalize line
endings or rewrite source paths inside an accepted bundle without new review
bindings. Source presence, bounded review, implementation and hardware coverage
remain separate states. Pending C5 claims may be committed as explicitly
unqualified, excerpt-free projections. Their original evidence and review working
files stay external; projections must never inherit acceptance of original bytes.

This repository is public. Publish migration/research changes only within the
user-authorized scope, on a feature branch and draft PR. Do not merge them
into main as part of a Toolkit code PR. Root legacy index.json is unchanged by
the firmware knowledge migration.
