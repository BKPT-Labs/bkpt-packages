# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Compose one flat target graph from layers that vary independently.

A recipe names what a target is made of:

* a reviewed graph, written for one part, package and board;
* the chip family's inventory and domains configuration;
* the part and package of this target, with that package's pinout descriptor;
* the board, as its own file: derived from a netlist for an official board, or
  declared by its owner for any other.

The reviewed graph is first re-targeted. Facts that belong to the old part,
package or board are relocated in the new sources, replaced, or dropped, and
each of those decisions is stated in the recipe: nothing is carried over by
default. The inventory and domains stages then run exactly as they do for the
original target, so every target is the same flat bundle the engine already
solves. A board may restrict what is available and say what is connected; it
cannot change what the silicon declares or waive what the foundation needs.

A composed graph is a new candidate. Reviewed layers do not make a reviewed
composition: the result needs its own review before it is accepted.

Usage:
    python compose_target.py --recipe RECIPE.json --source-root ROOT --out NEW.json
"""
import argparse
import copy
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_domains as domains  # noqa: E402
import author_device_inventory as inventory  # noqa: E402

SECTIONS = ("targets", "resources", "edges", "capabilities", "implementations")


def load(path: pathlib.Path):
    return json.loads(path.read_text(encoding="utf-8"))


def relocate(evidence, old: inventory.Source, new: inventory.Source, identifying):
    """Find the cited lines, unchanged, in another descriptor. One match or none.

    Only lines that say what they are about can move: the excerpt must carry
    every identifying member of the record, such as signal name, instance and
    pad. A fragment without them, such as a pad with its selector, can
    reappear under another signal and mean something else."""
    wanted = old.lines[evidence["start_line"] - 1:evidence["end_line"]]
    if not all(any(line.lstrip().startswith(f'"{key}"') for line in wanted) for key in identifying):
        return None
    span = len(wanted)
    found = [index + 1 for index in range(len(new.lines) - span + 1) if new.lines[index:index + span] == wanted]
    return found[0] if len(found) == 1 else None


def holders(value):
    """Every record that cites evidence, with a key that survives edits:
    top-level records and the services nested in a target."""
    for section in SECTIONS:
        for record in value[section]:
            yield (section, record["id"], None), record
            for service in record.get("services", []):
                yield (section, record["id"], service["id"]), service


class Retarget:
    """Moves a reviewed graph to another part and package, and records every
    fact it could not carry over."""

    def __init__(self, base, recipe, inventory_config, source_root):
        self.value, self.recipe, self.root = base, recipe, source_root
        self.old_prefix = inventory_config["resource_prefix"]
        self.identifying = inventory_config["pinout"]["identifying"]
        self.new = recipe["target"]
        self.report = {"relocated": [], "dropped": [], "replaced": [], "removed_pads": [],
                       "removed_capabilities": [], "restated": 0}
        self.pending = {}
        targets = [record for record in base["targets"] if record["id"] == inventory_config["target"]]
        if len(targets) != 1 or len(base["targets"]) != 1:
            raise SystemExit("re-targeting needs a graph with exactly the one configured target")
        self.target = targets[0]

    def identity(self, value):
        return self.new["resource_prefix"] + value[len(self.old_prefix):] if value.startswith(self.old_prefix + ".") else value

    def rename(self):
        old_id, new_id = self.target["id"], self.new["id"]
        for key in ("id", "board", "part", "package"):
            self.target[key] = self.new[key]
        for record in self.target["reservations"]:
            record["resource"] = self.identity(record["resource"])
        for record in self.value["evidence"]:
            record["targets"] = [new_id if name == old_id else name for name in record["targets"]]
        for section in ("resources", "edges", "capabilities", "implementations"):
            for record in self.value[section]:
                if record["target"] != old_id:
                    raise SystemExit(f"{record['id']} belongs to another target")
                record["target"] = new_id
        for record in self.value["resources"]:
            record["id"] = self.identity(record["id"])
        for record in self.value["edges"]:
            record["from"], record["to"] = self.identity(record["from"]), self.identity(record["to"])
        for capability in self.value["capabilities"]:
            for role in capability["roles"]:
                if "prefer" in role:
                    role["prefer"] = [self.identity(name) for name in role["prefer"]]

    def drop(self, identifier, replacement=None):
        """Remove a citation from every record. A replacement is added once the
        later stages have written the evidence it names."""
        self.value["evidence"] = [record for record in self.value["evidence"] if record["id"] != identifier]
        for key, holder in holders(self.value):
            if identifier in holder.get("evidence", []):
                holder["evidence"].remove(identifier)
                if replacement:
                    self.pending.setdefault(key, []).extend(replacement)
        self.report["replaced" if replacement else "dropped"].append(identifier)

    def package(self, pinout_id):
        """Swap in this package's descriptor and settle every citation of the old one."""
        spec = self.recipe["package"]["pinout"]
        record = next(source for source in self.value["sources"] if source["id"] == pinout_id)
        old = inventory.Source(record, self.root)
        data = (self.root / spec["path"]).read_bytes()
        if spec.get("sha256", inventory.sha256(data)) != inventory.sha256(data):
            raise SystemExit("the package descriptor differs from the hash the recipe pins")
        record.update(path=spec["path"], sha256=inventory.sha256(data), revision=spec["revision"])
        new = inventory.Source(record, self.root)
        decisions = self.recipe.get("scoped_evidence", {})
        for evidence in [e for e in self.value["evidence"] if e["source"] == pinout_id]:
            decision = decisions.get(evidence["id"], "relocate")
            if decision == "restate":
                # Cited again, at lines the recipe gives, with a statement of its own.
                if evidence["id"] not in self.recipe.get("restate", {}).get("evidence", {}):
                    raise SystemExit(f"evidence {evidence['id']} is marked for restating but the recipe does not restate it")
            elif decision == "relocate":
                start = relocate(evidence, old, new, self.identifying)
                if start is None:
                    raise SystemExit(f"evidence {evidence['id']} cites package lines this descriptor does not contain "
                                     "unchanged and identified; the recipe must drop, replace or restate it")
                evidence["end_line"] = start + evidence["end_line"] - evidence["start_line"]
                evidence["start_line"] = start
                evidence["excerpt_sha256"] = inventory.sha256(new.excerpt(start, evidence["end_line"]).encode("utf-8"))
                self.report["relocated"].append(evidence["id"])
            elif decision == "drop":
                self.drop(evidence["id"])
            else:
                self.drop(evidence["id"], decision["replace"])
        for identifier, decision in decisions.items():
            if identifier in {e["id"] for e in self.value["evidence"]} and decision not in ("relocate", "restate"):
                self.drop(identifier, None if decision == "drop" else decision["replace"])
        return new

    def pads(self, pinout: inventory.Source, pad_name):
        """Pads the old package bonded and this one does not leave the graph,
        with the routes, choices and preferences that named them. A capability
        that still needs one of them is not quietly left unsolvable: the recipe
        must drop it by name. Bond positions always leave."""
        bonded = {pad_name.format(port=value["port"], index=value["index"])
                  for _, _, value, _ in inventory.children(pinout, "die_pads") if value["type"] == "gpio"}
        def selected(role):
            """Pads a role with a fixed selector can take, however it names them."""
            if role.get("kind") != "pin" or not role.get("attributes"):
                return None
            return {record["id"] for record in self.value["resources"] if record["kind"] == "pin"
                    and all(record["attributes"].get(key) == value for key, value in role["attributes"].items())}

        before = {(capability["id"], role["id"]): selected(role)
                  for capability in self.value["capabilities"] for role in capability["roles"]}
        gone = set()
        for record in self.value["resources"]:
            if record["kind"] != "pin":
                continue
            record["attributes"].pop("package_position", None)
            if record["attributes"]["pin"] not in bonded:
                gone.add(record["id"])
        self.value["resources"] = [record for record in self.value["resources"] if record["id"] not in gone]
        self.value["edges"] = [edge for edge in self.value["edges"] if edge["from"] not in gone and edge["to"] not in gone]
        self.target["reservations"] = [r for r in self.target["reservations"] if r["resource"] not in gone]
        names = {identifier.split(".", 1)[1] for identifier in gone}
        dropped = set(self.recipe.get("drop_capabilities", []))
        # A role that could take a pad before and can take none now, whether
        # it named the pad or selected it by any other attribute.
        needing = sorted({capability for (capability, _), pads in before.items() if pads and pads <= gone})
        if set(needing) - dropped:
            raise SystemExit(f"capabilities {sorted(set(needing) - dropped)} need pads this package does not bond "
                             f"({sorted(names)}); the recipe must drop them")
        if dropped - set(needing):
            raise SystemExit(f"the recipe drops capabilities that lost no pad: {sorted(dropped - set(needing))}")
        self.value["capabilities"] = [c for c in self.value["capabilities"] if c["id"] not in dropped]
        if dropped:
            selected = {card for capability in self.value["capabilities"] for card in capability["implementations"]}
            self.value["implementations"] = [card for card in self.value["implementations"] if card["id"] in selected]
        for capability in self.value["capabilities"]:
            for role in capability["roles"]:
                if "prefer" in role:
                    role["prefer"] = [name for name in role["prefer"] if name not in gone]
                    if not role["prefer"]:
                        del role["prefer"]
            for parameter in capability["parameters"].values():
                if parameter.get("type") == "choice" and names & set(parameter["values"]):
                    parameter["values"] = [value for value in parameter["values"] if value not in names]
                    if not parameter["values"]:
                        raise SystemExit(f"{capability['id']}: no choice is left on this package")
        self.report["removed_pads"], self.report["removed_capabilities"] = sorted(names), sorted(dropped)

    def restate(self):
        """Wording that named the old part, package or board, restated exactly."""
        spec = self.recipe.get("restate", {})
        for old, new in spec.get("assumptions", []):
            if self.target["assumptions"].count(old) != 1:
                raise SystemExit("assumption to restate not found exactly once")
            self.target["assumptions"][self.target["assumptions"].index(old)] = new
            self.report["restated"] += 1
        records = {record["id"]: record for record in self.value["evidence"]}
        for identifier, change in spec.get("evidence", {}).items():
            record = records[identifier]
            if "device" in change or "lines" in change:
                source = inventory.Source(next(s for s in self.value["sources"] if s["id"] == record["source"]), self.root)
                if "device" in change:
                    # The device entry of this part in the same pack description.
                    opening = f'<device Dname="{change["device"]}">'
                    starts = [n for n, line in enumerate(source.lines, 1) if line.strip() == opening]
                    if len(starts) != 1:
                        raise SystemExit(f"expected one device entry {change['device']}, found {len(starts)}")
                    end = next(n for n in range(starts[0], len(source.lines) + 1) if source.lines[n - 1].strip() == "</device>")
                    record.update(start_line=starts[0], end_line=end)
                else:
                    record.update(start_line=change["lines"][0], end_line=change["lines"][1])
                record["excerpt_sha256"] = inventory.sha256(
                    source.excerpt(record["start_line"], record["end_line"]).encode("utf-8"))
            record["statement"] = change["statement"]
            self.report["restated"] += 1

    def settle(self):
        """After every stage ran: add the replacements they made citable, drop
        sources nothing cites any more, and refuse a record without a source.
        A record that left the graph takes its pending replacements with it."""
        known = {record["id"] for record in self.value["evidence"]}
        for key, holder in holders(self.value):
            for identifier in self.pending.get(key, []):
                if identifier not in known:
                    raise SystemExit(f"replacement evidence {identifier} does not exist")
                if identifier not in holder["evidence"]:
                    holder["evidence"].append(identifier)
            if not holder["evidence"]:
                raise SystemExit(f"{'/'.join(part for part in key[1:] if part)} is left without a source after re-targeting")
        cited = {record["source"] for record in self.value["evidence"]}
        self.value["sources"] = [source for source in self.value["sources"] if source["id"] in cited]


def foundation(target, inventory_config, recipe, config, source_root):
    """The graph names a project foundation by hash. A part or board that
    foundation does not list can be allocated and never built, and the target
    must say so instead of looking buildable."""
    path = source_root / inventory_config["foundation_manifest"]
    if inventory.sha256(path.read_bytes()) != target["foundation_sha256"]:
        raise SystemExit("the configured foundation manifest is not the one the graph names")
    manifest = load(path)
    if target["part"] in manifest["parts"] and target["board"] in manifest["boards"]:
        if recipe.get("allocation_only"):
            raise SystemExit("the foundation lists this part and board; the recipe must not call the target allocation only")
        return True
    if not recipe.get("allocation_only"):
        raise SystemExit(f"foundation {manifest['id']} lists neither part {target['part']} nor board {target['board']}; "
                         "the recipe must state that this target is allocation only")
    config["assumption"] += (" No project foundation lists this part and board: the target can be allocated and cannot be"
                             " materialized or built until one does.")
    return False


def apply_board(config, board, reserved_sources=()):
    """A board file replaces the board-specific parts of the domains
    configuration. It can reserve, exclude and say what is connected. It
    cannot replace a chip source, bring a chip rule, or write a chip fact."""
    allowed = {"schema", "sources", "policy", "board", "feature_pads", "profile_pads", "assumption", "assumption_replacements"}
    if set(board) - allowed:
        raise SystemExit(f"a board file cannot set {sorted(set(board) - allowed)}")
    rules = set(config["board_rules"])
    superseded = config["board"].get("source")
    kept = [source for source in config["sources"] if source["id"] != superseded]
    taken = {source["id"] for source in kept} | set(reserved_sources)
    clash = sorted(taken & {source["id"] for source in board.get("sources", [])})
    if clash:
        raise SystemExit(f"a board file cannot replace sources {clash}")
    config["sources"] = kept + board.get("sources", [])
    policy = board.get("policy")
    if policy and set(policy["lines"]) - rules:
        raise SystemExit(f"a board cannot bring the rules {sorted(set(policy['lines']) - rules)}")
    config["board"], config["board_policy"] = board["board"], policy
    features = board.get("feature_pads", {})
    for pad, feature in features.items():
        outside = sorted(key for key in feature["attributes"] if not key.startswith("board_"))
        if outside or feature["policy"] not in rules or not feature.get("circuit"):
            raise SystemExit(f"board feature {pad} may only add board_ attributes under a board rule, with the circuit that supports it")
    config["gpio_interrupt"]["feature_pads"] = features
    for profile in config["profiles"]:
        pads = {name: rule for capability in profile["capabilities"]
                for name, rule in board.get("profile_pads", {}).get(capability, {}).items()}
        if set(pads.values()) - rules:
            raise SystemExit(f"a board cannot bring the rules {sorted(set(pads.values()) - rules)}")
        profile.pop("extra_pads", None)
        if pads:
            profile["extra_pads"] = pads
    config["assumption"] = board["assumption"]
    config["assumption_replacements"] = board.get("assumption_replacements", config.get("assumption_replacements", []))


def compose(recipe_path: pathlib.Path, source_root: pathlib.Path):
    recipe = load(recipe_path)
    folder = recipe_path.parent
    inventory_config = load(folder / recipe["inventory"])
    config = load(folder / recipe["domains"])
    base_path = (folder / recipe["inventory"]).parent / inventory_config["base"]["path"]
    data = base_path.read_bytes()
    if inventory.sha256(data) != inventory_config["base"]["sha256"]:
        raise SystemExit("base graph differs from the configured hash")
    base = json.loads(data)
    retarget = None
    if "target" in recipe:
        retarget = Retarget(base, recipe, inventory_config, source_root)
        retarget.rename()
        pinout = retarget.package(inventory_config["pinout"]["source"])
        retarget.pads(pinout, inventory_config["pad_name"])
        retarget.restate()
        inventory_config = copy.deepcopy(inventory_config)
        inventory_config.update(target=recipe["target"]["id"], resource_prefix=recipe["target"]["resource_prefix"])
        # The family configuration is written for the die; this package may
        # bond fewer signals and offer fewer of the reviewed choices.
        config["composed"] = True
        config["peripherals"]["pinout_file"] = pathlib.PurePosixPath(recipe["package"]["pinout"]["path"]).name
    inventory_config["id"] = recipe["id"]
    if "board" in recipe:
        apply_board(config, load(folder / recipe["board"]), {source["id"] for source in base["sources"]})
    target = next(record for record in base["targets"] if record["id"] == inventory_config["target"])
    buildable = foundation(target, inventory_config, recipe, config, source_root)
    graph, sources, summary = inventory.build_from(inventory_config, base, source_root)
    domains.run(graph, sources, summary, inventory_config, config, source_root)
    summary["buildable"] = buildable
    if retarget:
        retarget.settle()
        summary["retargeted"] = retarget.report
    return graph, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--recipe", type=pathlib.Path, required=True)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if arguments.out.exists():
        raise SystemExit("refusing to overwrite an existing graph")
    graph, summary = compose(arguments.recipe, arguments.source_root)
    inventory.write(graph, arguments.out, summary)


if __name__ == "__main__":
    sys.exit(main())
