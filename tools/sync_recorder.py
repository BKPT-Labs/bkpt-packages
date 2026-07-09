#!/usr/bin/env python3
"""Vendor the ViewAlyzer firmware recorder into the viewalyzer-recorder package.

Copies the embedded recorder trees (core/, freertos/, zephyr/ — never the
host-side c/ or python/ dirs) from the ViewAlyzer source repo into
`viewalyzer-recorder/recorder/`, stamps provenance (source git rev, recorder
version macro, wire version, content hash) into the package manifest, and
updates the registry entry in index.json.

Usage:
    python3 tools/sync_recorder.py [--source <ViewAlyzer repo>] [--rev N]

Without --rev the registry rev is bumped by 1 only when the content hash
changed (re-running on identical sources is a no-op).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PKG_DIR = REPO_ROOT / "viewalyzer-recorder"
RECORDER_DEST = PKG_DIR / "recorder"
DEFAULT_SOURCE = REPO_ROOT.parent.parent / "ViewAlyzer_Root" / "ViewAlyzer"

# Embedded-target trees only. c/ (host UDP lib) and python/ (host decoder)
# never ship to firmware projects.
EMBEDDED_TREES = ("core", "freertos", "zephyr")

# The wire version the vendored recorder emits by default (VA_TIMESTAMP_BITS=32
# -> sync marker v2). Bump alongside any recorder default change.
WIRE_VERSION = 2


def _git_rev(repo: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", str(repo), "status", "--porcelain",
             "ViewAlyzerRecorder"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        return f"{out}-dirty" if dirty else out
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def _recorder_version(core_header: Path) -> str:
    m = re.search(
        r'#define\s+VA_RECORDER_VERSION\s+"([^"]+)"', core_header.read_text()
    )
    return m.group(1) if m else "unknown"


def _content_hash(root: Path, rel_files: list[str]) -> str:
    h = hashlib.sha256()
    for rel in sorted(rel_files):
        h.update(rel.encode())
        h.update(b"\x00")
        h.update((root / rel).read_bytes())
    return f"sha256:{h.hexdigest()}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                    help="ViewAlyzer source repo (default: sibling checkout)")
    ap.add_argument("--rev", type=str, default=None,
                    help="Registry rev to stamp (default: bump on change)")
    args = ap.parse_args()

    src_recorder = args.source / "ViewAlyzerRecorder"
    if not src_recorder.is_dir():
        print(f"error: no ViewAlyzerRecorder/ under {args.source}", file=sys.stderr)
        return 2

    # ---- copy the embedded trees (clean slate, no strays survive) ----------
    if RECORDER_DEST.exists():
        shutil.rmtree(RECORDER_DEST)
    copied: list[str] = []
    for tree in EMBEDDED_TREES:
        src = src_recorder / tree
        for f in sorted(p for p in src.rglob("*") if p.is_file()):
            rel = f"recorder/{tree}/{f.relative_to(src)}"
            dest = PKG_DIR / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dest)
            copied.append(rel)

    # ---- provenance ---------------------------------------------------------
    content_hash = _content_hash(PKG_DIR, copied)
    manifest_path = PKG_DIR / "manifest.json"
    manifest = (
        json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    )
    old_hash = (manifest.get("provenance") or {}).get("content_hash")

    manifest.setdefault(
        "schema", "https://opendatasheet.org/schema/v0.1/component-manifest.json"
    )
    manifest["family"] = "ViewAlyzer Recorder"
    manifest["kind"] = "component"
    pkg = manifest.setdefault("package", {})
    # Non-recorder package files (templates etc.) are preserved across syncs;
    # the recorder/ subtree is regenerated wholesale.
    extra = [f for f in pkg.get("files", []) if not f.startswith("recorder/")]
    pkg["files"] = copied + extra
    manifest["provenance"] = {
        "source_repo": "ViewAlyzer",
        "source_rev": _git_rev(args.source),
        "recorder_version": _recorder_version(
            RECORDER_DEST / "core" / "ViewAlyzer.h"
        ),
        "wire_version": WIRE_VERSION,
        "content_hash": content_hash,
    }

    # ---- registry entry -----------------------------------------------------
    index_path = REPO_ROOT / "index.json"
    index = json.loads(index_path.read_text())
    rows = index.setdefault("families", [])
    row = next((r for r in rows if r.get("id") == "viewalyzer-recorder"), None)
    if row is None:
        row = {
            "id": "viewalyzer-recorder",
            "family": "ViewAlyzer Recorder",
            "kind": "component",
            "rev": "0",
            "manifest": "viewalyzer-recorder/manifest.json",
        }
        rows.append(row)
    if args.rev is not None:
        row["rev"] = args.rev
    elif content_hash != old_hash:
        row["rev"] = str(int(row.get("rev", "0")) + 1)
    row["hash"] = content_hash

    manifest["rev"] = row["rev"]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    index_path.write_text(json.dumps(index, indent=2) + "\n")

    changed = "changed" if content_hash != old_hash else "unchanged"
    print(f"synced {len(copied)} files ({changed})")
    print(f"  rev              : {row['rev']}")
    print(f"  source_rev       : {manifest['provenance']['source_rev']}")
    print(f"  recorder_version : {manifest['provenance']['recorder_version']}")
    print(f"  content_hash     : {content_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
