# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for composing a target from a reviewed graph, a package and a board.

Run: python -B -m unittest knowledge/tools/test_compose_target.py
"""
import contextlib
import copy
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_domains as domains  # noqa: E402
import author_device_inventory as inventory  # noqa: E402
import compose_target as compose  # noqa: E402
import target_support as support  # noqa: E402


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
    BASE = {"id": "base", "sdk": "sdk-1", "hal": "HAL", "parts": ["PART-R"], "boards": ["OLD"],
            "build": {"artifact": "{{name}}.elf", "configure": ["--preset", "debug_{{board}}"]},
            "configuration": ["Captured."], "provenance": [{"uri": "capture"}],
            "files": [{"source": "clock.c", "sha256": inventory.sha256(b"clock")},
                      {"source": "sdk.h", "sha256": "0" * 64, "root": "sdk"}]}

    def record(self, data, parts=("PART-C",), boards=("mine",)):
        """A derivation record and, built independently of the tool, the manifest it must give."""
        expected = {**json.loads(data), "id": "sibling", "parts": list(parts), "boards": list(boards),
                    "build": {"artifact": "{{name}}.elf", "configure": ["--preset", "debug_OLD"]},
                    "configuration": ["Captured.", "Same files."],
                    "provenance": [{"uri": "capture"}, {"uri": "base", "revision": "1", "sha256": inventory.sha256(data),
                                                        "locator": "manifest.json"}]}
        digest = inventory.sha256((json.dumps(expected, indent=2) + "\n").encode("utf-8"))
        return {"schema": 1, "id": "sibling", "directory": "sibling-1", "base": {"id": "base", "sha256": inventory.sha256(data)},
                "parts": list(parts), "boards": list(boards), "build_values": {"board": "OLD"}, "configuration": ["Same files."],
                "provenance": {"uri": "base", "revision": "1", "locator": "manifest.json"}, "sha256": digest}, expected

    def check(self, manifest, recipe, record=None, target=None):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            data = json.dumps(manifest).encode("utf-8")
            (root / "manifest.json").write_bytes(data)
            target = target or {"part": "PART-C", "board": "mine", "sdk": "sdk-1", "foundation": "base",
                                "foundation_sha256": inventory.sha256(data)}
            config = {"assumption": "Declared."}
            result = compose.foundation(target, {"foundation_manifest": "manifest.json"}, recipe, config, root, record)
            return result, target, config

    def test_a_target_no_foundation_lists_must_say_it_cannot_be_built(self):
        with self.assertRaises(SystemExit) as refused:
            self.check(self.BASE, {})
        self.assertIn("does not list part PART-C or board mine", str(refused.exception))
        buildable, _, config = self.check(self.BASE, {"allocation_only": True})
        self.assertFalse(buildable)
        self.assertIn("cannot be materialized or built", config["assumption"])

    def test_the_refusal_names_only_what_the_foundation_does_not_list(self):
        for manifest, named, unnamed in (({**self.BASE, "parts": ["PART-C"]}, "board mine", "PART-C"),
                                         ({**self.BASE, "boards": ["mine"]}, "part PART-C", "board mine")):
            with self.assertRaises(SystemExit) as refused:
                self.check(manifest, {})
            self.assertIn(named, str(refused.exception))
            self.assertNotIn(unnamed, str(refused.exception))
            self.assertFalse(self.check(manifest, {"allocation_only": True})[0])

    def test_a_listed_target_is_buildable_and_cannot_be_called_allocation_only(self):
        listed = {**self.BASE, "parts": ["PART-C"], "boards": []}
        self.assertTrue(self.check(listed, {})[0])
        with self.assertRaises(SystemExit):
            self.check(listed, {"allocation_only": True})
        with self.assertRaises(SystemExit):
            self.check({**listed, "sdk": "sdk-2"}, {})

    def test_the_base_must_be_the_one_the_reviewed_graph_names(self):
        data = json.dumps(self.BASE).encode("utf-8")
        for target in ({"foundation": "base", "foundation_sha256": "0" * 64}, {"foundation": "other", "foundation_sha256": inventory.sha256(data)}):
            with self.assertRaises(SystemExit) as refused:
                self.check(self.BASE, {}, None, {"part": "PART-R", "board": "OLD", "sdk": "sdk-1", **target})
            self.assertIn("reviewed graph", str(refused.exception))

    def test_a_derived_foundation_is_the_base_under_another_identity(self):
        data = json.dumps(self.BASE).encode("utf-8")
        record, expected = self.record(data)
        derived = support.derive_foundation(record, data)
        self.assertEqual(json.loads(derived), expected)
        self.assertEqual(json.loads(derived)["files"], self.BASE["files"])
        buildable, target, _ = self.check(self.BASE, {}, record)
        self.assertTrue(buildable)
        self.assertEqual((target["foundation"], target["foundation_sha256"]), ("sibling", record["sha256"]))
        self.assertEqual(record["sha256"], inventory.sha256(derived))

    def test_a_foundation_with_other_bytes_cannot_take_the_place_of_the_reviewed_one(self):
        data = json.dumps(self.BASE).encode("utf-8")
        record, _ = self.record(data)
        reviewed = {"part": "PART-C", "board": "mine", "sdk": "sdk-1", "foundation": "base", "foundation_sha256": inventory.sha256(data)}
        # An unedited clock file changed and its manifest hash was updated to match.
        drifted = {**self.BASE, "files": [{"source": "clock.c", "sha256": inventory.sha256(b"another clock")}, self.BASE["files"][1]]}
        with self.assertRaises(SystemExit) as refused:
            self.check(drifted, {}, record, dict(reviewed))
        self.assertIn("reviewed graph", str(refused.exception))
        with self.assertRaises(SystemExit):
            support.derive_foundation(record, json.dumps(drifted).encode("utf-8"))
        for change in ({"sha256": "0" * 64}, {"configuration": ["Other words."]}, {"build_values": {"board": "NEW"}},
                       {"base": {"id": "other", "sha256": inventory.sha256(data)}}, {"files": []}, {"id": "base"}, {"boards": []}):
            with self.assertRaises(SystemExit, msg=change):
                support.derive_foundation({**record, **change}, data)

    def test_a_derivation_for_another_target_is_refused_even_as_allocation_only(self):
        data = json.dumps(self.BASE).encode("utf-8")
        for change, named in (({"parts": ["PART-X"]}, "part PART-C"), ({"boards": ["theirs"]}, "board mine")):
            changed, _ = self.record(data, **change)
            for recipe in ({}, {"allocation_only": True}):
                with self.assertRaises(SystemExit) as refused:
                    self.check(self.BASE, recipe, changed)
                self.assertIn(named, str(refused.exception))
                self.assertIn("another target", str(refused.exception))

    def statements(self, description, record, kind="declared"):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            (root / "board.json").write_text(json.dumps(description), encoding="utf-8")
            board = {"board": {"kind": kind, "source": "description"}, "sources": [{"id": "description", "path": "board.json"}]}
            compose.board_statements(record, board, root)

    def test_the_board_must_state_what_a_derived_foundation_depends_on(self):
        needs = {"board_declares": {"clock": {"source": "crystal", "frequency_hz": 24000000}}}
        self.statements({"clock": {"source": "crystal", "frequency_hz": 24000000}}, needs)
        self.statements({"clock": {"source": "crystal", "frequency_hz": 24000000, "tolerance_ppm": 20}}, needs)
        self.statements({}, {})
        compose.board_statements(None, None, pathlib.Path("."))
        for description in ({}, {"clock": {"source": "bypass", "frequency_hz": 24000000}}, {"clock": {"source": "crystal", "frequency_hz": 8000000}},
                            {"clock": {"source": "crystal"}}, {"clock": {"source": "crystal", "frequency_hz": "24000000"}},
                            {"clock": {"source": "crystal", "frequency_hz": 24000000.5}}, {"clock": "crystal"}):
            with self.assertRaises(SystemExit, msg=description) as refused:
                self.statements(description, needs)
            self.assertIn("clock", str(refused.exception))
        with self.assertRaises(SystemExit):
            self.statements({"clock": {"source": "crystal", "frequency_hz": 24000000}}, needs, kind="netlist")
        with self.assertRaises(SystemExit):
            compose.board_statements(needs, None, pathlib.Path("."))

    def test_an_installed_tree_holds_the_base_and_each_derived_foundation_with_checked_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            (root / "base-1").mkdir()
            data = json.dumps(self.BASE).encode("utf-8")
            (root / "base-1/manifest.json").write_bytes(data)
            (root / "base-1/clock.c").write_bytes(b"clock")
            (root / "base-1/capture-receipt.json").write_bytes(b"{}")
            record, _ = self.record(data)
            index = support.install_foundations(root / "base-1/manifest.json", [record], root / "out")
            self.assertEqual([(entry["id"], entry["manifest"], entry["sha256"], entry["parts"]) for entry in index["packages"]],
                             [("base", "base-1/manifest.json", inventory.sha256(data), ["PART-R"]),
                              ("sibling", "sibling-1/manifest.json", record["sha256"], ["PART-C"])])
            self.assertEqual(json.loads((root / "out/index.json").read_text(encoding="utf-8")), index)
            for directory in ("base-1", "sibling-1"):
                self.assertEqual((root / "out" / directory / "clock.c").read_bytes(), b"clock")
                self.assertTrue((root / "out" / directory / "capture-receipt.json").exists())
            self.assertEqual(inventory.sha256((root / "out/sibling-1/manifest.json").read_bytes()), record["sha256"])
            (root / "base-1/clock.c").write_bytes(b"another clock")
            with self.assertRaises(SystemExit) as refused:
                support.install_foundations(root / "base-1/manifest.json", [record], root / "again")
            self.assertIn("clock.c", str(refused.exception))
            self.assertFalse((root / "again").exists())
            with self.assertRaises(SystemExit):
                support.install_foundations(root / "base-1/manifest.json", [{**record, "directory": "base-1"}], root / "third")


class Corpus(unittest.TestCase):
    CATALOG = {
        "scopes": [{"id": "FAMILY", "kind": "family", "name": "FAMILY"}, {"id": "DEVICE", "kind": "device", "name": "DEVICE"},
                   {"id": "PART-R", "kind": "part", "name": "PART-R"}, {"id": "BIG", "kind": "package", "name": "BIG"},
                   {"id": "OLD", "kind": "board", "name": "OLD"}],
        "memberships": [{"child": "DEVICE", "parent": "FAMILY", "evidence": "pack"}, {"child": "PART-R", "parent": "DEVICE", "evidence": "pack"},
                        {"child": "PART-R", "parent": "BIG", "evidence": "pinout"}, {"child": "OLD", "parent": "PART-R", "evidence": "kit"}],
        "sources": [{"id": "pack", "scopes": ["FAMILY", "DEVICE"]},
                    {"id": "pinout", "kind": "pins", "path": "big.json", "sha256": "b", "uri": "pack://vendor/pinout/big.json",
                     "revision": "1", "authority": "vendor", "scopes": ["PART-R"], "options": {"package": "BIG"}}],
        "boards": [{"id": "OLD", "part": "PART-R", "family": "FAMILY", "package": "BIG", "hal": "HAL", "sdk": "sdk-1", "source": "kit",
                    "backend": {"id": "generator", "version": "1", "options": {"pack": "kit"}}, "build": {}, "bindings": {"led": "PA5"}}]}
    RECIPE = {"target": {"board": "mine", "part": "PART-C", "package": "SMALL"},
              "package": {"pinout": {"path": "descriptors/small pinout.json"}},
              "corpus": {"like_board": "OLD", "pack_description": "pack", "backend_version": "foundation-1",
                         "pinout": {"id": "pinout-small", "like": "pinout", "path": "small.json"},
                         "board": {"id": "mine-description", "path": "boards/mine.json", "uri": "board://mine/1", "revision": "1",
                                   "authority": "community", "name": "Mine"}}}
    MANIFEST = {"sdk": "sdk-1", "hal": "HAL", "build": {"configure": ["--preset", "debug_OLD"]}}

    def overlay(self, catalog=None, recipe=None, manifest=None):
        return support.corpus_overlay(catalog or self.CATALOG, recipe or self.RECIPE, manifest or self.MANIFEST,
                                      {"pinout": "p", "board": "d"})

    def test_a_board_joins_the_catalog_with_its_scopes_memberships_and_sources(self):
        before = copy.deepcopy(self.CATALOG)
        catalog = self.overlay()
        self.assertEqual(self.CATALOG, before)
        self.assertEqual([scope for scope in catalog["scopes"] if scope not in before["scopes"]],
                         [{"id": "PART-C", "kind": "part", "name": "PART-C"}, {"id": "SMALL", "kind": "package", "name": "SMALL"},
                          {"id": "mine", "kind": "board", "name": "Mine"}])
        # The pack description joins the ordering code to its device and package; the owner speaks for the board alone.
        self.assertEqual(catalog["memberships"][4:], [
            {"child": "PART-C", "parent": "DEVICE", "evidence": "pack"}, {"child": "PART-C", "parent": "SMALL", "evidence": "pack"},
            {"child": "mine", "parent": "PART-C", "evidence": "mine-description"}])
        self.assertEqual(catalog["sources"][0]["scopes"], ["FAMILY", "DEVICE", "PART-C"])
        self.assertEqual(catalog["sources"][2], {
            "id": "pinout-small", "kind": "pins", "path": "small.json", "sha256": "p", "uri": "pack://vendor/pinout/small pinout.json",
            "revision": "1", "authority": "vendor", "scopes": ["PART-C"], "options": {"package": "SMALL"}})
        self.assertEqual((catalog["sources"][3]["scopes"], catalog["sources"][3]["sha256"], catalog["sources"][3]["authority"]),
                         (["mine"], "d", "community"))
        self.assertEqual(catalog["boards"][1], {
            "id": "mine", "part": "PART-C", "family": "FAMILY", "package": "SMALL", "hal": "HAL", "sdk": "sdk-1",
            "source": "mine-description", "backend": {"id": "template", "version": "foundation-1", "options": {}},
            "build": {"configure": ["--preset", "debug_OLD"]}, "bindings": {}})

    def test_a_second_board_of_a_known_part_adds_only_itself(self):
        once = self.overlay()
        recipe = copy.deepcopy(self.RECIPE)
        recipe["target"]["board"] = "second"
        recipe["corpus"]["pinout"]["id"], recipe["corpus"]["board"]["id"] = "pinout-small-2", "second-description"
        twice = self.overlay(once, recipe)
        self.assertEqual([scope["id"] for scope in twice["scopes"]][-4:], ["PART-C", "SMALL", "mine", "second"])
        self.assertEqual(twice["memberships"][len(once["memberships"]):], [{"child": "second", "parent": "PART-C", "evidence": "second-description"}])

    def test_what_does_not_fit_the_catalog_is_refused(self):
        with self.assertRaises(SystemExit):
            self.overlay(self.overlay())
        with self.assertRaises(SystemExit):
            self.overlay(manifest={**self.MANIFEST, "sdk": "sdk-2"})
        # A part the catalog already knows in another package cannot gain a second one.
        known = copy.deepcopy(self.RECIPE)
        known["target"]["part"] = "PART-R"
        with self.assertRaises(SystemExit) as refused:
            self.overlay(recipe=known)
        self.assertIn("BIG", str(refused.exception))
        clash = copy.deepcopy(self.CATALOG)
        clash["scopes"].append({"id": "SMALL", "kind": "part", "name": "SMALL"})
        with self.assertRaises(SystemExit):
            self.overlay(clash)
        orphan = copy.deepcopy(self.CATALOG)
        orphan["sources"][0]["scopes"] = ["FAMILY"]
        with self.assertRaises(SystemExit):
            self.overlay(orphan)


class Boards(unittest.TestCase):
    CONFIG = {"sources": [{"id": "policy"}, {"id": "peripherals"}],
              "policy": {"source": "policy", "lines": {"functions": 3}},
              "board_rules": ["debug", "general", "console", "button"],
              "gpio_interrupt": {}, "profiles": [{"capabilities": ["uart"]}, {"capabilities": ["spi"]}],
              "assumption_replacements": [["a", "b"]]}
    BOARD = {"schema": 1, "sources": [{"id": "description"}],
             "board": {"kind": "declared", "source": "description"}, "assumption": "Declared."}
    FEATURE = {"policy": "button", "attributes": {"board_feature": "button"}, "circuit": [{"net": "3V3", "member": ["B1", "3"]}]}

    def test_a_board_file_supplies_every_board_specific_part(self):
        config = copy.deepcopy(self.CONFIG)
        policy = {"source": "description", "lines": {"general": 1}}
        compose.apply_board(config, {**self.BOARD, "policy": policy, "feature_pads": {"PC13": self.FEATURE},
                                     "profile_pads": {"uart": {"PA2": "console"}}})
        # After the family's policy text, before the chip sources.
        self.assertEqual([entry["id"] for entry in config["sources"]], ["policy", "description", "peripherals"])
        self.assertEqual((config["board"], config["board_policy"]), (self.BOARD["board"], policy))
        self.assertEqual(config["gpio_interrupt"]["feature_pads"], {"PC13": self.FEATURE})
        self.assertEqual(config["profiles"], [{"capabilities": ["uart"], "extra_pads": {"PA2": "console"}}, {"capabilities": ["spi"]}])
        self.assertEqual((config["assumption"], config["assumption_replacements"]), ("Declared.", [["a", "b"]]))
        # A board that brings none of them leaves none, and adds to what the family restates.
        bare = copy.deepcopy(self.CONFIG)
        compose.apply_board(bare, {**self.BOARD, "assumption_replacements": [["c", "d"]]})
        self.assertEqual(bare["assumption_replacements"], [["a", "b"], ["c", "d"]])
        self.assertEqual((bare["gpio_interrupt"]["feature_pads"], bare["board_policy"]), ({}, None))
        self.assertEqual(bare["profiles"], self.CONFIG["profiles"])

    def test_a_family_configuration_states_nothing_about_a_board(self):
        self.assertEqual(compose.board_facts(self.CONFIG), [])
        for change, named in (({"board": {"kind": "netlist", "source": "netlist"}}, "board"),
                              ({"assumption": "Follows the netlist."}, "assumption"),
                              ({"gpio_interrupt": {"feature_pads": {}}}, "feature_pads"),
                              ({"profiles": [{"capabilities": ["uart"], "extra_pads": {}}]}, "extra_pads"),
                              ({"policy": {"source": "policy", "lines": {"functions": 3, "console": 9}}}, "policy rule console"),
                              # A chip field that labels pads for a board, or rests on a rule only a board brings.
                              ({"board_policy": {"source": "policy", "lines": {}}}, "board_policy"),
                              ({"general_attribute": {"allocation": "general", "board_feature": "button"}}, "general_attribute.board_feature"),
                              ({"profiles": [{"capabilities": ["uart"], "pad_attribute": {"board_console": "true"}}]},
                               "profiles[0].pad_attribute.board_console"),
                              ({"withheld_pads": {"policy": "console", "pads": ["PA8"]}}, "withheld_pads.policy under board rule console"),
                              ({"foundation_signals": {"policy": "debug", "signals": []}}, "foundation_signals.policy under board rule debug"),
                              ({"allocations": [{"instance": {"policy": "button"}}]}, "allocations[0].instance.policy under board rule button")):
            with self.assertRaises(SystemExit, msg=change) as refused:
                compose.apply_board({**copy.deepcopy(self.CONFIG), **change}, self.BOARD)
            self.assertIn("belong in a board file", str(refused.exception))
            self.assertIn(named, str(refused.exception))
        # A chip rule may rest on a chip rule, and never on a source the board brings.
        chip = {"withheld_pads": {"policy": "functions", "pads": ["PC13"], "cite": {"source": "peripherals"}}}
        compose.apply_board({**copy.deepcopy(self.CONFIG), **chip}, self.BOARD)
        chip["withheld_pads"]["cite"]["source"] = "description"
        with self.assertRaises(SystemExit) as refused:
            compose.apply_board({**copy.deepcopy(self.CONFIG), **chip}, self.BOARD)
        self.assertIn("names the board's sources ['description']", str(refused.exception))

    def test_a_board_states_its_rules_in_a_policy_text_of_its_own_or_in_the_familys(self):
        for text in ("description", "policy"):
            config = copy.deepcopy(self.CONFIG)
            compose.apply_board(config, {**self.BOARD, "policy": {"source": text, "lines": {"debug": 1}}})
            self.assertEqual(config["board_policy"]["source"], text)
        # A line of a chip source is a manufacturer statement, never a rule.
        for text in ("peripherals", "pinout", "unknown"):
            with self.assertRaises(SystemExit, msg=text) as refused:
                compose.apply_board(copy.deepcopy(self.CONFIG), {**self.BOARD, "policy": {"source": text, "lines": {"debug": 1}}}, {"pinout"})
            self.assertIn("policy text of its own", str(refused.exception))

    def test_a_board_is_read_from_a_netlist_or_declared(self):
        compose.apply_board(copy.deepcopy(self.CONFIG), {**self.BOARD, "board": {"kind": "netlist", "source": "description"}})
        for board in ({"source": "description"}, {"kind": "schematic", "source": "description"}):
            with self.assertRaises(SystemExit, msg=board):
                compose.apply_board(copy.deepcopy(self.CONFIG), {**self.BOARD, "board": board})

    def test_a_recipe_without_a_board_is_refused_before_anything_is_read(self):
        with tempfile.TemporaryDirectory() as folder:
            recipe = pathlib.Path(folder) / "recipe.json"
            recipe.write_text(json.dumps({"schema": 1, "inventory": "missing.json", "domains": "missing.json"}), encoding="utf-8")
            with self.assertRaises(SystemExit) as refused:
                compose.compose(recipe, pathlib.Path(folder))
            self.assertIn("names the board file", str(refused.exception))

    def test_a_rule_no_policy_text_states_is_refused_by_name(self):
        subject = object.__new__(domains.Domains)
        subject.config = {"policy": {"lines": {"functions": 3}}, "board_policy": {"source": "description", "lines": {"debug": 1}}}
        with self.assertRaises(SystemExit) as refused:
            subject.policy_entry("console")
        self.assertIn("console", str(refused.exception))

    def test_the_checked_in_recipes_name_board_files_their_family_configuration_accepts(self):
        recipes = sorted(pathlib.Path(__file__).resolve().parents[1].glob("*/targets/*.json"))
        self.assertGreaterEqual(len(recipes), 3)
        kinds = set()
        # What the manufacturer's board says, so losing any of it fails here.
        nucleo = compose.load(pathlib.Path(__file__).resolve().parents[1] / "stm32c5/boards/nucleo-c542rc.json")
        self.assertEqual((nucleo["board"]["reserved_nets"], nucleo["board"]["excluded_nets"]),
                         ({"DBG.SWDIO": "debug", "DBG.SWCLK": "debug", "OSC_IN": "clock", "OSC_OUT": "clock"},
                          {"PH2_BOOT0": "boot", "PA2": "console", "PA3": "console"}))
        self.assertEqual(nucleo["profile_pads"], {"uart_transfer_operation": {"PA2": "console", "PA3": "console"},
                                                  "i2c_controller_dma": {"PB9": "two_wire_header"},
                                                  "i2c_controller_operation": {"PB9": "two_wire_header"}})
        self.assertEqual(nucleo["feature_pads"], {"PC13": {
            "policy": "user_button", "attributes": {"board_feature": "user_button"},
            "circuit": [{"net": "3V3", "member": ["B1", "3"]}, {"net": "GND", "member": ["R13", "1"]}]}})
        self.assertEqual(nucleo["policy"]["lines"], {"debug": 5, "clock": 6, "general": 7, "boot": 8, "console": 9, "user_button": 18,
                                                     "two_wire_header": 28})
        self.assertEqual((nucleo["policy"]["source"], nucleo["board"]["kind"], nucleo["board"]["source"]),
                         ("policy-v8", "netlist", "board-netlist"))
        for path in recipes:
            recipe = compose.load(path)
            config = compose.load(path.parent / recipe["domains"])
            self.assertEqual(compose.board_facts(config), [], path.name)
            board = compose.load(path.parent / recipe["board"])
            compose.apply_board(config, board)
            # Every rule the board cites is one it states itself.
            cited = {*board["board"].get("reserved_nets", {}).values(), *board["board"].get("excluded_nets", {}).values(),
                     *(feature["policy"] for feature in board.get("feature_pads", {}).values()),
                     *(rule for pads in board.get("profile_pads", {}).values() for rule in pads.values()), "general"}
            self.assertLessEqual(cited, set(board["policy"]["lines"]), path.name)
            kinds.add(config["board"]["kind"])
        self.assertEqual(kinds, {"netlist", "declared"})

    # Every checked-in recipe and the graph it composes to. A change of these
    # bytes is a change of what is published and is made here on purpose.
    COMPOSED = {
        "stm32c5/targets/nucleo-c542rc.json": "477c68df1a041c57dfb0d808ce4b24a94911e6b757ff374430b27154cdc0b987",
        "stm32c5/targets/stm32c542cc-lqfp48-declared.json": "4789e698ee6199eb6d33b2c842f42d456cb9d0a3c79391c80e3642a6715085f9",
        "stm32c5/targets/stm32c542kc-lqfp32-declared.json": "ea38c8d4e6e1f5290a27f368fd92099830998c395dea85b4f8df836984343332",
    }

    @unittest.skipUnless(os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT"), "needs the pinned sources: set BKPT_KNOWLEDGE_SOURCE_ROOT")
    def test_a_widened_role_lists_the_domain_the_accepted_graph_gave_it(self):
        knowledge = pathlib.Path(__file__).resolve().parents[1]
        graph, _ = compose.compose(knowledge / "stm32c5/targets/nucleo-c542rc.json",
                                          pathlib.Path(os.environ["BKPT_KNOWLEDGE_SOURCE_ROOT"]))
        accepted = json.loads((knowledge / "stm32c5/candidates/device-coverage-r2/accepted.json").read_bytes())
        pins = [record for record in accepted["resources"] if record["kind"] == "pin"]
        for name in ("i2c_controller_dma", "i2c_controller_operation"):
            before = next(capability for capability in accepted["capabilities"] if capability["id"] == name)
            after = next(capability for capability in graph.value["capabilities"] if capability["id"] == name)
            for role in ("scl", "sda"):
                was = next(entry for entry in before["roles"] if entry["id"] == role)
                # What the accepted graph searched for this role: every pad its attributes select, by identifier.
                domain = sorted(record["id"] for record in pins
                                if all(record["attributes"].get(key) == value for key, value in was["attributes"].items()))
                now = next(entry for entry in after["roles"] if entry["id"] == role)
                self.assertEqual(now["earlier"], [{"through": 2, "resources": domain}], (name, role))
                self.assertEqual(len(domain), 4)
            # A role that gained nothing lists nothing.
            self.assertNotIn("earlier", next(entry for entry in after["roles"] if entry["id"] == "bus"))
        widened = [(capability["id"], role["id"]) for capability in graph.value["capabilities"]
                   for role in capability["roles"] if "earlier" in role]
        self.assertEqual(len(widened), 4)
        self.assertEqual(graph.value["schema"], 5)
        self.assertTrue(all(source.get("scope") in ("device", "board", "sdk", "policy", "implementation")
                            for source in graph.value["sources"]))
        self.assertEqual({source["id"] for source in graph.value["sources"] if source["scope"] == "board"}, {"board", "board-netlist"})

    @unittest.skipUnless(os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT"), "needs the pinned sources: set BKPT_KNOWLEDGE_SOURCE_ROOT")
    def test_a_widened_profile_keeps_the_accepted_route_identifiers(self):
        knowledge = pathlib.Path(__file__).resolve().parents[1]
        graph, _ = compose.compose(knowledge / "stm32c5/targets/nucleo-c542rc.json",
                                   pathlib.Path(os.environ["BKPT_KNOWLEDGE_SOURCE_ROOT"]))
        accepted = json.loads((knowledge / "stm32c5/candidates/device-coverage-r2/accepted.json").read_bytes())
        edges = {edge["id"]: edge for edge in graph.value["edges"]}
        resources = {record["id"]: record for record in graph.value["resources"]}
        for name in ("i2c_controller_dma", "i2c_controller_operation"):
            before = next(capability for capability in accepted["capabilities"] if capability["id"] == name)
            after = next(capability for capability in graph.value["capabilities"] if capability["id"] == name)
            # The links are the accepted ones, relation for relation.
            self.assertEqual(after["links"], before["links"], name)
        for relation, role, function in (("scl_on", "scl", "i2c_scl"), ("sda_on", "sda", "i2c_sda")):
            was = {edge["id"]: edge for edge in accepted["edges"] if edge["relation"] == relation}
            now = {edge["id"]: edge for edge in graph.value["edges"] if edge["relation"] == relation}
            self.assertEqual(len(was), 2)
            for identifier, edge in was.items():
                # An accepted route keeps its identifier, ends, relation and conditions, and what it cited.
                kept = now[identifier]
                self.assertEqual([kept[key] for key in ("from", "to", "relation", "conditions")],
                                 [edge[key] for key in ("from", "to", "relation", "conditions")])
                self.assertEqual(kept["evidence"][:len(edge["evidence"])], edge["evidence"])
            # One route of the relation for every pad the role can take, each with the
            # alternate function of the descriptor route between the same ends.
            capability = next(entry for entry in graph.value["capabilities"] if entry["id"] == "i2c_controller_operation")
            marker = next(entry for entry in capability["roles"] if entry["id"] == role)["attributes"]
            described = {(edge["from"], edge["to"]): edge for edge in graph.value["edges"]
                         if edge["relation"] == "signal" and edge.get("attributes", {}).get("function") == function
                         and all(resources[edge["to"]]["attributes"].get(key) == value for key, value in marker.items())}
            self.assertEqual({(edge["from"], edge["to"]) for edge in now.values()}, set(described))
            self.assertGreater(len(now), len(was))
            for edge in now.values():
                route = described[(edge["from"], edge["to"])]
                self.assertEqual(edge["id"], f"{edge['from'].split('.', 1)[1]}.{relation}.{edge['to'].split('.', 1)[1]}")
                self.assertEqual(edge["attributes"], {"alternate": route["attributes"]["alternate"]})
                self.assertTrue(set(route["evidence"]) <= set(edge["evidence"]))
                self.assertIn("policy8-reviewed_routes", edge["evidence"])
                # The descriptor route keeps its own identifier beside it.
                self.assertIn(route["id"], edges)
            for card in ("hal2.i2c-dma", "hal2.i2c-dma-operation"):
                component = next(entry for entry in graph.value["implementations"] if entry["id"] == card)["component"]
                source = component["bindings"][f"{role}_alternate"]["source"]
                self.assertEqual((source["type"], source["relation"], source["attribute"]), ("route_attribute", relation, "alternate"))

    @unittest.skipUnless(os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT"), "needs the pinned sources: set BKPT_KNOWLEDGE_SOURCE_ROOT")
    def test_the_two_ways_of_holding_the_converter_name_different_modes(self):
        knowledge = pathlib.Path(__file__).resolve().parents[1]
        graph, _ = compose.compose(knowledge / "stm32c5/targets/nucleo-c542rc.json",
                                   pathlib.Path(os.environ["BKPT_KNOWLEDGE_SOURCE_ROOT"]))
        moded = {(capability["id"], role["id"]): (role["kind"], role["mode"]) for capability in graph.value["capabilities"]
                 for role in capability["roles"] if "mode" in role}
        self.assertEqual(moded, {("analog_input_pin", "converter"): ("analog_converter", "application_driver"),
                                 ("analog_reading", "converter"): ("analog_converter", "generated_units")})
        # Nothing else takes a converter, so no holder without a mode can sit between the two.
        holders = {capability["id"] for capability in graph.value["capabilities"]
                   for role in capability["roles"] if role["kind"] == "analog_converter"}
        self.assertEqual(holders, {"analog_input_pin", "analog_reading"})
        accepted = json.loads((knowledge / "stm32c5/candidates/device-coverage-r2/accepted.json").read_bytes())
        before = next(capability for capability in accepted["capabilities"] if capability["id"] == "analog_input_pin")
        after = next(capability for capability in graph.value["capabilities"] if capability["id"] == "analog_input_pin")
        # The mode is all the accepted allocation gained: same roles otherwise, same links, same converter capacity.
        self.assertEqual([{key: value for key, value in role.items() if key != "mode"} for role in after["roles"]], before["roles"])
        self.assertEqual(after["links"], before["links"])
        capacity = [{record["id"]: record.get("capacity") for record in value["resources"] if record["kind"] == "analog_converter"}
                    for value in (accepted, graph.value)]
        self.assertEqual(capacity[0], capacity[1])

    @unittest.skipUnless(os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT"), "needs the pinned sources: set BKPT_KNOWLEDGE_SOURCE_ROOT")
    def test_the_checked_in_recipes_compose_to_the_same_bytes(self):
        knowledge = pathlib.Path(__file__).resolve().parents[1]
        self.assertEqual(sorted(self.COMPOSED), sorted(path.relative_to(knowledge).as_posix() for path in knowledge.glob("*/targets/*.json")))
        for name, expected in self.COMPOSED.items():
            with contextlib.redirect_stdout(io.StringIO()):
                graph, _ = compose.compose(knowledge / name, pathlib.Path(os.environ["BKPT_KNOWLEDGE_SOURCE_ROOT"]))
            self.assertEqual(inventory.sha256(inventory.encoded(graph)), expected, name)

    def test_a_board_file_cannot_reach_into_the_chip(self):
        for key in ("instances", "profiles", "route_marks", "allocations"):
            with self.assertRaises(SystemExit, msg=key):
                compose.apply_board(copy.deepcopy(self.CONFIG), {"board": {}, "assumption": "x", key: []})

    def test_a_board_cannot_replace_a_chip_source_bring_a_chip_rule_or_write_a_chip_fact(self):
        board, feature = self.BOARD, self.FEATURE
        compose.apply_board(copy.deepcopy(self.CONFIG), {**board, "feature_pads": {"PC13": feature}})
        for change in ({"sources": [{"id": "peripherals"}]},
                       {"sources": [{"id": "pinout"}]},
                       {"policy": {"source": "description", "lines": {"functions": 3}}},
                       {"profile_pads": {"uart": {"PA2": "functions"}}},
                       {"feature_pads": {"PC13": {**feature, "attributes": {"port_symbol": "HAL_GPIOA"}}}},
                       {"feature_pads": {"PC13": {**feature, "policy": "functions"}}},
                       {"feature_pads": {"PC13": {**feature, "circuit": []}}}):
            with self.assertRaises(SystemExit, msg=change):
                compose.apply_board(copy.deepcopy(self.CONFIG), {**board, **change}, {"pinout"})

    def declared(self, description, withheld=(), identity='"board": "mine",\n  "part": "PART-C",\n  "package": "SMALL",\n  '):
        subject = object.__new__(domains.Domains)
        description = description.replace("{\n  ", "{\n  " + identity, 1)
        sources = {"description": source(description, "description"), "notes": source("PH2 keeps its state.\n", "notes"),
                   "policy": source("Debug: reserved.\nGeneral: free.\nBoot: kept out.\n", "policy")}
        base = {"evidence": [], "edges": [], "resources": [
            {"id": f"PART.{pin}", "kind": "pin", "attributes": {}, "evidence": []} for pin in ("PA1", "PA13", "PH2")]}
        subject.graph = inventory.Graph(base, "board", sources)
        subject.sources, subject.value, subject.prefix, subject.pad_template = sources, base, "PART", "P{port}{index}"
        subject.pads = {name: {"port": name[1], "index": int(name[2:])} for name in ("PA1", "PA13", "PH2")}
        subject.target = {"id": "board", "board": "mine", "part": "PART-C", "package": "SMALL", "evidence": [], "reservations": []}
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

    def test_a_description_is_for_one_board_carrying_one_exact_part(self):
        body = '"unlisted_pads": "free",\n  "reserved": {},\n  "excluded": {}\n}\n'
        self.declared('{\n  "description": "Worked example.",\n  "clock": {\n    "source": "crystal"\n  },\n  ' + body)
        for identity, named in (('', "board"), ('"board": "mine",\n  "part": "PART",\n  "package": "SMALL",\n  ', "part PART"),
                                ('"board": "theirs",\n  "part": "PART-C",\n  "package": "SMALL",\n  ', "board theirs"),
                                ('"board": "mine",\n  "part": "PART-C",\n  "package": "BIG",\n  ', "package BIG")):
            with self.assertRaises(SystemExit, msg=identity) as refused:
                self.declared("{\n  " + body, identity=identity)
            self.assertIn(named, str(refused.exception))

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


class Positions(unittest.TestCase):
    """A role that selects a pad by its package position means that pad."""

    def positioned(self, moved_to=None):
        subject = retargeted()
        pad = next(record for record in subject.value["resources"] if record["id"] == "PART-C.PA1")
        position = pad["attributes"]["package_position"]
        subject.value["capabilities"].append({
            "id": "at-position", "target": "NEW", "evidence": ["kept"], "parameters": {}, "implementations": ["card"],
            "roles": [{"id": "pin", "kind": "pin", "attributes": {"package_position": position}}]})
        subject.pads(source(SMALL), "P{port}{index}")
        # What the inventory stage then writes from the new package's bonds.
        pad["attributes"]["package_position"] = moved_to or position
        return subject

    def test_the_same_position_on_the_new_package_is_kept(self):
        subject = self.positioned()
        subject.settle()
        self.assertEqual(subject.positional, {("at-position", "pin"): {"PA1"}})

    def test_a_position_that_is_another_pad_or_none_on_the_new_package_is_refused(self):
        with self.assertRaises(SystemExit) as refused:
            self.positioned(moved_to="99").settle()
        self.assertIn("at-position.pin selects by package position: it meant ['PA1']", str(refused.exception))

    def test_a_role_another_stage_freed_from_its_position_is_not_held_to_it(self):
        subject = self.positioned(moved_to="99")
        subject.value["capabilities"][-1]["roles"][0]["attributes"] = {"allocation": "general"}
        subject.settle()


if __name__ == "__main__":
    unittest.main()
