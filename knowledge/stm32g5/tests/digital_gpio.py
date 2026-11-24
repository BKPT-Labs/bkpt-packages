# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Keep the G564 managed GPIO cards aligned with the existing pad domain and verified edits."""
import json
import pathlib
import unittest


PACKAGE = pathlib.Path(__file__).resolve().parents[1]


class ManagedGpio(unittest.TestCase):
    def setUp(self):
        graph = json.loads((PACKAGE / "graph.json").read_bytes())
        self.capabilities = {entry["id"]: entry for entry in graph["capabilities"]}
        self.cards = {entry["id"]: entry for entry in graph["implementations"]}
        self.sources = {entry["id"]: entry for entry in graph["sources"]}
        self.evidence = {entry["id"]: entry for entry in graph["evidence"]}

    def test_pin_and_startup_ownership(self):
        for direction in ("input", "output"):
            with self.subTest(direction=direction):
                managed = self.capabilities["digital_" + direction]
                allocation = self.capabilities["gpio_" + direction]
                self.assertEqual(managed["parameters"]["pin"], allocation["parameters"]["pin"])
                self.assertEqual(managed["roles"], allocation["roles"])
                self.assertNotIn("PC13", managed["parameters"]["pin"]["values"])
                card = self.cards[managed["implementations"][0]]
                allocation_card = self.cards[allocation["implementations"][0]]
                self.assertEqual([(e["path"], e["old"], e["new"]) for e in card["edits"]],
                                 [(e["path"], e["old"], e["new"]) for e in allocation_card["edits"]])
                self.assertEqual(card["component"]["edits"], self.cards["hal2.play-logic-gate"]["component"]["edits"])

    def test_header_excerpts_are_exact_declarations(self):
        for direction in ("input", "output"):
            component = self.cards["hal2.digital-" + direction]["component"]
            declarations = component["interface"]["declarations"].splitlines()
            ids = component["interface_sources"]["declarations"]
            self.assertEqual(len(ids), len(declarations))
            for identifier, wanted in zip(ids, declarations):
                row = self.evidence[identifier]
                self.assertEqual(row["start_line"], row["end_line"])
                source = PACKAGE / "sources" / self.sources[row["source"]]["path"]
                self.assertEqual(source.read_text().splitlines()[row["start_line"] - 1], wanted)


if __name__ == "__main__":
    unittest.main()
