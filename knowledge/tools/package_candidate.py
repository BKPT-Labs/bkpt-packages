# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Place a generated graph in a knowledge package as a candidate.

A candidate directory holds the graph, the list of every source it pins, and
the foundation manifest it builds on. Sources BKPT authored are copied into the
package so the candidate can be rebuilt from it; manufacturer sources are only
listed by path and hash and are never copied. A review record, when given, must
bind the exact graph bytes and is written beside an identical `accepted.json`.

Usage:
    python package_candidate.py --graph GRAPH.json --package knowledge/stm32c5 \
        --name device-coverage --source-root ROOT --foundation-from workflow-efficiency \
        [--review REVIEW.json]
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import sys


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def owned(source) -> bool:
    """BKPT-authored inputs say so in their authority; nothing else is copied."""
    return source["authority"].lower().startswith("bkpt")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--graph", type=pathlib.Path, required=True)
    parser.add_argument("--package", type=pathlib.Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--foundation-from", required=True, help="existing candidate whose foundation manifest this graph builds on")
    parser.add_argument("--review", type=pathlib.Path)
    arguments = parser.parse_args()

    data = arguments.graph.read_bytes()
    graph = json.loads(data)
    target = arguments.package / "candidates" / arguments.name
    if target.exists():
        raise SystemExit(f"{target} exists; a packaged candidate is never overwritten")
    foundation = arguments.package / "candidates" / arguments.foundation_from / "foundation-manifest.json"
    # A target names its foundation by the hash of the manifest file itself.
    if {entry["foundation_sha256"] for entry in graph["targets"]} != {sha256(foundation.read_bytes())}:
        raise SystemExit("the graph does not build on that candidate's foundation")
    if arguments.review:
        review = json.loads(arguments.review.read_text(encoding="utf-8"))
        if review.get("bundle_sha256") != sha256(data) or review.get("decision") != "accept":
            raise SystemExit("the review record does not accept these exact bytes")

    inputs, copies = {"schema": 1, "owned": [], "external": []}, []
    for source in graph["sources"]:
        path = arguments.source_root / source["path"]
        content = path.read_bytes()
        if sha256(content) != source["sha256"]:
            raise SystemExit(f"source {source['id']} differs from the hash the graph pins")
        record = {key: source[key] for key in ("id", "path", "sha256", "revision", "authority")}
        if owned(source):
            record["package_path"] = "sources/" + source["path"]
            packaged = arguments.package / record["package_path"]
            if packaged.exists():
                if sha256(packaged.read_bytes()) != source["sha256"]:
                    raise SystemExit(f"{packaged} already holds different bytes")
            else:
                copies.append((path, packaged))
            inputs["owned"].append(record)
        else:
            inputs["external"].append(record)

    target.mkdir(parents=True)
    (target / "candidate.json").write_bytes(data)
    (target / "source-inputs.json").write_text(json.dumps(inputs, indent=2) + "\n", encoding="utf-8", newline="\n")
    shutil.copyfile(foundation, target / "foundation-manifest.json")
    for path, packaged in copies:
        packaged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, packaged)
    if arguments.review:
        (target / "accepted.json").write_bytes(data)
        shutil.copyfile(arguments.review, target / "accepted.review.json")
    # Listing a candidate in the manifest does not activate it.
    manifest_path = arguments.package / "knowledge-pack.json"
    original = manifest_path.read_bytes().decode("utf-8")
    pack = json.loads(original)
    listed = f"candidates/{arguments.name}/candidate.json"
    if listed not in pack["candidates"]:
        pack["candidates"].append(listed)
        # Keep the manifest's own line endings so the change is one line.
        ending = "\r\n" if "\r\n" in original else "\n"
        text = json.dumps(pack, indent=2) + "\n"
        manifest_path.write_bytes(text.replace("\n", ending).encode("utf-8"))
    print(json.dumps({"candidate": str(target), "sha256": sha256(data), "owned": len(inputs["owned"]),
                      "external": len(inputs["external"]), "copied": len(copies), "accepted": bool(arguments.review)}, indent=1))


if __name__ == "__main__":
    sys.exit(main())
