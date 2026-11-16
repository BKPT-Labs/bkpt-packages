# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for the stage that gives every capability its brief.

Run: python -B -m unittest knowledge/tools/test_author_briefs.py
"""
import copy
import json
import pathlib
import sys
import types
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_capability_briefs as subject  # noqa: E402
import author_device_inventory as inventory  # noqa: E402

KNOWLEDGE = pathlib.Path(__file__).resolve().parents[1]


def graph(schema=5):
    return types.SimpleNamespace(value={"schema": schema, "capabilities": [
        {"id": "one", "target": "T", "description": "The first.", "parameters": {}, "roles": [{"id": "r", "kind": "pin"}]},
        {"id": "two", "target": "T", "description": "The second.", "parameters": {}, "roles": []}]})


def config(**briefs):
    return {"schema": 1, "graph_schema": 6,
            "briefs": [{"capability": name, "brief": text} for name, text in (briefs or {"one": "First line", "two": "Second line"}).items()]}


class Briefs(unittest.TestCase):
    def tearDown(self):
        inventory.use_schema({"schema": 4}, None)

    def test_every_capability_gets_its_brief_after_the_description_and_nothing_else_changes(self):
        subject_graph = graph()
        before = copy.deepcopy(subject_graph.value)
        summary = subject.run(subject_graph, {}, config())
        after = subject_graph.value
        self.assertEqual(after["schema"], 6)
        self.assertEqual([c["brief"] for c in after["capabilities"]], ["First line", "Second line"])
        self.assertEqual(list(after["capabilities"][0]), ["id", "target", "description", "brief", "parameters", "roles"])
        stripped = {**after, "schema": 5, "capabilities": [{k: v for k, v in c.items() if k != "brief"} for c in after["capabilities"]]}
        self.assertEqual(stripped, before)
        self.assertEqual(summary["briefs"], {"capabilities": 2, "longest_bytes": 11, "not_held": []})
        # The bounds of schema 6 are those of schema 5.
        self.assertEqual(inventory.LIMITS, inventory.SCHEMA_LIMITS[5])

    def test_the_stage_is_deterministic(self):
        first, second = graph(), graph()
        subject.run(first, {}, config())
        subject.run(second, {}, config())
        self.assertEqual(inventory.encoded(first), inventory.encoded(second))

    def test_a_capability_without_a_brief_is_refused(self):
        with self.assertRaisesRegex(SystemExit, "without a brief: \\['two'\\]"):
            subject.run(graph(), {}, config(one="First line"))

    def test_a_brief_for_an_unknown_capability_is_refused(self):
        with self.assertRaisesRegex(SystemExit, "does not hold: \\['three'\\]"):
            subject.run(graph(), {}, config(one="a", two="b", three="c"))

    def test_a_brief_for_a_capability_the_recipe_dropped_is_kept_out(self):
        summary = subject.run(graph(), {}, config(one="a", two="b", three="c"), dropped=["three"])
        self.assertEqual(summary["briefs"]["not_held"], ["three"])

    def test_a_brief_the_firmware_tool_would_refuse_is_refused(self):
        for text, reason in (("", "non-empty"), (" lead", "leading or trailing"), ("trail ", "leading or trailing"),
                             ("x" * 97, "at most 96"), ("tab\there", "printable ASCII"), ("dash " + chr(0x2014) + " no", "printable ASCII"),
                             ("caf" + chr(0xE9), "printable ASCII"), ("line\nbreak", "printable ASCII"), (7, "non-empty")):
            with self.assertRaisesRegex(SystemExit, reason, msg=repr(text)):
                subject.run(graph(), {}, config(one=text, two="b"))
        # The bounds themselves pass: one byte, 96 bytes, every printable character.
        subject.run(graph(), {}, config(one="x", two="y" * 96))
        subject.run(graph(), {}, config(one="".join(chr(c) for c in range(0x21, 0x7F)), two="a b"))

    def test_a_graph_below_schema_6_takes_no_brief(self):
        with self.assertRaisesRegex(SystemExit, "needs graph schema 6"):
            subject.run(graph(), {}, {**config(), "graph_schema": 5})
        with self.assertRaisesRegex(SystemExit, "needs graph schema 6"):
            subject.run(graph(), {}, {key: value for key, value in config().items() if key != "graph_schema"})

    def test_a_duplicate_an_existing_brief_or_a_stray_key_is_refused(self):
        duplicate = config()
        duplicate["briefs"].append({"capability": "one", "brief": "Again"})
        with self.assertRaisesRegex(SystemExit, "more than one brief"):
            subject.run(graph(), {}, duplicate)
        held = graph()
        held.value["capabilities"][0]["brief"] = "Old"
        with self.assertRaisesRegex(SystemExit, "already carries a brief"):
            subject.run(held, {}, config())
        stray = config()
        stray["briefs"][0]["note"] = "x"
        with self.assertRaisesRegex(SystemExit, "nothing else"):
            subject.run(graph(), {}, stray)
        with self.assertRaisesRegex(SystemExit, "schema 1"):
            subject.run(graph(), {}, {**config(), "extra": 1})
        named = config()
        named["briefs"][0]["capability"] = 7
        with self.assertRaisesRegex(SystemExit, "nothing else"):
            subject.run(graph(), {}, named)
        listed = config()
        listed["briefs"][0] = ["one", "First line"]
        with self.assertRaisesRegex(SystemExit, "nothing else"):
            subject.run(graph(), {}, listed)
        with self.assertRaisesRegex(SystemExit, "a list of briefs"):
            subject.run(graph(), {}, {"schema": 1, "graph_schema": 6})

    def test_without_graph_schema_a_schema_6_graph_keeps_its_bounds(self):
        subject.run(graph(6), {}, {key: value for key, value in config().items() if key != "graph_schema"})
        self.assertEqual(inventory.LIMITS, inventory.SCHEMA_LIMITS[6])

    def test_the_package_graph_carries_the_family_briefs(self):
        # Every capability of the package graph carries the brief of the family
        # configuration, every brief passes the rules, and every recipe names it.
        family = json.loads((KNOWLEDGE / "stm32c5/imports/capability-briefs.json").read_text(encoding="utf-8"))
        value = json.loads((KNOWLEDGE / "stm32c5/graph.json").read_text(encoding="utf-8"))
        self.assertEqual(value["schema"], 6)
        self.assertEqual({c["id"]: c["brief"] for c in value["capabilities"]},
                         {entry["capability"]: entry["brief"] for entry in family["briefs"]})
        for entry in family["briefs"]:
            subject.check(entry["capability"], entry["brief"])
        for recipe in (KNOWLEDGE / "stm32c5/targets").glob("*.json"):
            self.assertEqual(json.loads(recipe.read_text(encoding="utf-8")).get("briefs"), "../imports/capability-briefs.json", recipe.name)


if __name__ == "__main__":
    unittest.main()
