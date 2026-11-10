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
solves. A board may restrict what is available and add what is connected; it
cannot change what the silicon declares.

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


def load(path: pathlib.Path):
    return json.loads(path.read_text(encoding="utf-8"))


def relocate(evidence, old: inventory.Source, new: inventory.Source):
    """Find the cited lines, unchanged, in another descriptor. One match or none."""
    wanted = old.lines[evidence["start_line"] - 1:evidence["end_line"]]
    span = len(wanted)
    found = [index + 1 for index in range(len(new.lines) - span + 1) if new.lines[index:index + span] == wanted]
    return found[0] if len(found) == 1 else None


class Retarget:
    """Moves a reviewed graph to another part and package, and records every
    fact it could not carry over."""

    def __init__(self, base, recipe, inventory_config, source_root):
        self.value, self.recipe, self.root = base, recipe, source_root
        self.old_prefix = inventory_config["resource_prefix"]
        self.new = recipe["target"]
        self.report = {"relocated": [], "dropped": [], "replaced": [], "removed_pads": [], "restated": 0}
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
        inventory has written the evidence it names."""
        self.value["evidence"] = [record for record in self.value["evidence"] if record["id"] != identifier]
        for section in ("targets", "resources", "edges", "capabilities", "implementations"):
            for record in self.value[section]:
                holders = [record, *record.get("services", [])]
                for holder in holders:
                    if identifier in holder.get("evidence", []):
                        holder["evidence"].remove(identifier)
                        if replacement:
                            self.pending.setdefault(id(holder), (holder, []))[1].extend(replacement)
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
                start = relocate(evidence, old, new)
                if start is None:
                    raise SystemExit(f"evidence {evidence['id']} cites package lines this descriptor does not contain; "
                                     "the recipe must drop or replace it")
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
        with the routes and choices that named them. Bond positions always do."""
        bonded = {pad_name.format(port=value["port"], index=value["index"])
                  for _, _, value, _ in inventory.children(pinout, "die_pads") if value["type"] == "gpio"}
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
        for capability in self.value["capabilities"]:
            for role in capability["roles"]:
                if "prefer" in role:
                    role["prefer"] = [name for name in role["prefer"] if name not in gone]
                    if not role["prefer"]:
                        del role["prefer"]
            for parameter in capability["parameters"].values():
                if parameter.get("type") == "choice" and names & set(parameter["values"]):
                    parameter["values"] = [value for value in parameter["values"] if value not in names]
        self.report["removed_pads"] = sorted(names)

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
            if "device" in change:
                # The device entry of this part in the same pack description.
                source = inventory.Source(next(s for s in self.value["sources"] if s["id"] == record["source"]), self.root)
                opening = f'<device Dname="{change["device"]}">'
                starts = [n for n, line in enumerate(source.lines, 1) if line.strip() == opening]
                if len(starts) != 1:
                    raise SystemExit(f"expected one device entry {change['device']}, found {len(starts)}")
                end = next(n for n in range(starts[0], len(source.lines) + 1) if source.lines[n - 1].strip() == "</device>")
                record.update(start_line=starts[0], end_line=end,
                              excerpt_sha256=inventory.sha256(source.excerpt(starts[0], end).encode("utf-8")))
            if "lines" in change:
                source = inventory.Source(next(s for s in self.value["sources"] if s["id"] == record["source"]), self.root)
                start, end = change["lines"]
                record.update(start_line=start, end_line=end,
                              excerpt_sha256=inventory.sha256(source.excerpt(start, end).encode("utf-8")))
            record["statement"] = change["statement"]
            self.report["restated"] += 1

    def settle(self):
        """After the inventory ran: add the replacements it made citable, then
        refuse any record left without a source."""
        known = {record["id"] for record in self.value["evidence"]}
        for holder, replacement in self.pending.values():
            for identifier in replacement:
                if identifier not in known:
                    raise SystemExit(f"replacement evidence {identifier} does not exist")
                if identifier not in holder["evidence"]:
                    holder["evidence"].append(identifier)
        for section in ("targets", "resources", "edges", "capabilities", "implementations"):
            for record in self.value[section]:
                if not record["evidence"]:
                    raise SystemExit(f"{record['id']} is left without a source after re-targeting")


def apply_board(config, board):
    """A board file replaces the board-specific parts of the domains
    configuration. It can reserve, exclude and connect; nothing else."""
    allowed = {"schema", "sources", "policy", "board", "feature_pads", "profile_pads", "assumption", "assumption_replacements"}
    if set(board) - allowed:
        raise SystemExit(f"a board file cannot set {sorted(set(board) - allowed)}")
    provided = {source["id"] for source in board.get("sources", [])}
    superseded = config["board"].get("source")
    config["sources"] = [source for source in config["sources"]
                         if source["id"] not in provided and source["id"] != superseded] + board.get("sources", [])
    config["board"] = board["board"]
    config["board_policy"] = board.get("policy")
    config["gpio_interrupt"]["feature_pads"] = board.get("feature_pads", {})
    for profile in config["profiles"]:
        pads = {name: rule for capability in profile["capabilities"]
                for name, rule in board.get("profile_pads", {}).get(capability, {}).items()}
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
    report = None
    retarget = None
    if "target" in recipe:
        retarget = Retarget(base, recipe, inventory_config, source_root)
        retarget.rename()
        pinout = retarget.package(inventory_config["pinout"]["source"])
        retarget.pads(pinout, inventory_config["pad_name"])
        retarget.restate()
        inventory_config = copy.deepcopy(inventory_config)
        inventory_config.update(target=recipe["target"]["id"], resource_prefix=recipe["target"]["resource_prefix"])
        # The family configuration lists the die's signals; this package may bond fewer.
        config["unbonded_signals"] = "report"
        report = retarget.report
    inventory_config["id"] = recipe["id"]
    if "board" in recipe:
        apply_board(config, load(folder / recipe["board"]))
    graph, sources, summary = inventory.build_from(inventory_config, base, source_root)
    if retarget:
        retarget.settle()
    domains.run(graph, sources, summary, inventory_config, config, source_root)
    if report is not None:
        summary["retargeted"] = report
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
