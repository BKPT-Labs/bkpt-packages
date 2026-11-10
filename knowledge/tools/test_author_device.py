# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for the device authoring tools on small synthetic sources.

Run: python -B -m unittest knowledge/tools/test_author_device.py
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_domains as domains  # noqa: E402
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
        self.assertEqual(inventory.declaration(source(text), "Enable"), (1, 7, "definition"))

    def test_a_prototype_is_not_a_definition_and_takes_no_unrelated_comment(self):
        text = "#endif /* OTHER */\nint Set(int source);\n/**\n  * @brief Get it.\n  */\nint Get(int a,\n        int b);\n"
        self.assertEqual(inventory.declaration(source(text), "Set"), (2, 2, "prototype"))
        self.assertEqual(inventory.declaration(source(text), "Get"), (3, 7, "prototype"))
        graph = inventory.Graph({"evidence": [], "resources": [], "edges": []}, "board", {})
        graph.cite_symbol("api", source(text), "Set")
        self.assertIn("does not contain its definition", graph.value["evidence"][0]["statement"])

    def test_an_enumerator_and_a_macro_are_one_line(self):
        text = "typedef enum\n{\n  PORT_A = BASE, /*!< port A */\n} port_t;\n#define PIN_5 LL_PIN_5 /*!< pin 5 */\n"
        self.assertEqual(inventory.declaration(source(text), "PORT_A"), (3, 3, "value"))
        self.assertEqual(inventory.declaration(source(text), "PIN_5"), (5, 5, "value"))

    def test_a_one_line_comment_above_an_undescribed_enumerator_is_cited_with_it(self):
        text = "typedef enum\n{\n  /** Channel 1 */\n  CH_1 = 0U,\n\n  CH_2 = 1U,\n  /** not for the next */\n  CH_3 = 2U, /*!< own */\n} ch_t;\n"
        self.assertEqual(inventory.declaration(source(text), "CH_1"), (3, 4, "value"))
        self.assertEqual(inventory.declaration(source(text), "CH_2"), (6, 6, "value"))
        self.assertEqual(inventory.declaration(source(text), "CH_3"), (8, 8, "value"))
        graph = inventory.Graph({"evidence": [], "resources": [], "edges": []}, "board", {})
        graph.cite_symbol("mark", source(text), "CH_2")
        self.assertIn("carries no description", graph.value["evidence"][0]["statement"])

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


SVD = """<device>
  <peripherals>
    <peripheral>
      <name>BUS1</name>
      <interrupt>
        <name>BUS1</name>
        <value>7</value>
      </interrupt>
      <registers>
        <register>
          <name>CR</name>
        </register>
      </registers>
    </peripheral>
    <peripheral derivedFrom="BUS1">
      <name>BUS2</name>
      <interrupt>
        <name>BUS2</name>
        <value>8</value>
      </interrupt>
    </peripheral>
  </peripherals>
</device>
"""
POLICY = ("Functions: authored. BUS1_CLK and BUS2_CLK are bus_clock. BUS1_TX is bus_tx. Nothing else.\n"
          "Requests: authored.\n"
          "Symbols: BUS1_CLK is LL_DMA1_REQUEST_BUS2_RX.\n"
          "General: authored.\n"
          "Allocation: authored.\n")
TABLE = "request table\n4 bus2_rx_dma\n5 bus2_tx_dma\n"
LOW = "#define LL_DMA1_REQUEST_BUS2_RX 4U /*!< rx */\n#define LL_DMA1_REQUEST_BUS2_TX 5U /*!< tx */\n"
HIGH = "  HAL_DMA1_REQUEST_BUS2_RX = LL_DMA1_REQUEST_BUS2_RX, /*!< rx */\n  HAL_DMA1_REQUEST_BUS2_TX = LL_DMA1_REQUEST_BUS2_TX, /*!< tx */\n"


def domains_under_test(functions=None, low=LOW, high=HIGH, table=TABLE):
    """A Domains object over synthetic sources, without the board stage."""
    sources = {"policy": source(POLICY, "policy"), "table": source(table, "table"),
               "low": source(low, "low"), "high": source(high, "high")}
    base = {"evidence": [], "edges": [
        {"id": "route.BUS1_CLK.PA1", "relation": "signal", "from": "PART.BUS1", "to": "PART.PA1",
         "attributes": {"signal": "BUS1_CLK"}, "evidence": []},
        {"id": "route.BUS2_CLK.PA2", "relation": "signal", "from": "PART.BUS2", "to": "PART.PA2",
         "attributes": {"signal": "BUS2_CLK"}, "evidence": []},
        {"id": "route.BUS1_TX.PA3", "relation": "signal", "from": "PART.BUS1", "to": "PART.PA3",
         "attributes": {"signal": "BUS1_TX"}, "evidence": []},
        {"id": "route.BUS1_RX.PA4", "relation": "signal", "from": "PART.BUS1", "to": "PART.PA4",
         "attributes": {"signal": "BUS1_RX"}, "evidence": []}],
        "resources": [{"id": "PART.CH0", "kind": "channel", "attributes": {"controller": "DMA1"}, "evidence": []},
                      {"id": "PART.CH1", "kind": "channel", "attributes": {"controller": "DMA1"}, "evidence": []},
                      {"id": "PART.BUS1", "kind": "bus", "attributes": {}, "evidence": []},
                      {"id": "PART.BUS2", "kind": "bus", "attributes": {}, "evidence": []}],
        "capabilities": [], "implementations": []}
    subject = object.__new__(domains.Domains)
    subject.graph = inventory.Graph(base, "board", sources)
    subject.sources, subject.value, subject.prefix, subject.conditions = sources, base, "PART", {}
    subject.policy_source = sources["policy"]
    subject.config = {
        "policy": {"source": "policy", "lines": {"functions": 1, "requests": 2, "symbols": 3, "general": 4, "allocation": 5}},
        "general_attribute": {"allocation": "general"},
        "route_marks": [{"attribute": "function", "policy": "functions", "values": functions or {
            "bus_clock": ["BUS1_CLK", "BUS2_CLK"], "bus_tx": ["BUS1_TX"]}}],
        "dma_requests": {
            "policy": "requests", "kind": "request", "channel_kind": "channel",
            "table": {"source": "table", "row": "{selector} {name}_{direction}_dma"},
            "controllers": {"DMA1": {"ll": ["low", "LL_DMA1_REQUEST_{NAME}_{DIRECTION}"],
                                     "hal": ["high", "HAL_DMA1_REQUEST_{NAME}_{DIRECTION}"]}},
            "attributes": {"ownership_policy": "one"}, "evidence": [], "channel_evidence": [],
            "channel_selection": {"id": "selection", "source": "table", "start": 1, "end": 1,
                                  "statement": "A request is selected per channel."}}}
    return subject


class DeviceDescription(unittest.TestCase):
    def test_a_derived_peripheral_names_its_layout_and_its_own_interrupt(self):
        blocks = domains.description_blocks(source(SVD))
        self.assertIsNone(blocks["BUS1"]["derived"])
        self.assertEqual(blocks["BUS2"]["derived"], "BUS1")
        self.assertEqual([(entry["name"], entry["value"]) for entry in blocks["BUS2"]["interrupts"]], [("BUS2", "8")])
        self.assertEqual((blocks["BUS2"]["start"], blocks["BUS2"]["name_line"]), (15, 16))

    def test_a_peripheral_described_twice_is_refused(self):
        with self.assertRaises(SystemExit):
            domains.description_blocks(source(SVD.replace("<name>BUS2</name>\n      <interrupt>", "<name>BUS1</name>\n      <interrupt>")))


class Restated(unittest.TestCase):
    def test_reviewed_wording_must_be_found_exactly_once(self):
        self.assertEqual(domains.restate("card", "fixed BUS1 only", [["fixed BUS1", "any bus"]]), "any bus only")
        for text in ("no anchor here", "fixed BUS1 and fixed BUS1"):
            with self.assertRaises(SystemExit):
                domains.restate("card", text, [["fixed BUS1", "any bus"]])


class Functions(unittest.TestCase):
    def test_only_the_signals_the_policy_names_are_marked(self):
        subject = domains_under_test()
        self.assertEqual(subject.route_marks(), {"function=bus_clock": 2, "function=bus_tx": 1})
        marked = {edge["id"]: edge["attributes"].get("function") for edge in subject.value["edges"]}
        self.assertEqual(marked, {"route.BUS1_CLK.PA1": "bus_clock", "route.BUS2_CLK.PA2": "bus_clock",
                                  "route.BUS1_TX.PA3": "bus_tx", "route.BUS1_RX.PA4": None})

    def test_a_configuration_that_differs_from_the_policy_is_refused(self):
        for functions in ({"bus_clock": ["BUS1_CLK"], "bus_tx": ["BUS1_TX"]},
                          {"bus_clock": ["BUS1_CLK", "BUS2_CLK"], "bus_tx": ["BUS1_TX", "BUS1_RX"]},
                          {"bus_clock": ["BUS1_CLK", "BUS2_CLK"], "bus_rx": ["BUS1_RX"]}):
            with self.assertRaises(SystemExit, msg=functions):
                domains_under_test(functions).route_marks()

    def test_a_symbol_value_is_cited_where_the_sdk_declares_it(self):
        subject = domains_under_test()
        subject.config["route_marks"] = [{"attribute": "channel_symbol", "policy": "symbols", "symbol_source": "low",
                                          "values": {"LL_DMA1_REQUEST_BUS2_RX": ["BUS1_CLK"]}}]
        self.assertEqual(subject.route_marks(), {"channel_symbol=LL_DMA1_REQUEST_BUS2_RX": 1})
        edge = subject.value["edges"][0]
        self.assertEqual(edge["attributes"]["channel_symbol"], "LL_DMA1_REQUEST_BUS2_RX")
        self.assertEqual(edge["evidence"], ["policy3-symbols", "inv-mark-LL_DMA1_REQUEST_BUS2_RX"])
        subject.config["route_marks"][0]["values"] = {"NOT_DECLARED": ["BUS1_CLK"]}
        with self.assertRaises(SystemExit):
            subject.route_marks()


class Allocations(unittest.TestCase):
    SPEC = {"capability": "bus_clock_pin", "card": "card.bus-clock", "policy": "allocation", "function": "bus_clock",
            "instance_kind": "bus", "instance_role": "bus", "route_attributes": ["signal"],
            "description": "Allocate.", "guidance": "Allocation only."}

    def allocated(self, eligible, **changes):
        subject = domains_under_test()
        subject.eligible = eligible
        subject.route_marks()
        return subject, subject.allocation({**self.SPEC, **changes})

    def test_only_eligible_pads_of_covered_instances_are_offered(self):
        subject, routes = self.allocated(["PA1"])
        self.assertEqual(routes, ["BUS1:PA1"])
        capability = subject.value["capabilities"][0]
        self.assertEqual(capability["links"], [{"from": "bus", "to": "pad", "relation": "signal",
                                                "attributes": {"function": "bus_clock"}}])
        self.assertEqual([role["kind"] for role in capability["roles"]], ["bus", "pin"])
        self.assertEqual(subject.value["implementations"][0]["edits"], [])

    def test_an_allocation_with_nothing_to_offer_or_a_missing_value_is_refused(self):
        with self.assertRaises(SystemExit):
            self.allocated([])
        with self.assertRaises(SystemExit):
            self.allocated(["PA1"], route_attributes=["channel_symbol"])
        with self.assertRaises(SystemExit):
            self.allocated(["PA1"], instance_kind="converter")


class Requests(unittest.TestCase):
    def test_a_request_line_needs_table_and_definitions_to_agree(self):
        subject = domains_under_test()
        identifier = subject.request("BUS2", "rx")
        record = next(r for r in subject.value["resources"] if r["id"] == identifier)
        self.assertEqual(record["attributes"], {"peripheral": "BUS2", "direction": "rx", "selector": "4",
                                                "hal_request_dma1": "HAL_DMA1_REQUEST_BUS2_RX", "ownership_policy": "one"})
        added = [(edge["relation"], edge["to"]) for edge in subject.value["edges"] if "request" in edge["id"]]
        self.assertEqual(added, [("rx_request", identifier), ("routes_to", "PART.CH0"), ("routes_to", "PART.CH1")])

    def test_a_disagreeing_or_unrelated_definition_is_refused(self):
        with self.assertRaises(SystemExit):
            domains_under_test(low=LOW.replace("RX 4U", "RX 6U")).request("BUS2", "rx")
        with self.assertRaises(SystemExit):
            domains_under_test(high=HIGH.replace("= LL_DMA1_REQUEST_BUS2_RX,", "= 4,")).request("BUS2", "rx")
        with self.assertRaises(SystemExit):
            domains_under_test(table=TABLE + "9 bus2_rx_dma\n").request("BUS2", "rx")

    def test_a_route_needs_its_channel_controller_checked(self):
        subject = domains_under_test()
        subject.value["resources"][1]["attributes"]["controller"] = "DMA2"
        with self.assertRaises(SystemExit):
            subject.request("BUS2", "rx")

    def test_a_recorded_request_is_held_to_the_same_facts(self):
        subject = domains_under_test()
        identifier = subject.request("BUS2", "rx")
        counts = (len(subject.value["resources"]), len(subject.value["edges"]), len(subject.value["evidence"]))
        self.assertEqual(subject.request("BUS2", "rx"), identifier)
        self.assertEqual(counts, (len(subject.value["resources"]), len(subject.value["edges"]), len(subject.value["evidence"])))
        record = next(r for r in subject.value["resources"] if r["id"] == identifier)
        record["attributes"]["selector"] = "99"
        with self.assertRaises(SystemExit):
            subject.request("BUS2", "rx")
        record["attributes"]["selector"] = "4"
        subject.value["edges"] = [edge for edge in subject.value["edges"] if not edge["id"].endswith("routes_to.CH1")]
        with self.assertRaises(SystemExit):
            subject.request("BUS2", "rx")


class Instances(unittest.TestCase):
    def promoted(self, instance, layout_of):
        subject = domains_under_test()
        subject.sources["svd"] = source(SVD, "svd")
        subject.blocks = None
        subject.config["device_description"] = {"source": "svd", "unpromoted_kind": "bus"}
        subject.config["instances"] = [{
            "instance": instance, "kind": "serial_bus", "layout_of": layout_of, "policy": "general",
            "symbols": {"hal_request": ["high", "HAL_DMA1_REQUEST_BUS2_RX", "LL_DMA1_REQUEST_{instance}_RX"]}}]
        return subject, subject.instances()

    def test_a_symbol_must_name_the_instance_it_is_given_to(self):
        subject, promoted = self.promoted("BUS2", "BUS1")
        self.assertEqual(promoted, ["BUS2"])
        record = next(r for r in subject.value["resources"] if r["id"] == "PART.BUS2")
        self.assertEqual((record["kind"], record["attributes"]), ("serial_bus", {"hal_request": "HAL_DMA1_REQUEST_BUS2_RX"}))
        with self.assertRaises(SystemExit):
            self.promoted("BUS1", None)

    def test_the_configured_layout_must_match_the_description(self):
        with self.assertRaises(SystemExit):
            self.promoted("BUS2", None)


if __name__ == "__main__":
    unittest.main()
