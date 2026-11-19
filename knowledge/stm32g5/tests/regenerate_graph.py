# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Compatibility CLI for the G5 Nucleo recipe. Prepare its source root first."""
import argparse
import json
import os
from pathlib import Path
import sys

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE.parent / "tools"))
import author_device_inventory as inventory
import compose_target


def generate(root):
    return compose_target.compose(PACKAGE / "targets/nucleo-g564re.json", root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT"))
    parser.add_argument("--stage-root", type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--out", type=Path)
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.source_root is None:
        parser.error("give --source-root or set BKPT_KNOWLEDGE_SOURCE_ROOT")
    graph, summary = generate(args.source_root)
    if args.check and inventory.encoded(graph) != (PACKAGE / "graph.json").read_bytes():
        raise SystemExit("graph.json differs from the Nucleo target recipe")
    if args.stage_root:
        if args.stage_root.exists() and any(args.stage_root.iterdir()):
            raise SystemExit("stage the sources in an empty directory")
        for source in graph.value["sources"]:
            relative = Path(source["path"])
            if relative.is_absolute() or relative.drive or ".." in relative.parts:
                raise SystemExit("expected a relative graph source path")
            data = (args.source_root / relative).read_bytes()
            if inventory.sha256(data) != source["sha256"]:
                raise SystemExit(f"source hash mismatch: {relative}")
            dest = args.stage_root / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            with dest.open("xb") as handle:
                handle.write(data)
    if args.out:
        inventory.write(graph, args.out, summary)
    else:
        print(json.dumps({"graph": "unchanged", "sha256": inventory.sha256(inventory.encoded(graph))}))


if __name__ == "__main__":
    main()
