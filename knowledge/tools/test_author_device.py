# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for the device authoring tools on small synthetic sources.

Run: python -B -m unittest knowledge/tools/test_author_device.py
"""
import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_domains as domains  # noqa: E402
import descriptor  # noqa: E402
import json  # noqa: E402
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
        same_line = "int F(void) {\n  return 0;\n}\nint G(void); /* note */\nint H(void);\n"
        self.assertEqual(inventory.declaration(source(same_line), "F"), (1, 3, "definition"))
        self.assertEqual(inventory.declaration(source(same_line), "G"), (4, 4, "prototype"))

    def test_a_banner_above_a_macro_is_not_its_description(self):
        text = "/* ### Module config ### */\n#define USE_MODULE 0U\n/*! Instance two */\n  SECOND = BASE2,\n"
        self.assertEqual(inventory.declaration(source(text), "USE_MODULE"), (2, 2, "value"))
        self.assertEqual(inventory.declaration(source(text), "SECOND"), (3, 4, "value"))

    def test_one_evidence_id_names_one_place(self):
        graph = inventory.Graph({"evidence": [], "resources": [], "edges": []}, "board", {})
        first, second = source("int Init(void);\n", "header"), source("int Init(void)\n{\n}\n", "body")
        graph.cite_symbol("api", first, "Init")
        graph.cite_symbol("api", first, "Init")
        with self.assertRaises(SystemExit):
            graph.cite_symbol("api", second, "Init")

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
    subject.unbonded, subject.composed, subject.narrowed = set(), False, []
    subject.config = {
        "policy": {"source": "policy", "label": "Policy", "evidence_prefix": "policy3",
                   "lines": {"functions": 1, "requests": 2, "symbols": 3, "general": 4, "allocation": 5}},
        "general_attribute": {"allocation": "general"},
        "route_marks": [{"attribute": "function", "policy": "functions", "values": functions or {
            "bus_clock": ["BUS1_CLK", "BUS2_CLK"], "bus_tx": ["BUS1_TX"]}}],
        "dma_requests": {
            "policy": "requests", "kind": "request", "channel_kind": "channel",
            "table": {"source": "table", "row": "{selector} {request}"},
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

    def service(self, **spec):
        subject = domains_under_test()
        subject.target = {"id": "board", "evidence": [], "services": [
            {"id": "startup", "properties": {"optional": "BUS1 starts only with the shared card."}, "evidence": ["kept"]}]}
        subject.config["service_restatements"] = [{
            "service": "startup", "policy": "general",
            "properties": {"optional": [["only with the shared card", "with the shared card or inside its own unit"]]},
            "cite": [{"id": "own-unit", "source": "table", "start": 1, "end": 1, "statement": "The unit starts itself."}],
            **spec}]
        return subject

    def test_a_service_is_restated_with_the_rule_and_the_lines_that_show_it(self):
        subject = self.service()
        subject.service_restatements()
        service = subject.target["services"][0]
        self.assertEqual(service["properties"]["optional"], "BUS1 starts with the shared card or inside its own unit.")
        self.assertEqual(service["evidence"], ["kept", "policy3-general", "own-unit"])
        # A service may cite only what a reader can check as a statement, never a template.
        self.assertNotEqual(domains.find(subject.value["evidence"], "own-unit").get("role"), "template")

    def test_a_service_restatement_needs_its_property_and_something_that_shows_it(self):
        too_long = {"properties": {"optional": [["only with the shared card", "x" * 250]]}}
        for spec in ({"properties": {"absent": [["a", "b"]]}}, {"cite": []}, {"service": "unknown"}, too_long):
            with self.assertRaises(SystemExit):
                self.service(**spec).service_restatements()


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
        # The policy and configuration still agree; only the SDK declaration is gone.
        absent = domains_under_test(low=LOW.replace("#define LL_DMA1_REQUEST_BUS2_RX 4U /*!< rx */\n", ""))
        absent.config["route_marks"] = subject.config["route_marks"]
        with self.assertRaises(SystemExit) as refused:
            absent.route_marks()
        self.assertIn("expected one declaration", str(refused.exception))

    def test_a_value_the_policy_defines_must_be_configured(self):
        with self.assertRaises(SystemExit) as refused:
            domains_under_test({"bus_clock": ["BUS1_CLK", "BUS2_CLK"]}).route_marks()
        self.assertIn("bus_tx", str(refused.exception))

    def test_every_configured_signal_must_have_a_route(self):
        subject = domains_under_test()
        subject.value["edges"] = [edge for edge in subject.value["edges"] if edge["attributes"]["signal"] != "BUS2_CLK"]
        with self.assertRaises(SystemExit) as refused:
            subject.route_marks()
        self.assertIn("BUS2_CLK", str(refused.exception))

    def test_a_smaller_package_reports_the_signals_it_does_not_bond(self):
        subject = domains_under_test()
        subject.composed = True
        subject.value["edges"] = [edge for edge in subject.value["edges"] if edge["attributes"]["signal"] != "BUS2_CLK"]
        self.assertEqual(subject.route_marks(), {"function=bus_clock": 1, "function=bus_tx": 1})
        self.assertEqual(subject.unbonded, {"BUS2_CLK"})


class Allocations(unittest.TestCase):
    SPEC = {"capability": "bus_clock_pin", "card": "card.bus-clock", "policy": "allocation", "function": "bus_clock",
            "instance_kind": "bus", "instance_role": "bus", "route_attributes": ["signal"],
            "pad_attribute": {"bus_clock_pad": "true"},
            "description": "Allocate.", "guidance": "Allocation only."}

    def allocated(self, eligible, prepare=None, **changes):
        subject = domains_under_test()
        subject.eligible = eligible
        subject.value["resources"] += [{"id": f"PART.{pin}", "kind": "pin", "attributes": {}, "evidence": []}
                                       for pin in ("PA1", "PA2")]
        subject.route_marks()
        if prepare:
            prepare(subject)
        return subject, subject.allocation({**self.SPEC, **changes})

    def test_only_eligible_pads_of_covered_instances_are_offered(self):
        subject, routes = self.allocated(["PA1"])
        self.assertEqual(routes, ["BUS1:PA1"])
        capability = subject.value["capabilities"][0]
        self.assertEqual(capability["links"], [{"from": "bus", "to": "pad", "relation": "signal",
                                                "attributes": {"function": "bus_clock"}}])
        self.assertEqual([role["kind"] for role in capability["roles"]], ["bus", "pin"])
        self.assertEqual(capability["roles"][1]["attributes"], {"bus_clock_pad": "true"})
        marked = {r["id"]: r["attributes"] for r in subject.value["resources"] if r["kind"] == "pin"}
        self.assertEqual(marked, {"PART.PA1": {"bus_clock_pad": "true"}, "PART.PA2": {}})
        self.assertEqual(subject.value["implementations"][0]["edits"], [])

    def test_an_excluded_pad_is_not_offered_and_must_be_named_by_the_policy(self):
        with self.assertRaises(SystemExit) as refused:
            self.allocated(["PA1", "PA2"], excluded_pads=["PA2"])
        self.assertIn("does not exclude PA2", str(refused.exception))
        subject = domains_under_test()
        policy = POLICY.replace("Allocation: authored.", "Allocation: authored. PA2 is outside this rule.")
        subject.sources["policy"] = subject.policy_source = source(policy, "policy")
        subject.eligible = ["PA1", "PA2"]
        subject.value["resources"] += [{"id": f"PART.{pin}", "kind": "pin", "attributes": {}, "evidence": []}
                                       for pin in ("PA1", "PA2")]
        subject.route_marks()
        self.assertEqual(subject.allocation({**self.SPEC, "excluded_pads": ["PA2"]}), ["BUS1:PA1"])

    def test_a_route_value_another_stage_wrote_is_never_replaced(self):
        def conflicting(subject):
            subject.value["edges"][0]["attributes"].update(alternate="AF1", alternate_symbol="OTHER")
        with self.assertRaises(SystemExit) as refused:
            self.allocated(["PA1"], prepare=conflicting,
                           alternate_symbol={"source": "low", "symbol": "LL_DMA1_REQUEST_BUS2_RX", "split": "AF"})
        self.assertIn("another alternate_symbol", str(refused.exception))

    UNIT = "void {{unit}}_init(void);\nint {{unit}}_start(void);\n"
    COMPONENT = {
        "parameters": {"rate": {"type": "choice", "values": ["1", "2"]}},
        "edits_from": {"card": "card.reviewed", "static": ["MODULE 1"]},
        "component": {"bindings": {"unit": {"source": {"type": "instance"}},
                                   "divider": {"source": {"type": "parameter", "name": "rate"}, "map": {"1": "9", "2": "4"}},
                                   "block": {"source": {"type": "resource_attribute", "role": "bus", "attribute": "symbol"}},
                                   "port": {"source": {"type": "resource_attribute", "role": "pad", "attribute": "port"}},
                                   "line": {"source": {"type": "route_attribute", "from": "bus", "to": "pad", "relation": "signal",
                                                       "attribute": "signal"}}},
                      "files": [{"path": "units/{{unit}}.h", "template": "unit-h"}, {"path": "units/{{unit}}.c", "template": "unit-c"}],
                      "claims": [], "interface": {"start": "{{unit}}_start()"}, "interface_sources": {"start": ["unit-api"]}}}

    def reviewed(self, subject):
        subject.value["implementations"].append({"id": "card.reviewed", "edits": [
            {"path": "conf.h", "expected_sha256": "c", "old": "MODULE 0", "new": "MODULE 1"},
            {"path": "conf.h", "expected_sha256": "c", "old": "CALLBACKS 0", "new": "CALLBACKS 1"}],
            "component": {"edits": [
                {"path": "CMakeLists.txt", "expected_sha256": "m", "old": "# here", "new": "  units/{{unit}}.c\n", "mode": "insert_before"},
                {"path": "main.c", "expected_sha256": "a", "old": "#include", "new": "#include \"units/{{unit}}.h\"\n", "mode": "insert_before"},
                {"path": "main.c", "expected_sha256": "a", "old": "for", "new": "    {{unit}}_init();\n", "mode": "insert_before"}]}})
        subject.sources["unit"] = source(self.UNIT, "unit")
        for identifier, (first, last) in {"unit-h": (1, 2), "unit-c": (1, 2), "unit-api": (2, 2)}.items():
            subject.graph.cite(identifier, subject.sources["unit"], first, last, "Authored.")
            domains.find(subject.value["evidence"], identifier)["role"] = "template"
        for record in subject.value["resources"]:
            record["attributes"].update({"symbol": "BUS"} if record["kind"] == "bus" else {"port": "PORT"})

    def test_a_component_shares_the_edits_of_a_reviewed_card_and_names_its_templates(self):
        subject, _ = self.allocated(["PA1"], prepare=self.reviewed, **copy.deepcopy(self.COMPONENT))
        capability, card = subject.value["capabilities"][0], subject.value["implementations"][-1]
        self.assertEqual(capability["parameters"], self.COMPONENT["parameters"])
        reviewed = subject.value["implementations"][0]
        # Only the edit that was asked for, and the very bytes of the reviewed one.
        self.assertEqual(card["edits"], [reviewed["edits"][0]])
        self.assertEqual(card["component"]["edits"], reviewed["component"]["edits"])
        self.assertIsNot(card["component"]["edits"], reviewed["component"]["edits"])
        self.assertEqual(card["component"]["files"], self.COMPONENT["component"]["files"])
        # A template is what is generated, never evidence for a claim of the card.
        self.assertFalse({"unit-h", "unit-c", "unit-api"} & set(card["evidence"]))
        self.assertNotIn("edits", self.COMPONENT["component"])

    def refused(self, message, prepare=None, **change):
        spec = copy.deepcopy(self.COMPONENT)
        for key, value in change.items():
            if key in ("bindings", "files"):
                spec["component"][key] = value
            else:
                spec[key] = value
        with self.assertRaises(SystemExit, msg=change) as refused:
            self.allocated(["PA1"], prepare=prepare or self.reviewed, **spec)
        self.assertIn(message, str(refused.exception))

    def test_a_component_must_fit_the_reviewed_edits_it_shares(self):
        def without(evidence):
            def prepare(subject):
                self.reviewed(subject)
                subject.value["evidence"] = [record for record in subject.value["evidence"] if record["id"] != evidence]
            return prepare
        self.refused("expected one edit", edits_from={"card": "card.reviewed", "static": ["MODULE 2"]})
        self.refused("expected one edit", edits_from={"card": "card.reviewed", "static": [" 1"]})
        self.refused("no template evidence ['unit-api']", prepare=without("unit-api"))
        self.refused("no template evidence ['unit-c']", prepare=without("unit-c"))
        files = self.COMPONENT["component"]["files"]
        # The shared edits compile, include and initialize the unit under one layout.
        self.refused("names units/{{unit}}.c", files=[files[0], {"path": "other/{{unit}}.c", "template": "unit-c"}])
        self.refused("names units/{{unit}}.h", files=[files[1]])
        bindings = self.COMPONENT["component"]["bindings"]
        self.refused("uses ['unit']", bindings={"name": bindings["unit"], **{k: v for k, v in bindings.items() if k != "unit"}})

        def another_api(subject):
            self.reviewed(subject)
            subject.value["implementations"][0]["component"]["edits"][2]["new"] = "    {{unit}}_begin();\n"
        self.refused("calls {{unit}}_begin", prepare=another_api)
        own = copy.deepcopy(self.COMPONENT)
        own["component"]["edits"] = []
        with self.assertRaises(SystemExit) as refused:
            self.allocated(["PA1"], prepare=self.reviewed, **own)
        self.assertIn("from a reviewed card", str(refused.exception))

    def test_a_binding_the_allocation_cannot_always_supply_is_refused(self):
        bindings = self.COMPONENT["component"]["bindings"]

        def changed(name, **change):
            return {**bindings, name: {**bindings[name], **change}}
        self.refused("does not map exactly", parameters={"rate": {"type": "choice", "values": ["1", "2", "3"]}})
        self.refused("does not have", parameters={})
        self.refused("does not have", bindings=changed("divider", source={"type": "parameter", "name": "speed"}, map={}))
        self.refused("cannot be mapped", bindings=changed("unit", map={"a": "b"}))
        self.refused("does not accept", bindings=changed("divider", map={"1": "{{other}}", "2": "4"}))
        self.refused("does not accept", bindings=changed("divider", map={"1": "", "2": "4"}))
        self.refused("a role the allocation does not have",
                     bindings=changed("block", source={"type": "resource_attribute", "role": "clock", "attribute": "symbol"}))
        self.refused("needs reset, which ['BUS1'] do not have",
                     bindings=changed("block", source={"type": "resource_attribute", "role": "bus", "attribute": "reset"}))
        self.refused("needs pull, which ['PA1'] do not have",
                     bindings=changed("port", source={"type": "resource_attribute", "role": "pad", "attribute": "pull"}))
        self.refused("a route the allocation does not have", bindings=changed("line", source={
            "type": "route_attribute", "from": "pad", "to": "bus", "relation": "signal", "attribute": "signal"}))
        self.refused("does not require of every route", bindings=changed("line", source={
            "type": "route_attribute", "from": "bus", "to": "pad", "relation": "signal", "attribute": "alternate"}))
        self.refused("unknown source", bindings=changed("unit", source={"type": "constant"}))

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
        identifier = subject.request("BUS2", "rx", "bus2_rx_dma")
        record = next(r for r in subject.value["resources"] if r["id"] == identifier)
        self.assertEqual(record["attributes"], {"peripheral": "BUS2", "direction": "rx", "selector": "4",
                                                "hal_request_dma1": "HAL_DMA1_REQUEST_BUS2_RX", "ownership_policy": "one"})
        added = [(edge["relation"], edge["to"]) for edge in subject.value["edges"] if "request" in edge["id"]]
        self.assertEqual(added, [("rx_request", identifier), ("routes_to", "PART.CH0"), ("routes_to", "PART.CH1")])

    def test_a_disagreeing_or_unrelated_definition_is_refused(self):
        with self.assertRaises(SystemExit):
            domains_under_test(low=LOW.replace("RX 4U", "RX 6U")).request("BUS2", "rx", "bus2_rx_dma")
        with self.assertRaises(SystemExit):
            domains_under_test(high=HIGH.replace("= LL_DMA1_REQUEST_BUS2_RX,", "= 4,")).request("BUS2", "rx", "bus2_rx_dma")
        with self.assertRaises(SystemExit):
            domains_under_test(table=TABLE + "9 bus2_rx_dma\n").request("BUS2", "rx", "bus2_rx_dma")

    def test_a_route_needs_its_channel_controller_checked(self):
        subject = domains_under_test()
        subject.value["resources"][1]["attributes"]["controller"] = "DMA2"
        with self.assertRaises(SystemExit):
            subject.request("BUS2", "rx", "bus2_rx_dma")

    def test_a_recorded_request_is_held_to_the_same_facts(self):
        subject = domains_under_test()
        identifier = subject.request("BUS2", "rx", "bus2_rx_dma")
        counts = (len(subject.value["resources"]), len(subject.value["edges"]), len(subject.value["evidence"]))
        self.assertEqual(subject.request("BUS2", "rx", "bus2_rx_dma"), identifier)
        self.assertEqual(counts, (len(subject.value["resources"]), len(subject.value["edges"]), len(subject.value["evidence"])))
        record = next(r for r in subject.value["resources"] if r["id"] == identifier)
        record["attributes"]["selector"] = "99"
        with self.assertRaises(SystemExit):
            subject.request("BUS2", "rx", "bus2_rx_dma")
        record["attributes"]["selector"] = "4"
        record["capacity"] = 2
        with self.assertRaises(SystemExit):
            subject.request("BUS2", "rx", "bus2_rx_dma")
        record["capacity"] = 1
        subject.value["edges"] = [edge for edge in subject.value["edges"] if not edge["id"].endswith("routes_to.CH1")]
        with self.assertRaises(SystemExit):
            subject.request("BUS2", "rx", "bus2_rx_dma")

    def test_configuration_cannot_replace_a_value_read_from_the_sources(self):
        subject = domains_under_test()
        subject.config["dma_requests"]["attributes"] = {"selector": "99"}
        with self.assertRaises(SystemExit) as refused:
            subject.request("BUS2", "rx", "bus2_rx_dma")
        self.assertIn("would replace", str(refused.exception))

    def test_a_description_above_the_definition_does_not_hide_its_selector(self):
        low = "/** rx request */\n#define LL_DMA1_REQUEST_BUS2_RX 4U\n#define LL_DMA1_REQUEST_BUS2_TX 5U /*!< tx */\n"
        high = "  /** rx */\n  HAL_DMA1_REQUEST_BUS2_RX = LL_DMA1_REQUEST_BUS2_RX,\n" + HIGH.split("\n", 1)[1]
        subject = domains_under_test(low=low, high=high)
        record = next(r for r in subject.value["resources"] if r["id"] == subject.request("BUS2", "rx", "bus2_rx_dma"))
        self.assertEqual(record["attributes"]["selector"], "4")


def peripheral(name, version=1.0, kind="bus"):
    return {"busMapping": [{"connections": ["APB1"], "mode": "slave"}], "digitalName": "ip_bus",
            "interconnect": [
                {"id": "Global", "instance": name, "ip_signal": "GLOBAL", "source": name, "type": "interrupt"},
                {"id": "RX", "instance": name, "ip_signal": "rx", "source": f"{name.lower()}_rx_dma", "type": "dma request"}],
            "name": name, "peripheralType": kind, "peripheralVersionNum": version,
            "pinout_signals": [{"id": "CLK", "name": f"{name}_CLK"}, {"id": "TX", "name": f"{name}_TX"}],
            "protocols": [{"signals": [{"signal": f"{name}_CLK", "type": "CLK"}, {"signal": f"{name}_TX", "type": "TX"}],
                           "type": "Wire"}]}


CLASS = {"kind": "serial_bus", "policy": "general",
         "ip": {"types": ["bus"], "digitalName": "ip_bus", "versions": [1.0]},
         "bus_clock": {"APB1": "PCLK1"},
         "symbols": {"hal_request": ["high", "HAL_DMA1_REQUEST_{instance}_RX", "LL_DMA1_REQUEST_{instance}_RX"]},
         "interrupt": "GLOBAL", "requests": {"rx": "rx"},
         "roles": {"policy": "roles", "protocol": "Wire", "functions": {"CLK": "bus_clock"}},
         "signal_marks": [{"id": "T(X)", "policy": "general", "attributes": {"lane": "lane-{0}"}}]}


PACK = """<subFamily DsubFamily="ONE">
  <descriptor schemaType="peripherals" path="Descriptors/peripherals/D1_peripherals.json"/>
  <descriptor schemaType="limitations" path="Descriptors/limitations/D1_limitations.json"/>
  <device Dname="PART">
    <feature type="QFP" name="QFP 48 7x7 mm" n="48"/>
    <variant Dvariant="PART-X">
      <descriptor schemaType="pinout" path="Descriptors/pinout/PART_48_pinout.json"/>
    </variant>
    <variant Dvariant="PART-Y">
      <feature type="Temp" n="-40" m="85"/>
    </variant>
  </device>
</subFamily>
<subFamily DsubFamily="TWO">
  <descriptor schemaType="peripherals" path="Descriptors/peripherals/D2_peripherals.json"/>
  <descriptor schemaType="limitations" path="Descriptors/limitations/D2_limitations.json"/>
  <device Dname="OTHER">
    <feature type="QFP" name="QFP 48 7x7 mm" n="48"/>
    <variant Dvariant="OTHER-X">
    </variant>
  </device>
</subFamily>
"""


def classes_under_test(peripherals=None, descriptor_file="D1_peripherals.json", part="PART-X", high=HIGH, cls=CLASS):
    subject = domains_under_test(high=high)
    peripherals = peripherals or [peripheral("BUS1"), peripheral("BUS2"), peripheral("BUS3", 2.0),
                                  peripheral("BUS4"), peripheral("SLOW1", kind="slow")]
    subject.sources.update(
        svd=source(SVD, "svd"),
        peripherals=source(json.dumps({"peripherals": peripherals}, indent=1, sort_keys=True), "peripherals"),
        limitations=source(json.dumps({"limitations": {"PART-X": {"removed_peripherals": ["BUS1"]},
                                                      "PART-Y": {"removed_peripherals": []},
                                                      "OTHER-X": {"removed_peripherals": []}}}, indent=1), "limitations"),
        pinout=source(json.dumps({"characteristics": {"die_name": "D1", "package_name": "QFP48"}}, indent=1), "pinout"),
        listing=source(PACK, "listing"),
        policy=source(POLICY + "Roles: in the Wire protocol CLK is bus_clock.\n", "policy"))
    subject.policy_source = subject.sources["policy"]
    subject.config["policy"]["lines"]["roles"] = 6
    subject.blocks = None
    subject.target = {"id": "board", "part": part, "package": "QFP48", "evidence": [], "reservations": []}
    subject.value["sources"] = [{"id": "peripherals", "path": "pack/" + descriptor_file},
                                {"id": "limitations", "path": "pack/D1_limitations.json"}]
    subject.value["resources"].append({"id": "PART.BUS2.irq", "kind": "interrupt", "attributes": {"irq_number": "8"}, "evidence": []})
    subject.config["device_description"] = {"source": "svd", "unpromoted_kind": "bus"}
    subject.config["peripherals"] = {"source": "peripherals", "limitations": "limitations", "pinout": "pinout",
                                     "pack_description": "listing", "die_in_path": "{die}_"}
    subject.config["instance_classes"] = [cls]
    return subject


class Descriptors(unittest.TestCase):
    def test_every_value_remembers_the_lines_it_occupies(self):
        text = '{\n "list": [\n  {\n   "name": "a }",\n   "n": 1.5\n  },\n  7\n ],\n "flag": true\n}\n'
        root = descriptor.parse(source(text))
        self.assertEqual(root.value, {"list": [{"name": "a }", "n": 1.5}, 7], "flag": True})
        self.assertEqual((root["list"].start, root["list"].end), (2, 8))
        first = root["list"][0]
        self.assertEqual((first.start, first.end, first["n"].start, first["name"].value), (3, 6, 5, "a }"))
        self.assertEqual((root["flag"].start, root.get("absent")), (9, None))

    def test_a_repeated_member_or_trailing_text_is_refused(self):
        for text in ('{"a": 1, "a": 2}', '{"a": 1} x', '{"a": ', '[01]', '[1 2]', '[1,]', '{"a": 1 "b": 2}', '{1: 2}',
                     '[NaN]', '[Infinity]', '[-Infinity]'):
            with self.assertRaises(SystemExit, msg=text):
                descriptor.parse(source(text))


class Classes(unittest.TestCase):
    def test_a_class_covers_what_the_descriptor_declares_and_reports_the_rest(self):
        subject = classes_under_test()
        promoted, skipped = subject.classes()
        self.assertEqual(promoted, ["BUS2"])
        self.assertEqual(skipped, {"BUS1": "removed for this part", "BUS3": "IP version 2.0 is not covered",
                                   "BUS4": "no signal of it is bonded in this package"})
        record = next(r for r in subject.value["resources"] if r["id"] == "PART.BUS2")
        self.assertEqual((record["kind"], record["attributes"]), ("serial_bus", {"hal_request": "HAL_DMA1_REQUEST_BUS2_RX"}))
        self.assertEqual(record["evidence"][:4], ["declared-BUS2", "declared-ip-BUS2", "declared-bus-BUS2", "described-BUS2"])
        edges = {edge["id"]: edge for edge in subject.value["edges"]}
        self.assertEqual(edges["BUS2.raises_interrupt.BUS2.irq"]["evidence"][0], "declared-interrupt-BUS2-GLOBAL")
        self.assertEqual(edges["route.BUS2_CLK.PA2"]["attributes"]["function"], "bus_clock")
        self.assertNotIn("function", edges["route.BUS1_CLK.PA1"]["attributes"])
        request = next(r for r in subject.value["resources"] if r["id"] == "PART.BUS2.rx.request")
        self.assertEqual((request["attributes"]["selector"], request["evidence"][0]), ("4", "declared-dma-request-BUS2-rx"))
        self.assertEqual(subject.unbonded, {"BUS2_TX"})

    def test_the_pack_must_tie_the_part_to_the_package_and_descriptors_in_use(self):
        with self.assertRaises(SystemExit):
            classes_under_test(part="PART-Z").classes()
        with self.assertRaises(SystemExit) as refused:
            classes_under_test(part="OTHER-X").classes()
        self.assertIn("peripherals descriptor D1_peripherals.json", str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            classes_under_test(descriptor_file="D2_peripherals.json").classes()
        self.assertIn("D2_peripherals.json", str(refused.exception))
        subject = classes_under_test()
        subject.target["package"] = "QFP64"
        with self.assertRaises(SystemExit) as refused:
            subject.classes()
        self.assertIn("QFP64", str(refused.exception))
        subject = classes_under_test()
        subject.config["peripherals"]["pinout_file"] = "PART_64_pinout.json"
        with self.assertRaises(SystemExit) as refused:
            subject.classes()
        self.assertIn("PART_64_pinout.json", str(refused.exception))

    def test_a_part_the_limitations_do_not_list_is_unknown_not_unrestricted(self):
        subject = classes_under_test()
        subject.sources["limitations"] = source(json.dumps({"limitations": {"PART-Y": {"removed_peripherals": []}}}, indent=1),
                                                "limitations")
        with self.assertRaises(SystemExit) as refused:
            subject.classes()
        self.assertIn("does not list PART-X", str(refused.exception))

    def test_a_part_without_listed_removals_keeps_every_instance(self):
        subject = classes_under_test(part="PART-Y", high=HIGH + "  HAL_DMA1_REQUEST_BUS1_RX = LL_DMA1_REQUEST_BUS1_RX, /*!< rx */\n")
        subject.value["resources"].append({"id": "PART.BUS1.irq", "kind": "interrupt", "attributes": {"irq_number": "7"}, "evidence": []})
        subject.sources["table"] = source(TABLE + "2 bus1_rx_dma\n", "table")
        subject.sources["low"] = source(LOW + "#define LL_DMA1_REQUEST_BUS1_RX 2U /*!< rx */\n", "low")
        promoted, skipped = subject.classes()
        self.assertEqual((promoted, sorted(skipped)), (["BUS1", "BUS2"], ["BUS3", "BUS4"]))

    def test_a_symbol_must_name_the_instance_it_is_given_to(self):
        with self.assertRaises(SystemExit) as refused:
            classes_under_test(high=HIGH.replace("LL_DMA1_REQUEST_BUS2_RX,", "XLL_DMA1_REQUEST_BUS2_RX,")).classes()
        self.assertIn("does not refer to", str(refused.exception))

    def test_a_role_the_policy_does_not_state_is_refused(self):
        cls = {**CLASS, "roles": {**CLASS["roles"], "functions": {"CLK": "bus_clock", "TX": "bus_data"}}}
        with self.assertRaises(SystemExit) as refused:
            classes_under_test(cls=cls).classes()
        self.assertIn("TX is bus_data", str(refused.exception))


class Internal(unittest.TestCase):
    """Capabilities without a pad, pad capabilities that generate nothing, and
    integration edits no reviewed card has written yet."""
    SPEC = {"capability": "bus_tick", "card": "card.bus-tick", "policy": "allocation", "instance_kind": "bus",
            "instance_role": "bus", "description": "Tick.", "guidance": "Generated."}

    def subject(self, raises=("BUS1", "BUS2")):
        subject = domains_under_test()
        Allocations.reviewed(Allocations(), subject)
        for name in raises:
            subject.value["resources"].append({"id": f"PART.{name}.irq", "kind": "interrupt",
                                               "attributes": {"handler": f"{name}_Handler"}, "evidence": []})
            subject.value["edges"].append({"id": f"{name}.raises", "relation": "raises_interrupt", "from": f"PART.{name}",
                                           "to": f"PART.{name}.irq", "evidence": []})
        return subject

    def component(self, **bindings):
        return {"edits_from": {"card": "card.reviewed", "static": ["MODULE 1"]},
                "component": {"bindings": {"unit": {"source": {"type": "instance"}}, **bindings},
                              "files": [{"path": "units/{{unit}}.h", "template": "unit-h"},
                                        {"path": "units/{{unit}}.c", "template": "unit-c"}],
                              "claims": [], "interface": {"start": "{{unit}}_start()"}}}

    def test_a_capability_without_a_pad_takes_an_instance_and_its_one_interrupt(self):
        subject = self.subject()
        handler = {"handler": {"source": {"type": "resource_attribute", "role": "interrupt", "attribute": "handler"}}}
        offered = subject.instance_capability({**self.SPEC, "interrupt_role": "interrupt", **self.component(**handler)})
        self.assertEqual(offered, ["BUS1", "BUS2"])
        capability = subject.value["capabilities"][0]
        self.assertEqual([(role["id"], role["kind"]) for role in capability["roles"]], [("bus", "bus"), ("interrupt", "interrupt")])
        self.assertEqual(capability["links"], [{"from": "bus", "to": "interrupt", "relation": "raises_interrupt"}])
        self.assertIn("policy3-allocation", domains.find(subject.value["resources"], "PART.BUS1")["evidence"])

    def test_an_instance_must_raise_exactly_one_recorded_interrupt(self):
        with self.assertRaises(SystemExit) as refused:
            self.subject(raises=("BUS1",)).instance_capability({**self.SPEC, "interrupt_role": "interrupt", **self.component()})
        self.assertIn("BUS2 raises 0 recorded interrupts", str(refused.exception))

    def test_a_capability_without_a_pad_cannot_read_a_pad_or_a_route(self):
        port = {"port": {"source": {"type": "resource_attribute", "role": "pad", "attribute": "port"}}}
        with self.assertRaises(SystemExit) as refused:
            self.subject().instance_capability({**self.SPEC, **self.component(**port)})
        self.assertIn("a role the allocation does not have", str(refused.exception))
        line = {"line": {"source": {"type": "route_attribute", "from": "bus", "to": "pad", "relation": "signal", "attribute": "signal"}}}
        with self.assertRaises(SystemExit) as refused:
            self.subject().instance_capability({**self.SPEC, **self.component(**line)})
        self.assertIn("a route the allocation does not have", str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            self.subject().instance_capability({**self.SPEC, "route_attributes": ["signal"], **self.component()})
        self.assertIn("has no route to read", str(refused.exception))

    def test_a_reviewed_component_is_taken_whole_for_another_class(self):
        subject = self.subject()
        reviewed = subject.value["implementations"][0]
        reviewed["component"].update(bindings={"unit": {"source": {"type": "instance"}},
                                               "block": {"source": {"type": "resource_attribute", "role": "bus", "attribute": "symbol"}}},
                                     files=[], claims=[], interface={})
        subject.value["capabilities"].append({"id": "reviewed_tick", "parameters": {"rate": {"type": "choice", "values": ["1"]}}})
        like = {"like": {"capability": "reviewed_tick", "card": "card.reviewed"}}
        subject.instance_capability({**self.SPEC, **like})
        capability, card = subject.value["capabilities"][-1], subject.value["implementations"][-1]
        self.assertEqual(capability["parameters"], {"rate": {"type": "choice", "values": ["1"]}})
        self.assertEqual((card["component"], card["edits"]), (reviewed["component"], reviewed["edits"]))
        self.assertIsNot(card["component"], reviewed["component"])
        with self.assertRaises(SystemExit) as refused:
            self.subject().instance_capability({**self.SPEC, **like, "parameters": {}})
        self.assertIn("takes its component and parameters from it", str(refused.exception))
        other = self.subject()
        other.value["implementations"][0]["component"]["bindings"] = {
            "reset": {"source": {"type": "resource_attribute", "role": "bus", "attribute": "reset"}}}
        other.value["capabilities"].append({"id": "reviewed_tick", "parameters": {}})
        with self.assertRaises(SystemExit) as refused:
            other.instance_capability({**self.SPEC, **like})
        self.assertIn("needs reset, which ['BUS1', 'BUS2'] do not have", str(refused.exception))

    EDIT = {"path": "conf.h", "source": "conf", "mode": "replace", "old": "OTHER 0", "new": "OTHER 1"}

    def edited(self, text="MODULE 0\nOTHER 0\n", pinned=None, **change):
        subject = self.subject()
        conf = source(text, "conf")
        subject.sources["conf"] = conf
        subject.value["sources"] = [{"id": "conf", "sha256": inventory.sha256(text.encode("utf-8"))}]
        for edit in subject.value["implementations"][0]["edits"]:
            edit["expected_sha256"] = pinned or subject.value["sources"][0]["sha256"]
        return subject, subject.static_edits({"card": "card.new", "static_edits": [{**self.EDIT, **change}]})

    def test_an_authored_edit_is_pinned_to_the_file_the_reviewed_edits_change(self):
        subject, edits = self.edited()
        self.assertEqual(edits, [{"path": "conf.h", "expected_sha256": subject.value["sources"][0]["sha256"],
                                  "old": "OTHER 0", "new": "OTHER 1", "mode": "replace"}])
        for message, change in (("no single reviewed hash for other.h", {"path": "other.h"}),
                                ("does not hold the text to replace exactly once", {"old": "ABSENT 0"}),
                                ("unknown edit mode", {"mode": "append"})):
            with self.assertRaises(SystemExit, msg=message) as refused:
                self.edited(**change)
            self.assertIn(message, str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            self.edited(text="OTHER 0\nOTHER 0\n")
        self.assertIn("exactly once", str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            self.edited(pinned="another file")
        self.assertIn("is not the file the reviewed edits of conf.h are pinned to", str(refused.exception))

    def test_a_pad_capability_offers_every_eligible_pad_and_generates_nothing(self):
        subject = domains_under_test()
        subject.eligible = ["PA1", "PA2"]
        subject.value["resources"] += [{"id": f"PART.{pin}", "kind": "pin", "attributes": {}, "evidence": []}
                                       for pin in ("PA1", "PA2", "PA3")]
        spec = {"capability": "level_input", "card": "card.level-input", "policy": "allocation",
                "pad_attribute": {"reads_level": "true"}, "description": "Read.", "guidance": "Read it."}
        self.assertEqual(subject.pad_capability(spec), 2)
        capability, card = subject.value["capabilities"][0], subject.value["implementations"][0]
        self.assertEqual(capability["parameters"], {"pin": {"type": "choice", "values": ["PA1", "PA2"]}})
        self.assertEqual(capability["roles"], [{"id": "pin", "kind": "pin", "attributes": {"reads_level": "true"},
                                                "parameter_attributes": {"pin": "pin"}, "count": 1}])
        self.assertEqual((card["edits"], "component" in card), ([], False))
        marked = {record["id"] for record in subject.value["resources"] if record["attributes"].get("reads_level")}
        self.assertEqual(marked, {"PART.PA1", "PART.PA2"})
        with self.assertRaises(SystemExit):
            subject.pad_capability(spec)
        empty = domains_under_test()
        empty.eligible = []
        with self.assertRaises(SystemExit):
            empty.pad_capability(spec)

    def test_a_later_graph_schema_raises_the_bounds_for_one_run_only(self):
        graph = {"schema": 4}
        inventory.use_schema(graph, 5)
        self.assertEqual((graph["schema"], inventory.LIMITS["sources"], inventory.LIMITS["citations"]), (5, 1024, 128))
        inventory.use_schema({"schema": 4}, None)
        self.assertEqual((inventory.LIMITS["sources"], inventory.LIMITS["citations"]), (128, 64))
        for wanted in (3, 6):
            with self.assertRaises(SystemExit):
                inventory.use_schema({"schema": 4}, wanted)
        with self.assertRaises(SystemExit):
            inventory.use_schema({"schema": 5}, 4)
        inventory.use_schema({"schema": 4}, None)


class Citations(unittest.TestCase):
    """Evidence a record should carry and a reviewed base did not give it."""

    def test_an_interrupt_cites_the_enum_and_vector_lines_the_inventory_holds(self):
        subject = domains_under_test()
        subject.value["resources"] += [
            {"id": "PART.FIRST.irq", "kind": "interrupt", "attributes": {}, "evidence": ["reviewed"]},
            {"id": "PART.SECOND.irq", "kind": "interrupt", "attributes": {}, "evidence": ["inv-irq-SECOND"]},
            {"id": "PART.THIRD.irq", "kind": "interrupt", "attributes": {}, "evidence": ["reviewed"]}]
        for identifier in ("inv-irq-FIRST", "inv-vector-FIRST", "inv-irq-SECOND"):
            subject.graph.cite(identifier, subject.sources["table"], 1, 1, "Transcribed.")
        self.assertEqual(subject.vectors(), 1)
        cited = {record["id"]: record["evidence"] for record in subject.value["resources"] if record["kind"] == "interrupt"}
        self.assertEqual(cited, {"PART.FIRST.irq": ["reviewed", "inv-irq-FIRST", "inv-vector-FIRST"],
                                 "PART.SECOND.irq": ["inv-irq-SECOND"], "PART.THIRD.irq": ["reviewed"]})

    def test_a_service_is_given_evidence_the_graph_holds_or_a_rule(self):
        def given(**spec):
            subject = domains_under_test()
            subject.target = {"services": [{"id": "clock", "evidence": ["reviewed"]}]}
            subject.graph.cite("startup", subject.sources["table"], 1, 1, "Start-up.")
            subject.config["service_evidence"] = [{"service": "clock", **spec}]
            subject.service_evidence()
            return subject.target["services"][0]["evidence"]
        self.assertEqual(given(evidence=["startup"], policy=["general"]), ["reviewed", "startup", "policy3-general"])
        for message, spec in (("no evidence ['absent']", {"evidence": ["absent"]}), ("nothing to add", {}),
                              ("nothing to add", {"evidence": []})):
            with self.assertRaises(SystemExit, msg=spec) as refused:
                given(**spec)
            self.assertIn(message, str(refused.exception))


SDK_C = """/**
  * @brief Start the unit.
  * @note  Only while the gate is off.
  */
hal_status_t HAL_UNIT_Start(void)
{
  return HAL_OK;
}
"""
SDK_H = "hal_status_t HAL_UNIT_Start(void);\n#define USE_UNIT_MODULE      0U\n"


class Scopes(unittest.TestCase):
    """What a source is about is declared, and what a citation calls its source is true."""

    def subject(self, schema=5, **config):
        subject = domains_under_test()
        subject.value.update(schema=schema, sources=[
            {"id": "policy", "authority": "BKPT engineering policy", "revision": "candidate v1"},
            {"id": "table", "authority": "STMicroelectronics", "revision": "r"}])
        subject.target = {"evidence": []}
        subject.config.update(config)
        subject.declared_scopes = dict(config.get("base_source_scopes", {}))
        return subject

    def test_every_source_of_a_schema_5_graph_declares_one_of_the_scopes(self):
        subject = self.subject(base_source_scopes={"policy": "policy", "table": "device"},
                               source_authorities={"table": "manufacturer_document_text_extraction"},
                               source_revisions={"table": "RM1 Rev 1, page 2"})
        self.assertEqual(subject.source_metadata(), {"policy": 1, "device": 1})
        self.assertEqual(subject.value["sources"][1], {"id": "table", "authority": "manufacturer_document_text_extraction",
                                                       "revision": "RM1 Rev 1, page 2", "scope": "device"})
        self.assertEqual(subject.value["sources"][0]["authority"], "BKPT engineering policy")
        for scopes, message in (({"policy": "policy"}, "source table declares no scope"),
                                ({"policy": "policy", "table": "chip"}, "source table declares no scope")):
            with self.assertRaises(SystemExit) as refused:
                self.subject(base_source_scopes=scopes).source_metadata()
            self.assertIn(message, str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            self.subject(base_source_scopes={"policy": "policy", "table": "device"}, source_revisions={"table": " "}).source_metadata()
        self.assertIn("empty or oversized", str(refused.exception))

    def test_an_older_graph_schema_takes_no_scope(self):
        self.assertEqual(self.subject(schema=4).source_metadata(), {})
        with self.assertRaises(SystemExit) as refused:
            self.subject(schema=4, base_source_scopes={"policy": "policy"}).source_metadata()
        self.assertIn("need graph schema 5", str(refused.exception))

    def test_a_symbol_citation_names_the_class_of_its_source_and_a_macro_value(self):
        subject = domains_under_test()
        header = source(SDK_H, "conf")
        self.assertIn("SDK source declares USE_UNIT_MODULE", subject.graph.cite_symbol("a", header, "USE_UNIT_MODULE") and
                      subject.value["evidence"][-1]["statement"])
        subject.graph.labels = {"conf": "Captured configuration"}
        subject.graph.cite_symbol("b", header, "USE_UNIT_MODULE")
        self.assertEqual(subject.value["evidence"][-1]["statement"], "Captured configuration defines USE_UNIT_MODULE as 0U on this line.")
        subject.graph.cite_symbol("b", header, "HAL_UNIT_Start")
        self.assertEqual(subject.value["evidence"][-1]["statement"], "Captured configuration declares HAL_UNIT_Start in these lines.")

    def test_a_unit_must_cite_the_definition_of_every_sdk_function_it_calls(self):
        subject = domains_under_test()
        subject.sources.update(body=source(SDK_C, "body"), head=source(SDK_H, "head"))
        unit = "  if (HAL_UNIT_Start() != HAL_OK) { return; }\n  /* HAL_OK is not a call */\n"
        subject.definitions({"card": "card.unit", "symbols": [["head", "HAL_UNIT_Start"], ["body", "HAL_UNIT_Start", "definition"]]}, unit)
        for symbols, message in (([["head", "HAL_UNIT_Start"]], "must cite its definition, not only a declaration"),
                                 ([], "must cite its definition, not only the call")):
            with self.assertRaises(SystemExit) as refused:
                subject.definitions({"card": "card.unit", "symbols": symbols}, unit)
            self.assertIn(message, str(refused.exception))

    def test_a_symbol_marked_as_a_definition_is_cited_under_its_own_identifier(self):
        subject = domains_under_test()
        subject.sources.update(body=source(SDK_C, "body"), head=source(SDK_H, "head"))
        cited = subject.cited_symbols({"card": "card.unit", "symbols": [["head", "HAL_UNIT_Start"], ["body", "HAL_UNIT_Start", "definition"]]})
        self.assertEqual(cited, ["inv-api-HAL_UNIT_Start", "inv-def-HAL_UNIT_Start"])
        whole = domains.find(subject.value["evidence"], "inv-def-HAL_UNIT_Start")
        # From the documentation comment, where the precondition is, to the end of the body.
        self.assertEqual((whole["start_line"], whole["end_line"]), (1, 8))
        for symbols, message in (([["head", "HAL_UNIT_Start", "definition"]], "marked as a definition and head holds none"),
                                 ([["body", "HAL_UNIT_Start", "body"]], "unknown mark")):
            with self.assertRaises(SystemExit) as refused:
                subject.cited_symbols({"card": "card.unit", "symbols": symbols})
            self.assertIn(message, str(refused.exception))

    def test_a_sentence_that_names_a_function_is_not_its_declaration(self):
        prose = "/* This subsection provides a HAL_UNIT_Start() function to start it. */\n" + SDK_C
        self.assertEqual(inventory.declaration(source(prose, "body"), "HAL_UNIT_Start"), (2, 9, "definition"))

    def test_a_reviewed_description_is_restated_by_exact_replacement(self):
        subject = domains_under_test()
        subject.value["capabilities"].append({"id": "copy", "description": "Copy with release proof.", "evidence": []})
        subject.config["capability_restatements"] = [{"capability": "copy", "description": [["release proof", "a read-back check"]]}]
        subject.reworded()
        self.assertEqual(subject.value["capabilities"][0], {"id": "copy", "description": "Copy with a read-back check.", "evidence": []})
        subject.config["capability_restatements"][0]["description"] = [["absent", "x"]]
        with self.assertRaises(SystemExit):
            subject.reworded()


if __name__ == "__main__":
    unittest.main()
