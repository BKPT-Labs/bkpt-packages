#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Vendor the ViewAlyzer firmware recorder into the viewalyzer-recorder package.

Copies all supported embedded recorder trees from the
ViewAlyzer source repo into
`viewalyzer-recorder/recorder/`, stamps provenance (source git rev, recorder
version macro, wire version, content hash) into the package manifest, and
updates the registry entry in index.json.

Usage:
    python3 tools/sync_recorder.py [--source <ViewAlyzer repo>] [--rev N] [--check]

Without --rev the registry rev is bumped by 1 only when the content hash
changed (re-running on identical sources is a no-op).
Text payloads use LF and manifest paths use forward slashes and the same
case-sensitive ordering on every OS.
--check verifies the manifest, registry and every payload byte without writing.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parent.parent
PKG_DIR = REPO_ROOT / "viewalyzer-recorder"
RECORDER_DEST = PKG_DIR / "recorder"
DEFAULT_SOURCE = REPO_ROOT.parent / "ViewAlyzer"

# Embedded-target trees only; anything else in the recorder dir never
# ships to firmware projects.
EMBEDDED_TREES = ("core", "freertos", "rtx5", "threadx", "zephyr")
# These directories are intentionally outside the firmware component. Any new
# directory must be classified here before release; never silently omit a port.
NON_EMBEDDED_TREES = ("docs",)



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
    """major.minor.patch from the numeric defines (the single source)."""
    text = core_header.read_text()
    parts = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define\s+VA_RECORDER_VERSION_{field}\s+(\d+)", text)
        if not m:
            return "unknown"
        parts.append(m.group(1))
    return ".".join(parts)


def _wire_version(core_header: Path) -> int:
    """VA_WIRE_VERSION from the recorder source (never hardcoded here)."""
    m = re.search(r"#define\s+VA_WIRE_VERSION\s+(\d+)", core_header.read_text())
    return int(m.group(1)) if m else 0


def _content_hash(root: Path, rel_files: list[str]) -> str:
    h = hashlib.sha256()
    for rel in sorted(rel_files):
        h.update(rel.encode())
        h.update(b"\x00")
        h.update((root / rel).read_bytes())
    return f"sha256:{h.hexdigest()}"


def _source_files(src_recorder: Path) -> list[tuple[Path, str]]:
    unknown = sorted(p.name for p in src_recorder.iterdir()
                     if p.is_dir() and p.name not in EMBEDDED_TREES + NON_EMBEDDED_TREES)
    if unknown:
        raise ValueError("unclassified recorder directories: " + ", ".join(unknown)
                         + "; update EMBEDDED_TREES or NON_EMBEDDED_TREES before release")
    header = src_recorder / "core" / "ViewAlyzer.h"
    if not header.is_file() or _recorder_version(header) == "unknown" or not _wire_version(header):
        raise ValueError("recorder core header is missing or has invalid version macros")
    config = src_recorder / "core" / "ViewAlyzerConfig.h"
    if config.is_file():
        # The source advertises its RTOS choices. A truncated source checkout
        # must not turn an advertised adapter into an apparently valid package.
        adapters = re.findall(r"^\s*#define\s+VA_RTOS_(\w+)\s+\d+\b",
                              config.read_text(encoding="utf-8"), re.MULTILINE)
        for adapter in adapters:
            if adapter == "NONE":
                continue
            tree = adapter.lower()
            if tree not in EMBEDDED_TREES or not any((src_recorder / tree).rglob("*.c")):
                raise ValueError(f"advertised recorder adapter is missing or unclassified: {tree}")
    files = []
    for tree in EMBEDDED_TREES:
        src = src_recorder / tree
        # Missing optional adapters allow older recorder releases to be checked.
        if not src.exists():
            continue
        entries = list(src.rglob("*"))
        if src.is_symlink() or any(p.is_symlink() for p in entries):
            raise ValueError(f"recorder tree contains a symbolic link: {tree}")
        for f in sorted((p for p in entries if p.is_file()),
                        key=lambda p: p.relative_to(src).as_posix()):
            files.append((f, f"recorder/{tree}/{f.relative_to(src).as_posix()}"))
    return files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                    help="ViewAlyzer source repo (default: sibling checkout)")
    ap.add_argument("--rev", type=str, default=None,
                    help="Registry rev to stamp (default: bump on change)")
    ap.add_argument("--check", action="store_true",
                    help="Fail on stale package content without changing files")
    args = ap.parse_args()

    src_recorder = args.source / "ViewAlyzerRecorder"
    if not src_recorder.is_dir():
        print(f"error: no ViewAlyzerRecorder/ under {args.source}", file=sys.stderr)
        return 2

    try:
        # Validate and stage everything before replacing the published payload.
        files = _source_files(src_recorder)
        with tempfile.TemporaryDirectory(prefix="bkpt-recorder-sync-") as directory:
            return _synchronize(args, files, Path(directory))
    except (OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


def _synchronize(args: argparse.Namespace, files: list[tuple[Path, str]], stage: Path) -> int:
    copied = [rel for _, rel in files]
    for source, rel in files:
        dest = stage / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        # Canonical text bytes make Windows, Linux and macOS agree. Preserve
        # binary data, including the previous generator's nested payloads.
        data = source.read_bytes()
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            pass
        else:
            if b"\x00" not in data:
                data = data.replace(b"\r\n", b"\n")
        dest.write_bytes(data)

    # ---- provenance ---------------------------------------------------------
    content_hash = _content_hash(stage, copied)
    manifest_path = PKG_DIR / "manifest.json"
    manifest = (
        json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    )
    old_hash = (manifest.get("provenance") or {}).get("content_hash")

    manifest.setdefault(
        "schema", "https://opendatasheet.org/schema/v0.2/component-manifest.json"
    )
    manifest.pop("family", None)
    manifest["kind"] = "component"
    manifest["name"] = "ViewAlyzer Recorder"
    pkg = manifest.setdefault("package", {})
    # Non-recorder package files (templates etc.) are preserved across syncs;
    # the recorder/ subtree is regenerated wholesale.
    extra = [f for f in pkg.get("files", []) if not f.startswith("recorder/")]
    for relative in extra:
        path = PurePosixPath(relative.replace("\\", "/"))
        if path.is_absolute() or ".." in path.parts or ":" in relative:
            raise ValueError(f"invalid preserved package path: {relative}")
        file = PKG_DIR.joinpath(*path.parts)
        if not file.is_file() or not file.resolve().is_relative_to(PKG_DIR.resolve()):
            raise ValueError(f"preserved package file missing or outside package: {relative}")
    pkg["files"] = copied + extra
    manifest["provenance"] = {
        "source_repo": "ViewAlyzer",
        "source_rev": _git_rev(args.source),
        "recorder_version": _recorder_version(
            stage / "recorder" / "core" / "ViewAlyzer.h"
        ),
        "wire_version": _wire_version(stage / "recorder" / "core" / "ViewAlyzer.h"),
        "content_hash": content_hash,
    }

    # ---- registry entry -----------------------------------------------------
    index_path = REPO_ROOT / "index.json"
    index = json.loads(index_path.read_text())
    rows = index.setdefault("packages", [])
    row = next((r for r in rows if r.get("id") == "viewalyzer-recorder"), None)
    if row is None:
        row = {
            "id": "viewalyzer-recorder",
            "kind": "component",
            "name": "ViewAlyzer Recorder",
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
    if args.check:
        actual = {f"recorder/{p.relative_to(RECORDER_DEST).as_posix()}"
                  for p in RECORDER_DEST.rglob("*") if p.is_file()}
        if (json.loads(manifest_path.read_text()) != manifest
                or json.loads(index_path.read_text()) != index or actual != set(copied)
                or any((PKG_DIR / rel).read_bytes() != (stage / rel).read_bytes() for rel in copied)):
            raise ValueError("recorder package is stale; run tools/sync_recorder.py from the release sources")
        print(f"recorder package verified: {len(copied)} files, rev {row['rev']}, {content_hash}")
        return 0

    # Only the generated subtree may be removed; never follow a redirected
    # destination outside the component package.
    if RECORDER_DEST.is_symlink() or RECORDER_DEST.resolve() != PKG_DIR.resolve() / "recorder":
        raise ValueError("recorder destination must be the package's recorder directory")
    if RECORDER_DEST.exists():
        shutil.rmtree(RECORDER_DEST)
    shutil.copytree(stage / "recorder", RECORDER_DEST)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    index_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8", newline="\n")

    changed = "changed" if content_hash != old_hash else "unchanged"
    print(f"synced {len(copied)} files ({changed})")
    print(f"  rev              : {row['rev']}")
    print(f"  source_rev       : {manifest['provenance']['source_rev']}")
    print(f"  recorder_version : {manifest['provenance']['recorder_version']}")
    print(f"  content_hash     : {content_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
