# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for the device authoring tools on small synthetic sources.

Run: python -B -m unittest knowledge/tools/test_author_device.py
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_inventory as inventory  # noqa: E402


def source(text, identifier="synthetic"):
    data = text.encode("utf-8")
    return inventory.Source({"id": identifier, "sha256": inventory.sha256(data)}, data=data)


HEADER = """typedef enum
{
  Core_IRQn = -1, /*!< core */
  First_IRQn = 0, /*!< first */
  Second_IRQn = 1, /*!< second, described over
                        two lines */
} IRQn_Type;
"""
STARTUP = """void (*const vectors[])(void) = {
  Core_Handler,
  First_IRQHandler,
  Second_IRQHandler
};
"""
INTERRUPTS = {"header": "header", "startup": "startup", "enum_type": "IRQn_Type", "symbol_suffix": "_IRQn",
              "handler_suffix": "_IRQHandler", "last_core_vector": "Core_Handler"}


def interrupts(header=HEADER, startup=STARTUP):
    sources = {"header": source(header, "header"), "startup": source(startup, "startup")}
    graph = inventory.Graph({"evidence": [], "resources": [], "edges": []}, "board", sources)
    config = {"interrupts": INTERRUPTS, "resource_prefix": "PART", "conditions": {}}
    return inventory.interrupts(graph, config, sources), graph


class Declarations(unittest.TestCase):
    def test_a_function_is_cited_with_its_documentation_and_body(self):
        text = "/**\n  * @brief Enable the port clock.\n  */\n__STATIC_INLINE void Enable(void)\n{\n  on();\n}\n"
        self.assertEqual(inventory.declaration(source(text), "Enable"), (1, 7, True))

    def test_an_enumerator_and_a_macro_are_one_line(self):
        text = "typedef enum\n{\n  PORT_A = BASE, /*!< port A */\n} port_t;\n#define PIN_5 LL_PIN_5 /*!< pin 5 */\n"
        self.assertEqual(inventory.declaration(source(text), "PORT_A"), (3, 3, False))
        self.assertEqual(inventory.declaration(source(text), "PIN_5"), (5, 5, False))

    def test_a_call_is_not_a_declaration(self):
        for text in ("  return Enable();\n", "return Enable();\n", "  Enable();\n", "if (Enable()) { }\n"):
            with self.assertRaises(SystemExit, msg=text):
                inventory.declaration(source(text), "Enable")

    def test_a_longer_name_is_not_the_symbol(self):
        with self.assertRaises(SystemExit):
            inventory.declaration(source("#define PIN_50 1\n  PIN_5x = 2,\n"), "PIN_5")


class Interrupts(unittest.TestCase):
    def test_external_interrupts_take_their_handler_from_the_vector_slot(self):
        (external, holes, slots), graph = interrupts()
        self.assertEqual((external, holes, slots), (2, 0, 2))
        second = next(r for r in graph.value["resources"] if r["id"] == "PART.Second.irq")
        self.assertEqual(second["attributes"]["handler_symbol"], "Second_IRQHandler")
        self.assertEqual(second["attributes"]["irq_number"], "1")

    def test_an_unreadable_declaration_is_refused_not_skipped(self):
        with self.assertRaises(SystemExit):
            interrupts(header=HEADER.replace("First_IRQn = 0,", "First_IRQn = (0),"))

    def test_a_missing_or_misplaced_handler_is_refused(self):
        with self.assertRaises(SystemExit):
            interrupts(startup=STARTUP.replace("First_IRQHandler", "0"))
        with self.assertRaises(SystemExit):
            interrupts(startup=STARTUP.replace("First_IRQHandler,\n  Second_IRQHandler", "Second_IRQHandler,\n  First_IRQHandler"))


class Sections(unittest.TestCase):
    def test_children_carry_their_lines_and_survive_braces_in_strings(self):
        text = '{\n  "items": [\n    {\n      "name": "a } b",\n      "id": 1\n    },\n    { "name": "c", "id": 2 }\n  ],\n  "named": {\n    "x[0]": {\n      "v": 1\n    }\n  }\n}\n'
        items = list(inventory.children(source(text), "items"))
        self.assertEqual([(start, end, value["id"]) for start, end, value, _ in items], [(3, 6, 1), (7, 7, 2)])
        named = list(inventory.children(source(text), "named"))
        self.assertEqual([(start, end, name) for start, end, _, name in named], [(10, 12, "x[0]")])


class ExistingPins(unittest.TestCase):
    def test_missing_attributes_are_added_and_a_disagreement_is_refused(self):
        record = {"id": "pin", "attributes": {"pin": "PA1"}, "evidence": ["old"]}
        inventory.complete(record, {"pin": "PA1", "port_symbol": "PORT_A"}, ["old", "new"])
        self.assertEqual(record["attributes"], {"pin": "PA1", "port_symbol": "PORT_A"})
        self.assertEqual(record["evidence"], ["old", "new"])
        with self.assertRaises(SystemExit):
            inventory.complete(record, {"port_symbol": "PORT_B"}, [])


if __name__ == "__main__":
    unittest.main()
