# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for composing a target from a reviewed graph, a package and a board.

Run: python -B -m unittest knowledge/tools/test_compose_target.py
"""
import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_domains as domains  # noqa: E402
import author_device_inventory as inventory  # noqa: E402
import compose_target as compose  # noqa: E402


def source(text, identifier="synthetic"):
    data = text.encode("utf-8")
    return inventory.Source({"id": identifier, "sha256": inventory.sha256(data)}, data=data)


def evidence(start, end):
    return {"id": "cite", "source": "pinout", "start_line": start, "end_line": end}


BASE = {
    "sources": [], "evidence": [
        {"id": "kept", "source": "manual", "targets": ["OLD"]},
        {"id": "board-only", "source": "board", "targets": ["OLD"]}],
    "targets": [{"id": "OLD", "board": "OLD", "part": "PART-R", "package": "BIG", "assumptions": ["Only OLD is covered."],
                 "evidence": ["kept", "board-only"], "services": [],
                 "reservations": [{"resource": "PART-R.PA9", "count": 1, "adoptable_by": []}]}],
    "resources": [
        {"id": "PART-R.BUS1", "target": "OLD", "kind": "bus", "attributes": {}, "evidence": ["kept"]},
        {"id": "PART-R.PA1", "target": "OLD", "kind": "pin", "attributes": {"pin": "PA1", "package_position": "7"}, "evidence": ["kept"]},
        {"id": "PART-R.PA9", "target": "OLD", "kind": "pin", "attributes": {"pin": "PA9", "package_position": "30"}, "evidence": ["board-only"]}],
    "edges": [
        {"id": "bus.PA1", "target": "OLD", "from": "PART-R.BUS1", "to": "PART-R.PA1", "relation": "on", "evidence": ["kept"]},
        {"id": "bus.PA9", "target": "OLD", "from": "PART-R.BUS1", "to": "PART-R.PA9", "relation": "on", "evidence": ["kept"]}],
    "capabilities": [{"id": "output", "target": "OLD", "evidence": ["kept"],
                      "parameters": {"pin": {"type": "choice", "values": ["PA1", "PA9"]}},
                      "roles": [{"id": "pin", "kind": "pin", "prefer": ["PART-R.PA9"]}]}],
    "implementations": [{"id": "card", "target": "OLD", "evidence": ["kept"]}],
}
RECIPE = {"target": {"id": "NEW", "board": "mine", "part": "PART-C", "package": "SMALL", "resource_prefix": "PART-C"}}
INVENTORY = {"resource_prefix": "PART-R", "target": "OLD"}
SMALL = '{\n  "die_pads": {\n    "PA1": {\n      "type": "gpio", "port": "A", "index": 1\n    }\n  }\n}\n'


def retargeted(recipe=RECIPE):
    subject = compose.Retarget(copy.deepcopy(BASE), recipe, INVENTORY, None)
    subject.rename()
    return subject


class Relocated(unittest.TestCase):
    def test_cited_lines_are_found_only_when_they_appear_exactly_once(self):
        old = source("a\nPA5 {\n  x\n}\nz\n")
        self.assertEqual(compose.relocate(evidence(2, 4), old, source("q\nq\nPA5 {\n  x\n}\n")), 3)
        self.assertIsNone(compose.relocate(evidence(2, 4), old, source("PA5 {\n  y\n}\n")))
        self.assertIsNone(compose.relocate(evidence(2, 4), old, source("PA5 {\n  x\n}\nPA5 {\n  x\n}\n")))


class Retargeted(unittest.TestCase):
    def test_every_identity_moves_to_the_new_part_and_target(self):
        subject = retargeted()
        value = subject.value
        self.assertEqual({record["target"] for section in ("resources", "edges", "capabilities", "implementations")
                          for record in value[section]}, {"NEW"})
        self.assertEqual([record["id"] for record in value["resources"]], ["PART-C.BUS1", "PART-C.PA1", "PART-C.PA9"])
        self.assertEqual((value["edges"][0]["from"], value["edges"][0]["to"]), ("PART-C.BUS1", "PART-C.PA1"))
        self.assertEqual(value["targets"][0]["reservations"][0]["resource"], "PART-C.PA9")
        self.assertEqual(value["capabilities"][0]["roles"][0]["prefer"], ["PART-C.PA9"])
        self.assertEqual(value["evidence"][0]["targets"], ["NEW"])

    def test_a_pad_the_package_does_not_bond_leaves_with_everything_that_named_it(self):
        subject = retargeted()
        subject.pads(source(SMALL), "P{port}{index}")
        value = subject.value
        self.assertEqual([record["id"] for record in value["resources"]], ["PART-C.BUS1", "PART-C.PA1"])
        self.assertEqual([edge["id"] for edge in value["edges"]], ["bus.PA1"])
        self.assertEqual(value["targets"][0]["reservations"], [])
        self.assertEqual(value["capabilities"][0]["parameters"]["pin"]["values"], ["PA1"])
        self.assertNotIn("prefer", value["capabilities"][0]["roles"][0])
        self.assertNotIn("package_position", value["resources"][1]["attributes"])
        self.assertEqual(subject.report["removed_pads"], ["PA9"])

    def test_a_dropped_citation_must_not_leave_a_record_without_a_source(self):
        subject = retargeted()
        subject.drop("board-only")
        self.assertEqual(subject.value["targets"][0]["evidence"], ["kept"])
        with self.assertRaises(SystemExit) as refused:
            subject.settle()
        self.assertIn("PART-C.PA9", str(refused.exception))

    def test_a_replacement_is_added_once_it_exists_and_refused_when_it_does_not(self):
        subject = retargeted()
        subject.drop("board-only", ["made-later"])
        with self.assertRaises(SystemExit):
            subject.settle()
        subject.value["evidence"].append({"id": "made-later", "source": "pinout", "targets": ["NEW"]})
        subject.settle()
        self.assertEqual(subject.value["resources"][2]["evidence"], ["made-later"])

    def test_wording_that_named_the_old_target_must_be_found_to_be_restated(self):
        recipe = {**RECIPE, "restate": {"assumptions": [["Only OLD is covered.", "Only NEW is covered."]]}}
        subject = retargeted(recipe)
        subject.restate()
        self.assertEqual(subject.value["targets"][0]["assumptions"], ["Only NEW is covered."])
        with self.assertRaises(SystemExit):
            subject.restate()


class Boards(unittest.TestCase):
    CONFIG = {"sources": [{"id": "policy"}, {"id": "netlist"}], "board": {"source": "netlist"},
              "gpio_interrupt": {"feature_pads": {"PC13": {}}},
              "profiles": [{"capabilities": ["uart"], "extra_pads": {"PA2": "console"}}],
              "assumption": "Follows the netlist.", "assumption_replacements": [["a", "b"]]}

    def test_a_board_file_replaces_only_what_belongs_to_a_board(self):
        config = copy.deepcopy(self.CONFIG)
        compose.apply_board(config, {"schema": 1, "sources": [{"id": "description"}], "policy": {"lines": {}},
                                     "board": {"kind": "declared", "source": "description"}, "assumption": "Declared."})
        self.assertEqual([entry["id"] for entry in config["sources"]], ["policy", "description"])
        self.assertEqual(config["gpio_interrupt"]["feature_pads"], {})
        self.assertNotIn("extra_pads", config["profiles"][0])
        self.assertEqual((config["assumption"], config["assumption_replacements"]), ("Declared.", [["a", "b"]]))

    def test_a_board_file_cannot_reach_into_the_chip(self):
        for key in ("instances", "profiles", "route_marks", "allocations"):
            with self.assertRaises(SystemExit, msg=key):
                compose.apply_board(copy.deepcopy(self.CONFIG), {"board": {}, "assumption": "x", key: []})

    def declared(self, description):
        subject = object.__new__(domains.Domains)
        sources = {"description": source(description, "description"),
                   "policy": source("Debug: reserved.\nGeneral: free.\nBoot: kept out.\n", "policy")}
        base = {"evidence": [], "edges": [], "resources": [
            {"id": f"PART.{pin}", "kind": "pin", "attributes": {}, "evidence": []} for pin in ("PA1", "PA13", "PH2")]}
        subject.graph = inventory.Graph(base, "board", sources)
        subject.sources, subject.value, subject.prefix, subject.pad_template = sources, base, "PART", "P{port}{index}"
        subject.pads = {name: {"port": name[1], "index": int(name[2:])} for name in ("PA1", "PA13", "PH2")}
        subject.target = {"id": "board", "evidence": [], "reservations": []}
        subject.policy_source = sources["policy"]
        subject.config = {"general_attribute": {"allocation": "general"},
                          "policy": {"source": "policy", "label": "Policy", "evidence_prefix": "p",
                                     "lines": {"debug": 1, "general": 2, "boot": 3}}}
        subject.declared_board({"kind": "declared", "source": "description"})
        return subject

    def test_a_declared_board_reserves_excludes_and_frees_the_rest(self):
        subject = self.declared('{\n  "eligible": "the rest",\n  "reserved": {\n    "PA13": "debug"\n  },\n'
                                '  "excluded": {\n    "PH2": "boot"\n  }\n}\n')
        self.assertEqual(subject.eligible, ["PA1"])
        self.assertEqual(subject.target["reservations"], [{"resource": "PART.PA13", "count": 1, "adoptable_by": []}])
        attributes = {record["id"]: record["attributes"] for record in subject.value["resources"]}
        self.assertEqual(attributes, {"PART.PA1": {"allocation": "general"}, "PART.PA13": {}, "PART.PH2": {}})
        self.assertEqual(subject.value["resources"][2]["evidence"], ["board-PH2", "p-boot"])

    def test_a_declared_board_cannot_name_a_pad_the_package_does_not_bond(self):
        with self.assertRaises(SystemExit) as refused:
            self.declared('{\n  "eligible": "the rest",\n  "reserved": {\n    "PB7": "debug"\n  },\n  "excluded": {}\n}\n')
        self.assertIn("PB7", str(refused.exception))


if __name__ == "__main__":
    unittest.main()
