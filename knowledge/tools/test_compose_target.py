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
INVENTORY = {"resource_prefix": "PART-R", "target": "OLD",
             "pinout": {"source": "pinout", "identifying": ["name", "die_pad"]}}
SMALL = '{\n  "die_pads": {\n    "PA1": {\n      "type": "gpio", "port": "A", "index": 1\n    }\n  }\n}\n'


def retargeted(recipe=RECIPE):
    subject = compose.Retarget(copy.deepcopy(BASE), recipe, INVENTORY, None)
    subject.rename()
    return subject


class Relocated(unittest.TestCase):
    def test_cited_lines_are_found_only_when_they_appear_exactly_once(self):
        old = source('a\n{\n  "name": "BUS1_CLK",\n  "die_pad": "PA5",\n  "id": "AF5"\n}\nz\n')
        block = '{\n  "name": "BUS1_CLK",\n  "die_pad": "PA5",\n  "id": "AF5"\n}\n'
        keys = ["name", "die_pad"]
        self.assertEqual(compose.relocate(evidence(2, 6), old, source("q\nq\n" + block), keys), 3)
        self.assertIsNone(compose.relocate(evidence(2, 6), old, source(block.replace("AF5", "AF6")), keys))
        self.assertIsNone(compose.relocate(evidence(2, 6), old, source(block + block), keys))

    def test_lines_that_do_not_say_what_they_are_about_cannot_move(self):
        old = source('{\n  "die_pad": "PA5",\n  "position": "15"\n}\n')
        other = source('{\n  "die_pad": "PB9",\n  "position": "15"\n}\n')
        self.assertIsNone(compose.relocate(evidence(3, 3), old, other, ["die_pad"]))
        self.assertIsNone(compose.relocate(evidence(1, 4), old, other, ["die_pad"]))
        # The same pad and selector under another signal is another fact.
        first = source('{\n  "name": "BUS1_CLK",\n  "die_pad": "PA5",\n  "id": "AF5"\n}\n')
        second = source('{\n  "name": "BUS2_CLK",\n  "die_pad": "PA5",\n  "id": "AF5"\n}\n')
        self.assertEqual(compose.relocate(evidence(3, 4), first, second, ["die_pad"]), 3)
        self.assertIsNone(compose.relocate(evidence(3, 4), first, second, ["name", "die_pad"]))


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

    def test_a_capability_that_needs_a_removed_pad_must_be_dropped_by_name(self):
        def fixed(recipe):
            subject = retargeted(recipe)
            subject.value["capabilities"].append({
                "id": "fixed", "target": "NEW", "evidence": ["kept"], "parameters": {}, "implementations": ["only-fixed"],
                "roles": [{"id": "pin", "kind": "pin", "attributes": {"pin": "PA9"}}]})
            subject.value["capabilities"][0]["implementations"] = ["card"]
            subject.value["implementations"].append({"id": "only-fixed", "target": "NEW", "evidence": ["kept"]})
            subject.pads(source(SMALL), "P{port}{index}")
            return subject
        with self.assertRaises(SystemExit) as refused:
            fixed(RECIPE)
        self.assertIn("fixed", str(refused.exception))
        subject = fixed({**RECIPE, "drop_capabilities": ["fixed"]})
        self.assertEqual([c["id"] for c in subject.value["capabilities"]], ["output"])
        self.assertEqual([c["id"] for c in subject.value["implementations"]], ["card"])
        with self.assertRaises(SystemExit):
            fixed({**RECIPE, "drop_capabilities": ["fixed", "output"]})

    def test_a_role_that_selects_a_removed_pad_by_any_attribute_counts_as_needing_it(self):
        subject = retargeted()
        subject.value["resources"][2]["attributes"].update(port_symbol="PORT_A", pin_symbol="PIN_9")
        subject.value["capabilities"].append({
            "id": "by-symbol", "target": "NEW", "evidence": ["kept"], "parameters": {}, "implementations": ["card"],
            "roles": [{"id": "pin", "kind": "pin", "attributes": {"port_symbol": "PORT_A", "pin_symbol": "PIN_9"}}]})
        subject.value["capabilities"][0]["implementations"] = ["card"]
        with self.assertRaises(SystemExit) as refused:
            subject.pads(source(SMALL), "P{port}{index}")
        self.assertIn("by-symbol", str(refused.exception))

    def test_a_dropped_citation_must_not_leave_a_record_without_a_source(self):
        subject = retargeted()
        subject.drop("board-only")
        self.assertEqual(subject.value["targets"][0]["evidence"], ["kept"])
        with self.assertRaises(SystemExit) as refused:
            subject.settle()
        self.assertIn("PART-C.PA9", str(refused.exception))

    def test_a_service_left_without_a_source_is_refused_too(self):
        subject = retargeted()
        subject.value["targets"][0]["services"] = [{"id": "timebase", "evidence": ["board-only"]}]
        subject.value["resources"][2]["evidence"].append("kept")
        subject.drop("board-only")
        with self.assertRaises(SystemExit) as refused:
            subject.settle()
        self.assertIn("timebase", str(refused.exception))

    def test_a_record_that_left_the_graph_takes_its_pending_replacement_with_it(self):
        subject = retargeted()
        subject.drop("board-only", ["never-made"])
        subject.pads(source(SMALL), "P{port}{index}")
        subject.value["targets"][0]["evidence"].append("kept")
        with self.assertRaises(SystemExit) as refused:
            subject.settle()
        self.assertIn("never-made", str(refused.exception))
        subject.pending = {key: value for key, value in subject.pending.items() if key[0] != "targets"}
        subject.settle()

    def test_sources_nothing_cites_any_more_leave_the_graph(self):
        subject = retargeted()
        subject.value["sources"] = [{"id": "manual"}, {"id": "board"}]
        subject.drop("board-only", None)
        subject.value["resources"][2]["evidence"].append("kept")
        subject.settle()
        self.assertEqual(subject.value["sources"], [{"id": "manual"}])

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


class Foundations(unittest.TestCase):
    def check(self, manifest, recipe, target=None):
        import json
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            data = json.dumps(manifest).encode("utf-8")
            (root / "manifest.json").write_bytes(data)
            target = target or {"part": "PART-C", "board": "mine", "sdk": "sdk-1", "foundation": "base",
                                "foundation_sha256": inventory.sha256(data)}
            config = {"assumption": "Declared."}
            result = compose.foundation(target, {"foundation_manifest": "manifest.json"}, recipe, config, root)
            return result, target, config

    BASE = {"id": "base", "sdk": "sdk-1", "parts": ["PART-R"], "boards": ["OLD"]}

    def test_a_target_no_foundation_lists_must_say_it_cannot_be_built(self):
        with self.assertRaises(SystemExit):
            self.check(self.BASE, {})
        buildable, _, config = self.check(self.BASE, {"allocation_only": True})
        self.assertFalse(buildable)
        self.assertIn("cannot be materialized or built", config["assumption"])

    def test_a_listed_target_is_buildable_and_cannot_be_called_allocation_only(self):
        listed = {**self.BASE, "parts": ["PART-C"], "boards": []}
        self.assertTrue(self.check(listed, {})[0])
        with self.assertRaises(SystemExit):
            self.check(listed, {"allocation_only": True})

    def test_a_recipe_may_name_the_foundation_of_its_own_part(self):
        own = {"id": "sibling", "sdk": "sdk-1", "parts": ["PART-C"], "boards": ["mine"]}
        target = {"part": "PART-C", "board": "mine", "sdk": "sdk-1", "foundation": "base", "foundation_sha256": "old"}
        buildable, target, _ = self.check(own, {"foundation": {"id": "sibling", "manifest": "manifest.json"}}, target)
        self.assertTrue(buildable)
        self.assertEqual(target["foundation"], "sibling")
        self.assertNotEqual(target["foundation_sha256"], "old")
        for wrong in ({"id": "other", "manifest": "manifest.json"},):
            with self.assertRaises(SystemExit):
                self.check(own, {"foundation": wrong}, dict(target))
        with self.assertRaises(SystemExit):
            self.check({**own, "sdk": "sdk-2"}, {"foundation": {"id": "sibling", "manifest": "manifest.json"}}, dict(target))


class Boards(unittest.TestCase):
    CONFIG = {"sources": [{"id": "policy"}, {"id": "netlist"}, {"id": "peripherals"}], "board": {"source": "netlist"},
              "board_rules": ["debug", "general", "console", "button"],
              "gpio_interrupt": {"feature_pads": {"PC13": {}}},
              "profiles": [{"capabilities": ["uart"], "extra_pads": {"PA2": "console"}}],
              "assumption": "Follows the netlist.", "assumption_replacements": [["a", "b"]]}

    def test_a_board_file_replaces_only_what_belongs_to_a_board(self):
        config = copy.deepcopy(self.CONFIG)
        compose.apply_board(config, {"schema": 1, "sources": [{"id": "description"}], "policy": {"lines": {}},
                                     "board": {"kind": "declared", "source": "description"}, "assumption": "Declared."})
        self.assertEqual([entry["id"] for entry in config["sources"]], ["policy", "peripherals", "description"])
        self.assertEqual(config["gpio_interrupt"]["feature_pads"], {})
        self.assertNotIn("extra_pads", config["profiles"][0])
        self.assertEqual((config["assumption"], config["assumption_replacements"]), ("Declared.", [["a", "b"]]))

    def test_a_board_file_cannot_reach_into_the_chip(self):
        for key in ("instances", "profiles", "route_marks", "allocations"):
            with self.assertRaises(SystemExit, msg=key):
                compose.apply_board(copy.deepcopy(self.CONFIG), {"board": {}, "assumption": "x", key: []})

    def test_a_board_cannot_replace_a_chip_source_bring_a_chip_rule_or_write_a_chip_fact(self):
        board = {"board": {"kind": "declared", "source": "description"}, "assumption": "Declared."}
        feature = {"policy": "button", "attributes": {"board_feature": "button"}, "circuit": [{"net": "3V3", "member": ["B1", "3"]}]}
        compose.apply_board(copy.deepcopy(self.CONFIG), {**board, "feature_pads": {"PC13": feature}})
        for change in ({"sources": [{"id": "peripherals"}]},
                       {"sources": [{"id": "pinout"}]},
                       {"policy": {"lines": {"functions": 3}}},
                       {"profile_pads": {"uart": {"PA2": "functions"}}},
                       {"feature_pads": {"PC13": {**feature, "attributes": {"port_symbol": "HAL_GPIOA"}}}},
                       {"feature_pads": {"PC13": {**feature, "policy": "functions"}}},
                       {"feature_pads": {"PC13": {**feature, "circuit": []}}}):
            with self.assertRaises(SystemExit, msg=change):
                compose.apply_board(copy.deepcopy(self.CONFIG), {**board, **change}, {"pinout"})

    def declared(self, description, withheld=()):
        subject = object.__new__(domains.Domains)
        sources = {"description": source(description, "description"), "notes": source("PH2 keeps its state.\n", "notes"),
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
        if withheld:
            subject.config["withheld_pads"] = {"policy": "boot", "pads": list(withheld), "cite": {
                "id": "datasheet-note", "source": "notes", "start": 1, "end": 1, "statement": "The pad keeps its state."}}
        subject.declared_board({"kind": "declared", "source": "description"})
        return subject

    def test_a_declared_board_reserves_excludes_and_frees_the_rest(self):
        subject = self.declared('{\n  "unlisted_pads": "free",\n  "reserved": {\n    "PA13": "debug"\n  },\n'
                                '  "excluded": {\n    "PH2": "boot"\n  }\n}\n')
        self.assertEqual(subject.eligible, ["PA1"])
        self.assertEqual(subject.target["reservations"], [{"resource": "PART.PA13", "count": 1, "adoptable_by": []}])
        attributes = {record["id"]: record["attributes"] for record in subject.value["resources"]}
        self.assertEqual(attributes, {"PART.PA1": {"allocation": "general"}, "PART.PA13": {}, "PART.PH2": {}})
        self.assertEqual(subject.value["resources"][2]["evidence"], ["board-PH2", "p-boot"])

    def test_a_declared_board_cannot_name_a_pad_the_package_does_not_bond(self):
        with self.assertRaises(SystemExit) as refused:
            self.declared('{\n  "unlisted_pads": "free",\n  "reserved": {\n    "PB7": "debug"\n  },\n  "excluded": {}\n}\n')
        self.assertIn("PB7", str(refused.exception))

    def test_a_declared_board_frees_unnamed_pads_only_when_it_says_so_in_the_one_understood_way(self):
        for statement in ('"unlisted_pads": "unavailable"', '"unlisted_pads": false', '"eligible": "the rest"',
                          '"unlisted_pads": "free",\n  "note": "but not PA1"'):
            with self.assertRaises(SystemExit, msg=statement):
                self.declared('{\n  ' + statement + ',\n  "reserved": {},\n  "excluded": {}\n}\n')

    def test_a_pad_the_chip_withholds_stays_out_whatever_the_board_says(self):
        subject = self.declared('{\n  "unlisted_pads": "free",\n  "reserved": {\n    "PA13": "debug"\n  },\n  "excluded": {}\n}\n',
                                withheld=["PH2"])
        self.assertEqual(subject.eligible, ["PA1"])
        record = next(r for r in subject.value["resources"] if r["id"] == "PART.PH2")
        self.assertEqual((record["attributes"], record["evidence"]), ({}, ["p-boot", "datasheet-note", "board-eligible"]))

    def test_the_board_must_reserve_every_pad_the_foundation_uses(self):
        subject = self.declared('{\n  "unlisted_pads": "free",\n  "reserved": {\n    "PA13": "debug"\n  },\n  "excluded": {}\n}\n')
        subject.value["edges"] = [
            {"id": "route.SWDIO.PA13", "relation": "signal", "to": "PART.PA13", "attributes": {"signal": "SWDIO"}},
            {"id": "route.OSC.PA1", "relation": "signal", "to": "PART.PA1", "attributes": {"signal": "OSC"}}]
        subject.config["foundation_signals"] = {"policy": "debug", "signals": ["SWDIO"]}
        subject.foundation_pads()
        subject.config["foundation_signals"]["signals"] = ["SWDIO", "OSC"]
        with self.assertRaises(SystemExit) as refused:
            subject.foundation_pads()
        self.assertIn("OSC on PA1", str(refused.exception))


if __name__ == "__main__":
    unittest.main()
