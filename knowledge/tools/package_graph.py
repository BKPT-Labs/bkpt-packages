# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Write a generated graph into a knowledge package as the package's graph.

A package holds one graph per chip, `graph.json`. Beside it stand the list of
every source the graph pins (`source-inputs.json`), the foundation manifest it
builds on (`foundation-manifest.json`) and the review record that binds its
bytes (`graph.review.json`). The graph is updated in place: an older graph is
in the repository's history, not in the tree.

Sources BKPT authored are copied into the package so the graph can be rebuilt
from it; manufacturer sources are only listed by path and hash and are never
copied. The package manifest is given the new hashes and the capability count.
A review record, when given, must bind the exact graph bytes. Without one the
graph is written and the result says that the review record on disk no longer
binds it: `bkpt-fw architecture build` asks for a record that does.

Usage:
    python package_graph.py --graph GRAPH.json --package knowledge/stm32c5 \
        --source-root ROOT [--review REVIEW.json] [--revision N]
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import sys

GRAPH, REVIEW, INPUTS, FOUNDATION, MANIFEST = ("graph.json", "graph.review.json", "source-inputs.json",
                                               "foundation-manifest.json", "knowledge-pack.json")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def owned(source) -> bool:
    """BKPT-authored inputs say so in their authority; nothing else is copied."""
    return source["authority"].lower().startswith("bkpt")


def package(graph_path: pathlib.Path, folder: pathlib.Path, source_root: pathlib.Path, review_path=None, revision=None):
    data = graph_path.read_bytes()
    graph = json.loads(data)
    foundation = folder / FOUNDATION
    # A target names its foundation by the hash of the manifest file itself.
    if {entry["foundation_sha256"] for entry in graph["targets"]} != {sha256(foundation.read_bytes())}:
        raise SystemExit("the graph does not build on the package's foundation manifest")
    review = None
    if review_path:
        review = review_path.read_bytes()
        record = json.loads(review.decode("utf-8"))
        if record.get("bundle_sha256") != sha256(data) or record.get("decision") != "accept":
            raise SystemExit("the review record does not bind these exact bytes")
    manifest_path = folder / MANIFEST
    original = manifest_path.read_bytes().decode("utf-8")
    pack = json.loads(original)
    if revision is not None and revision < pack["revision"]:
        raise SystemExit(f"revision {revision} is older than the package's {pack['revision']}")

    inputs, copies = {"schema": 1, "owned": [], "external": []}, []
    for source in graph["sources"]:
        content = (source_root / source["path"]).read_bytes()
        if sha256(content) != source["sha256"]:
            raise SystemExit(f"source {source['id']} differs from the hash the graph pins")
        record = {key: source[key] for key in ("id", "path", "sha256", "revision", "authority")}
        if owned(source):
            record["package_path"] = "sources/" + source["path"]
            packaged = folder / record["package_path"]
            if packaged.exists():
                if sha256(packaged.read_bytes()) != source["sha256"]:
                    raise SystemExit(f"{packaged} already holds different bytes")
            else:
                copies.append((source_root / source["path"], packaged))
            inputs["owned"].append(record)
        else:
            inputs["external"].append(record)

    (folder / GRAPH).write_bytes(data)
    (folder / INPUTS).write_text(json.dumps(inputs, indent=2) + "\n", encoding="utf-8", newline="\n")
    for path, packaged in copies:
        packaged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, packaged)
    if review is not None:
        (folder / REVIEW).write_bytes(review)
    on_disk = folder / REVIEW
    bound = on_disk.exists() and json.loads(on_disk.read_text(encoding="utf-8")).get("bundle_sha256") == sha256(data)
    pack["inputs"]["architecture"] = {"path": GRAPH, "sha256": sha256(data)}
    if on_disk.exists():
        pack["inputs"]["architecture_review"] = {"path": REVIEW, "sha256": sha256(on_disk.read_bytes())}
    pack["coverage"]["capability_count"] = len(graph["capabilities"])
    if revision is not None:
        pack["revision"] = revision
    # Keep the manifest's own line endings so the change is the changed lines.
    ending = "\r\n" if "\r\n" in original else "\n"
    manifest_path.write_bytes((json.dumps(pack, indent=2) + "\n").replace("\n", ending).encode("utf-8"))
    cited = {record["package_path"] for record in inputs["owned"]}
    kept = folder / "sources"
    uncited = sorted(path.relative_to(folder).as_posix() for path in kept.rglob("*")
                     if path.is_file() and path.relative_to(folder).as_posix() not in cited) if kept.is_dir() else []
    return {"graph": str(folder / GRAPH), "sha256": sha256(data), "capabilities": len(graph["capabilities"]),
            "owned": len(inputs["owned"]), "external": len(inputs["external"]), "copied": len(copies),
            "review_binds_the_graph": bound, "revision": pack["revision"],
            "package_sources_the_graph_does_not_cite": uncited}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--graph", type=pathlib.Path, required=True)
    parser.add_argument("--package", type=pathlib.Path, required=True)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--review", type=pathlib.Path)
    parser.add_argument("--revision", type=int)
    arguments = parser.parse_args()
    print(json.dumps(package(arguments.graph, arguments.package, arguments.source_root, arguments.review, arguments.revision), indent=1))


if __name__ == "__main__":
    sys.exit(main())
