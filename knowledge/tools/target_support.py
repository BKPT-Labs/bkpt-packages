# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Give a composed target what it needs besides its graph: a project foundation
it can be built on, and an entry in the knowledge corpus.

Both are derived from the target's recipe, never written by hand:

* A foundation for another part of the same device line is the reviewed base
  foundation under another identity. A derivation record in the package states
  the new identity and pins the hash of the base manifest and of the result.
  Every file entry is the base's, so no generated or vendor byte can differ.
* The corpus entry is the reviewed catalog plus one board: its scopes, its
  memberships with the source that supports each, and the two sources that
  describe it. The board is foundation backed and offers no generator recipe.

Vendor bytes stay outside the package. The foundation tree is installed from
the operator's copy of the base foundation and every file is checked against
its manifest hash while it is copied.

Usage:
    python target_support.py --recipe RECIPE.json --source-root ROOT \
        --corpus-source-root CORPUS_SOURCES --out NEW_DIRECTORY
"""
import argparse
import copy
import hashlib
import json
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load(path: pathlib.Path):
    return json.loads(path.read_text(encoding="utf-8"))


def serialized(value) -> bytes:
    return (json.dumps(value, indent=2) + "\n").encode("utf-8")


def derive_foundation(record, base: bytes) -> bytes:
    """The manifest of a foundation derived from a base manifest. The result
    is a function of the two inputs alone, and the record pins both hashes."""
    if set(record) - {"schema", "id", "directory", "base", "parts", "boards", "build_values", "configuration",
                      "provenance", "board_declares", "notes", "sha256"}:
        raise SystemExit("a foundation derivation states something this tool does not understand")
    manifest = json.loads(base)
    if sha256(base) != record["base"]["sha256"] or manifest["id"] != record["base"]["id"]:
        raise SystemExit(f"foundation {record['id']} derives from another base manifest than the one given")
    if record["id"] == manifest["id"] or not record["parts"] or not record["boards"]:
        raise SystemExit("a derived foundation needs its own id, its parts and its boards")
    manifest.update(id=record["id"], parts=record["parts"], boards=record["boards"])
    # A token the base left to its own board becomes the literal the captured
    # build files were written with.
    def literal(text):
        for token, value in record["build_values"].items():
            text = text.replace("{{" + token + "}}", value)
        return text

    manifest["build"] = {key: [literal(item) for item in value] if isinstance(value, list) else literal(value)
                         for key, value in manifest["build"].items()}
    manifest["configuration"] = manifest["configuration"] + record["configuration"]
    stated = record["provenance"]
    manifest["provenance"] = manifest["provenance"] + [{
        "uri": stated["uri"], "revision": stated["revision"], "sha256": sha256(base), "locator": stated["locator"]}]
    data = serialized(manifest)
    if sha256(data) != record["sha256"]:
        raise SystemExit(f"foundation {record['id']} derives to {sha256(data)}, not the hash its record pins")
    return data


def install_foundations(base_manifest: pathlib.Path, records, out: pathlib.Path):
    """Write a foundation tree: the base as it is, and each derived foundation
    with its own copy of the base's files. Every copied file is checked."""
    base = base_manifest.read_bytes()
    manifest = json.loads(base)
    entries = [(base_manifest.parent.name, base, manifest)]
    for record in records:
        data = derive_foundation(record, base)
        entries.append((record["directory"], data, json.loads(data)))
    if len({directory for directory, _, _ in entries}) != len(entries):
        raise SystemExit("two foundations would be installed in one directory")
    for entry in manifest["files"]:
        # Files without a root are the foundation's own; the rest come from the SDK the operator configures.
        if "root" not in entry and sha256((base_manifest.parent / entry["source"]).read_bytes()) != entry["sha256"]:
            raise SystemExit(f"{entry['source']} differs from the hash the base manifest pins")
    out.mkdir(parents=True)
    for directory, data, _ in entries:
        # The whole directory travels, including the capture receipt its provenance points at.
        shutil.copytree(base_manifest.parent, out / directory)
        (out / directory / "manifest.json").write_bytes(data)
    index = {"schema": 1, "packages": [
        {"id": described["id"], "manifest": f"{directory}/manifest.json", "sha256": sha256(data), "parts": described["parts"]}
        for directory, data, described in entries]}
    (out / "index.json").write_bytes(serialized(index))
    return index


def same_bytes(reference: pathlib.Path, path: pathlib.Path) -> str:
    """A corpus source is the file the graph was composed from. It is copied
    in when absent and never replaced."""
    data = reference.read_bytes()
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    elif path.read_bytes() != data:
        raise SystemExit(f"{path} is not the file the target was composed from")
    return sha256(data)


def corpus_overlay(catalog, recipe, manifest, hashes):
    """The reviewed catalog with one more board. `hashes` gives the hash of
    the package pinout descriptor and of the board description."""
    spec, target = recipe["corpus"], recipe["target"]
    catalog = copy.deepcopy(catalog)
    like = next(board for board in catalog["boards"] if board["id"] == spec["like_board"])
    kinds = {scope["id"]: scope["kind"] for scope in catalog["scopes"]}
    parents = [m["parent"] for m in catalog["memberships"] if m["child"] == like["part"] and kinds[m["parent"]] == "device"]
    if len(parents) != 1:
        raise SystemExit(f"part {like['part']} does not belong to exactly one device scope")
    pack = next(source for source in catalog["sources"] if source["id"] == spec["pack_description"])
    if parents[0] not in pack["scopes"]:
        raise SystemExit("the pack description does not cover the device this part belongs to")
    if manifest["sdk"] != like["sdk"] or manifest["hal"] != like["hal"]:
        raise SystemExit("the foundation is for another SDK or HAL than the board this one is modelled on")
    if any(scope["id"] == target["board"] for scope in catalog["scopes"]):
        raise SystemExit(f"the catalog already knows board {target['board']}")
    packages = {m["parent"] for m in catalog["memberships"] if m["child"] == target["part"] and kinds[m["parent"]] == "package"}
    if packages - {target["package"]}:
        raise SystemExit(f"the catalog knows part {target['part']} in package {sorted(packages)}, not {target['package']}")
    for identifier, kind, name in ((target["part"], "part", target["part"]), (target["package"], "package", target["package"]),
                                   (target["board"], "board", spec["board"]["name"])):
        if identifier not in kinds:
            catalog["scopes"].append({"id": identifier, "kind": kind, "name": name})
        elif kinds[identifier] != kind:
            raise SystemExit(f"scope {identifier} already exists as a {kinds[identifier]}")
    if target["part"] not in pack["scopes"]:
        pack["scopes"].append(target["part"])  # it describes this ordering code and its package
    pinout = next(source for source in catalog["sources"] if source["id"] == spec["pinout"]["like"])
    descriptor = pathlib.PurePosixPath(recipe["package"]["pinout"]["path"]).name
    added = [
        {**pinout, "id": spec["pinout"]["id"], "path": spec["pinout"]["path"], "sha256": hashes["pinout"],
         "uri": pinout["uri"].rsplit("/", 1)[0] + "/" + descriptor, "scopes": [target["part"]],
         "options": {**pinout["options"], "package": target["package"]}},
        # The owner's description speaks for this board alone, never for the part.
        {"id": spec["board"]["id"], "kind": "text", "path": spec["board"]["path"], "sha256": hashes["board"],
         "uri": spec["board"]["uri"], "revision": spec["board"]["revision"], "authority": spec["board"]["authority"],
         "scopes": [target["board"]], "options": {}},
    ]
    if {source["id"] for source in added} & {source["id"] for source in catalog["sources"]}:
        raise SystemExit("the catalog already has a source with one of these ids")
    catalog["sources"] += added
    # The pack description is what joins an ordering code to its device and package.
    wanted = [(target["part"], parents[0], pack["id"]), (target["part"], target["package"], pack["id"]),
              (target["board"], target["part"], spec["board"]["id"])]
    known = {(m["child"], m["parent"]) for m in catalog["memberships"]}
    catalog["memberships"] += [{"child": child, "parent": parent, "evidence": evidence}
                               for child, parent, evidence in wanted if (child, parent) not in known]
    # Foundation backed: the generator recipes of the board it is modelled on do not apply.
    catalog["boards"].append({
        "id": target["board"], "part": target["part"], "family": like["family"], "package": target["package"],
        "hal": manifest["hal"], "sdk": manifest["sdk"], "source": spec["board"]["id"],
        "backend": {"id": "template", "version": spec["backend_version"], "options": {}},
        "build": manifest["build"], "bindings": {}})
    return catalog


def description_of(board, source_root: pathlib.Path):
    """The owner's description behind a declared board file, and its path."""
    if board["board"].get("kind") != "declared":
        return None, None
    path = next(source["path"] for source in board["sources"] if source["id"] == board["board"]["source"])
    return load(source_root / path), source_root / path


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--recipe", type=pathlib.Path, required=True, action="append",
                        help="repeat to give several targets one foundation index and one corpus")
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--corpus-source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if arguments.out.exists():
        raise SystemExit("refusing to write into an existing directory")
    # Nothing is written unless every recipe composes: composition is what
    # ties a part to its package and a board description to both.
    import compose_target
    targets, records, base, catalog_path = [], [], None, None
    for path in arguments.recipe:
        recipe, folder = load(path), path.parent
        compose_target.compose(path, arguments.source_root)
        this_base = arguments.source_root / load(folder / recipe["inventory"])["foundation_manifest"]
        this_catalog = (folder / recipe["corpus"]["catalog"]).resolve()
        if targets and (this_base, this_catalog) != (base, catalog_path):
            raise SystemExit("targets installed together share one base foundation and one corpus catalog")
        base, catalog_path = this_base, this_catalog
        record = load(folder / recipe["foundation"])
        if record not in records:
            records.append(record)
        description, description_path = description_of(load(folder / recipe["board"]), arguments.source_root)
        if description is None:
            raise SystemExit("only a board declared by its owner gets a corpus entry from its recipe")
        targets.append((recipe, record, description_path))
    index = install_foundations(base, records, arguments.out / "foundations")
    catalog = load(catalog_path)
    for recipe, record, description_path in targets:
        spec = recipe["corpus"]
        hashes = {
            "pinout": same_bytes(arguments.source_root / recipe["package"]["pinout"]["path"],
                                 arguments.corpus_source_root / spec["pinout"]["path"]),
            "board": same_bytes(description_path, arguments.corpus_source_root / spec["board"]["path"]),
        }
        catalog = corpus_overlay(catalog, recipe, json.loads(derive_foundation(record, base.read_bytes())), hashes)
    # A corpus manifest is built beside the templates and components it names.
    shutil.copytree(catalog_path.parent, arguments.out / "corpus")
    (arguments.out / "corpus" / catalog_path.name).write_bytes(serialized(catalog))
    print(json.dumps({"foundations": index, "corpus_manifest": str(arguments.out / "corpus" / catalog_path.name),
                      "corpus_manifest_sha256": sha256(serialized(catalog))}, indent=1))


if __name__ == "__main__":
    sys.exit(main())
