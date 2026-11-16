# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for the clock stage on small synthetic sources.

Run: python -B -m unittest knowledge/tools/test_author_clocks.py
"""
import copy
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_clocks as clocks  # noqa: E402
import author_device_inventory as inventory  # noqa: E402

# 400 choices of which one is listed twice: longer than one citation may be.
MANY = [{"const": value} for value in [*range(1, 201), 200, *range(201, 401)]]
DESCRIPTOR = {"tree": {"elements": [
    {"id": "XTAL", "type": "variableSource", "default": 8000000, "value": 8000000, "min": 4000000, "max": 50000000,
     "outputTargets": ["SYNTH"], "role": "HSE", "block": "OSC", "outputClockId": "xtal_ck"},
    {"id": "SYNTH", "type": "synthesizer", "default": 24000000, "value": 24000000, "outputTargets": ["SYS_MUX"],
     "oneOf": [{"const": 24000000}, {"const": 49000000}], "role": "PLL", "block": "OSC", "outputClockId": "synth_ck"},
    {"id": "RC", "type": "fixedSource", "default": "48000000", "value": 48000000, "outputTargets": ["SYS_MUX", "RC_DIV", "LONG_DIV"],
     "role": "HSI", "block": "OSC", "outputClockId": "rc_ck"},
    {"id": "SLOW", "type": "variableSource", "default": 32000, "value": 32000, "min": 31000, "max": 33000,
     "outputTargets": ["WDT"], "role": "LSI", "block": "OSC", "outputClockId": "slow_ck"},
    {"id": "WDT", "type": "editableValue", "default": 32000, "value": 32000, "outputTargets": [],
     "clocked_instance": ["WDT1"], "role": "", "block": "", "outputClockId": "wdt_ck"},
    {"id": "RC_DIV", "type": "divider", "default": 3, "value": 3, "oneOf": [{"const": 3}], "outputTargets": ["UART_MUX"],
     "role": "", "block": "", "outputClockId": "rc_div_ck"},
    {"id": "LONG_DIV", "type": "divider", "default": 1, "value": 1, "outputTargets": [], "oneOf": MANY,
     "role": "", "block": "", "outputClockId": "long_ck"},
    {"id": "SYS_MUX", "type": "multiplexor", "default": "RC_in", "value": 0, "outputTargets": ["SYS"],
     "possible_Input": [{"label": "SYNTH", "input_Id": "SYNTH_in", "from": "SYNTH"}, {"label": "RC", "input_Id": "RC_in", "from": "RC"}],
     "role": "", "block": "", "outputClockId": "sys_mux_ck"},
    {"id": "SYS", "type": "editableValue", "default": 48000000, "value": 48000000, "max": 48000000,
     "outputTargets": ["BUS_DIV", "CORE"], "role": "Sysclk", "block": "", "outputClockId": "sys_ck"},
    {"id": "CORE", "type": "editableValue", "default": 48000000, "value": 48000000, "outputTargets": [],
     "clocked_instance": ["CPU"], "role": "", "block": "", "outputClockId": "core_ck"},
    {"id": "BUS_DIV", "type": "divider", "default": 1, "value": 1, "oneOf": [{"const": 1}, {"const": 2}, {"const": 4}],
     "outputTargets": ["BUS"], "role": "PCLK", "block": "", "outputClockId": "bus_div_ck"},
    {"id": "BUS", "type": "editableValue", "default": 48000000, "value": 48000000, "outputTargets": ["UART_MUX"],
     "clocked_instance": ["TIMER1"], "role": "PCLKTIM", "block": "", "outputClockId": "bus_ck"},
    {"id": "UART_MUX", "type": "multiplexor", "default": "BUS_in", "value": 0, "outputTargets": [],
     "clocked_instance": ["UART1", "UART2"],
     "possible_Input": [{"label": "PCLK", "input_Id": "BUS_in", "from": "BUS"}, {"label": "RCDIV", "input_Id": "RC_DIV_in", "from": "RC_DIV"}],
     "role": "", "block": "", "outputClockId": "uart_mux_ck"},
    {"id": "UART_CLK", "type": "editableValue", "default": 48000000, "value": 48000000, "outputTargets": [],
     "clocked_instance": ["UART1"], "role": "", "block": "", "outputClockId": "uart_ck"},
], "transitions": [
    {"id": "t1", "sourceTaskId": "UART_MUX_output", "targetTaskId": "UART_CLK_input"},
    {"id": "t2", "sourceTaskId": "SYNTH_output", "targetTaskId": "SYS_MUX__SYNTH_input"},
]}}

HEADER = """#ifndef SDK_RCC_H
#define SDK_RCC_H
typedef enum
{
  SDK_RCC_UART_CLK_SRC_NONE = 9, /*!< no clock */
  SDK_RCC_UART_CLK_SRC_PCLK = 0, /*!< bus clock */
  SDK_RCC_UART_CLK_SRC_RCDIV = 1, /*!< divided oscillator */
} sdk_rcc_uart_clk_src_t;

/**
  * Enables the UART1 clock.
  */
__STATIC_INLINE void SDK_RCC_UART1_EnableClock(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART1);
}

/**
  * Resets UART1.
  */
__STATIC_INLINE void SDK_RCC_UART1_Reset(void)
{
  LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_UART1);
}

#if defined(UART9)
/**
  * Enables the UART9 clock.
  */
__STATIC_INLINE void SDK_RCC_UART9_EnableClock(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART9);
}

#endif /* UART9 */
#if defined(OPTION_VALUE)
/**
  * Enables the OPT clock.
  */
__STATIC_INLINE void SDK_RCC_OPT_EnableClock(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_OPT);
}

#endif /* OPTION_VALUE */
#define SDK_RCC_SYNTH_OUT_24MHZ 1
#define SDK_RCC_SYNTH_OUT_49MHZ 2
#define SDK_RCC_SYS_SRC_SYNTH 3
#define SDK_RCC_BUS_PRESCALER2 4
#define SDK_RCC_SLOW_32KHZ 5
int SDK_RCC_UART_SetKernelClkSource(sdk_rcc_uart_clk_src_t source);
unsigned SDK_RCC_UART_GetKernelClkFreq(void);
#endif
"""
BODY = """#if defined(USE_RCC) && (USE_RCC == 1)
/**
  * Selects the UART kernel clock.
  */
int SDK_RCC_UART_SetKernelClkSource(sdk_rcc_uart_clk_src_t source)
{
  return 0;
}
#endif
"""
BUS = "#define LL_APB1_GRP1_PERIPH_UART1 RCC_APB1ENR_UART1EN\n#if defined(UART9)\n#define LL_APB1_GRP1_PERIPH_UART9 RCC_APB1ENR_UART9EN\n#endif\n"
DEVICE = "#define UART1 ((void *)1)\n#define TIMER1 ((void *)2)\n#define SLOW_VALUE 32000\n"
CONF = "#define USE_RCC 1U\n#define XTAL_VALUE 24000000UL\n"
CAPTURED = ("/* SYSCLK(Hz) = 24000000 */\nsynth_out = SDK_RCC_SYNTH_OUT_24MHZ;\nselect(SDK_RCC_SYS_SRC_SYNTH);\n"
            "bus_prescaler = SDK_RCC_BUS_PRESCALER2;\nother_prescaler = SDK_RCC_BUS_PRESCALER2;\n"
            "SDK_RCC_UART_SetKernelClkSource(SDK_RCC_UART_CLK_SRC_PCLK);\n")
UNIT = "// unit\nvoid {{unit}}_init(void)\n{\n  (void){{clock_api}}({{clock_source}});\n}\n"
MANUAL = ("Bit 3 UART1EN: UART1 and UART2 clock enable\nEach peripheral clock can be enabled by its enable bit.\n"
          "The clock is active two cycles later.\n")
MAPPING = {"configurable_elements": [
    {"name": "UART1", "docLink": {"RM": "RM1", "Chapter": "7"}, "configurableElements": [{"name": "TIMER1"}, {"name": "UART8"}]},
    {"name": "TWICE", "docLink": {}}, {"name": "TWICE", "docLink": {}}]}
POLICY = "\n".join(f"{name}: text." for name in (
    "Head", "Scope", "Clock nodes", "Peripheral clocks", "Kernel clocks", "Instance links", "Shared clocks", "Profile",
    "Component selections", "Rate basis", "Timer clock", "Chapter pointers", "Nominal sources")) + "\n"
FILES = {"clock.json": json.dumps(DESCRIPTOR, indent=2) + "\n", "rcc.h": HEADER, "rcc.c": BODY, "bus.h": BUS, "device.h": DEVICE,
         "conf.h": CONF, "captured.c": CAPTURED, "policy.txt": POLICY, "unit.c.in": UNIT, "manual.txt": MANUAL,
         "mapping.json": json.dumps(MAPPING, indent=2) + "\n"}
CONDITIONS = {"mode": "run"}
ROLES = [{"id": "port", "kind": "serial", "attributes": {"instance": "UART1"}, "parameter_attributes": {}, "count": 1}]
CONFIG = {
    "sources": [{"id": "policy", "path": "policy.txt", "revision": "1", "authority": "bkpt_policy", "scope": "policy"}],
    "policy": {"source": "policy", "label": "Policy", "evidence_prefix": "p",
               "lines": {"clock_nodes": 3, "peripheral_clocks": 4, "kernel_clocks": 5, "instance_links": 6, "shared_clocks": 7, "profile": 8,
                         "component_selections": 9, "rate_basis": 10, "timer_clock": 11, "chapter_pointers": 12, "nominal_sources": 13}},
    "own_kinds": ["clock_node", "peripheral_clock", "kernel_clock", "clock_assignment"],
    "source_types": ["variableSource", "fixedSource", "synthesizer"], "fixed_source_types": ["fixedSource"],
    "shared_types": ["multiplexor", "divider"],
    "descriptor": {"source": "clock", "kind": "clock_node", "relation": "clock_input", "clocks_relation": "clocks"},
    "sdk": {"header": "rcc-h", "definitions": "rcc-c", "bus_header": "bus", "device_header": "device", "function_prefix": "SDK_RCC_",
            "undetermined": ["OPTION_VALUE"],
            "settled": [{"test": "defined(USE_RCC) && (USE_RCC == 1)", "verdict": True, "source": "conf", "macro": "USE_RCC", "value": "1U",
                         "label": "Configuration"}]},
    "mapping": {"source": "mapping", "withheld_chapters": {}},
    "gates": {"kind": "peripheral_clock", "relation": "gates", "resource_attribute": "clock_enable_api", "unpromoted_kind": "unpromoted",
              "functions": {"enable_api": "SDK_RCC_{name}_EnableClock", "reset_api": "SDK_RCC_{name}_Reset"},
              "stated_links": {"UART1": [{"source": "manual", "pattern": r"^Bit 3 UART1EN", "instances": ["UART1"], "label": "Manual."}]}},
    "kernel_clocks": {"kind": "kernel_clock", "relation": "kernel_clock_selector",
                      "functions": {"set_source_api": "SDK_RCC_{name}_SetKernelClkSource", "frequency_api": "SDK_RCC_{name}_GetKernelClkFreq"},
                      "selectors": {"UART": {"element": "UART_MUX", "prefix": "SDK_RCC_UART_CLK_SRC_", "without_input": ["NONE"]}},
                      "selectors_without_setter": {"SYS_MUX": "set elsewhere"}},
    "profile": {"id": "xtal24", "kind": "clock_assignment", "relation": "assigns", "component_attribute": "kernel_clock_source",
                "component_api_attribute": "kernel_clock_api", "system_clock": "SYS",
                "gating_cite": ["manual", r"^Each peripheral clock can be enabled", "Manual on enable bits.", 1],
                "selections": [
                    {"element": "XTAL", "value": 24000000, "field": "XTAL_VALUE",
                     "cite": [["conf", r"^#define XTAL_VALUE\b", "Configuration states the crystal."]],
                     "support": [["conf", r"^#define USE_RCC\b", "The module is on."]]},
                    {"element": "SYNTH", "value": 24000000, "field": "synth_out", "symbol": "SDK_RCC_SYNTH_OUT_24MHZ",
                     "cite": [["captured", r"^synth_out = ", "Captured output."]]},
                    {"element": "SYS_MUX", "input": "SYNTH", "field": "select", "symbol": "SDK_RCC_SYS_SRC_SYNTH",
                     "cite": [["captured", r"^select\(", "Captured selection."]]},
                    {"element": "BUS_DIV", "value": 2, "field": "bus_prescaler", "symbol": "SDK_RCC_BUS_PRESCALER2",
                     "cite": [["captured", r"^bus_prescaler = ", "Captured prescaler."]]},
                    {"element": "SLOW", "value": 32000, "basis": "nominal", "field": "SLOW_VALUE",
                     "cite": [["device", r"^#define SLOW_VALUE\b", "Typical value."]]}],
                "expect": {"SYS": {"source": "captured", "pattern": r"SYSCLK\(Hz\) = 24000000", "label": "Captured value."}}},
    "rate_basis": [{"capabilities": ["serial"], "elements": ["UART_CLK"], "divides": "baud_rate"}],
    "service": {"id": "clock-profile", "kind": "clock_profile", "description": "The profile.",
                "frequencies": {"sysclk_hz": "SYS", "bus_hz": "BUS"}, "properties": {"records": "{prefix}.rcc.NAME under {profile}"}},
    "timer_clock": {"elements": ["BUS"], "cite": [["captured", r"^bus_prescaler = ", "Captured prescaler."]], "requires": {"BUS_DIV": "2"}, "role_suffix": "TIM"},
    "assumptions": ["One profile is assigned."],
}
TARGET = {"resource_prefix": "PART", "conditions": CONDITIONS, "target": "T"}


class Stage(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.files = dict(FILES)
        self.config = copy.deepcopy(CONFIG)
        self.descriptor = copy.deepcopy(DESCRIPTOR)
        self.listed = ["card"]
        self.roles = ROLES
        self.kernel_source = True

    def graph(self, baud=("9600", "19200")):
        self.files["clock.json"] = json.dumps(self.descriptor, indent=2) + "\n"
        for name, text in self.files.items():
            (self.root / name).write_bytes(text.encode("utf-8"))

        def pinned(identifier, name, scope):
            return {"id": identifier, "path": name, "sha256": inventory.sha256(self.files[name].encode("utf-8")), "revision": "1",
                    "authority": "manufacturer", "scope": scope}

        unit = inventory.Source(pinned("unit-c", "unit.c.in", "implementation"), self.root)
        value = {
            "schema": 5, "id": "synthetic",
            "sources": [pinned("clock", "clock.json", "device"), pinned("rcc-h", "rcc.h", "sdk"), pinned("rcc-c", "rcc.c", "sdk"),
                        pinned("bus", "bus.h", "sdk"), pinned("device", "device.h", "device"), pinned("conf", "conf.h", "sdk"),
                        pinned("captured", "captured.c", "sdk"), pinned("unit-c", "unit.c.in", "implementation"),
                        pinned("manual", "manual.txt", "device"), pinned("mapping", "mapping.json", "device")],
            "evidence": [{"id": "base", "source": "device", "start_line": 1, "end_line": 1, "excerpt_sha256": "0" * 64,
                          "statement": "Base.", "targets": ["T"]},
                         {"id": "unit-template", "source": "unit-c", "start_line": 1, "end_line": len(unit.lines),
                          "excerpt_sha256": inventory.sha256(unit.excerpt(1, len(unit.lines)).encode("utf-8")),
                          "statement": "Template.", "targets": ["T"], "role": "template"}],
            "targets": [{"id": "T", "context": {"clock_profile": "xtal24"}, "assumptions": [], "evidence": ["base"], "services": []}],
            "resources": [
                {"id": "PART.UART1", "target": "T", "kind": "serial", "capacity": 1, "conditions": CONDITIONS, "evidence": ["base"],
                 "implementation": None,
                 "attributes": {"instance": "UART1", "clock_enable_api": "SDK_RCC_UART1_EnableClock",
                                "kernel_clock_api": "SDK_RCC_UART_SetKernelClkSource",
                                "kernel_clock_source": "SDK_RCC_UART_CLK_SRC_PCLK"}},
                {"id": "PART.TIMER1", "target": "T", "kind": "unpromoted", "capacity": 1, "conditions": CONDITIONS, "evidence": ["base"],
                 "implementation": None, "attributes": {"instance": "TIMER1"}}],
            "edges": [],
            "implementations": [{"id": "card", "target": "T", "guidance": "Use it.", "evidence": ["base"], "edits": [], "component": {
                "bindings": {"unit": {"source": {"type": "instance"}},
                             "clock_api": {"source": {"type": "resource_attribute", "role": "port", "attribute": "kernel_clock_api"}},
                             "clock_source": {"source": {"type": "resource_attribute", "role": "port", "attribute": "kernel_clock_source"}}},
                "files": [{"path": "units/{{unit}}.c", "template": "unit-template"}], "edits": [], "claims": [], "interface": {}}},
                {"id": "startup", "target": "T", "guidance": "Calls the captured initialization.", "evidence": ["base"], "edits": []}],
            "capabilities": [{"id": "serial", "target": "T", "evidence": ["base"], "roles": copy.deepcopy(self.roles),
                              "implementations": list(self.listed),
                              "parameters": {"baud_rate": {"type": "choice", "values": list(baud)}}},
                             {"id": "console", "target": "T", "evidence": ["base"], "roles": [], "implementations": ["startup"],
                              "parameters": {}}],
        }
        if not self.kernel_source:
            del value["resources"][0]["attributes"]["kernel_clock_source"]
        inventory.use_schema(value, 5)
        return inventory.Graph(value, "T", {})

    def run_stage(self, **arguments):
        graph = self.graph(**arguments)
        summary = clocks.run(graph, {}, {}, TARGET, self.config, self.root)
        return graph.value, summary["clocks"]

    def refused(self, text, **arguments):
        with self.assertRaises(SystemExit) as refusal:
            self.run_stage(**arguments)
        self.assertIn(text, str(refusal.exception))

    def test_the_tree_the_sdk_records_and_the_profile(self):
        value, summary = self.run_stage()
        records = {record["id"]: record for record in value["resources"]}
        # One node per element, with what the descriptor says can be configured.
        self.assertEqual(summary["clock_tree"]["elements"], 14)
        self.assertEqual(records["PART.clock.BUS_DIV"]["attributes"]["choices"], "1,2,4")
        self.assertEqual(records["PART.clock.RC"]["attributes"]["value_hz"], "48000000")
        self.assertEqual(records["PART.clock.UART_MUX"]["attributes"]["shared_by"], "UART1,UART2")
        edges = {edge["id"]: edge for edge in value["edges"]}
        self.assertEqual(edges["clock.SYNTH.SYS_MUX"]["attributes"], {"input": "SYNTH"})
        # A connection the descriptor only draws is an input too, and is reported.
        self.assertIn("clock.UART_MUX.UART_CLK", edges)
        self.assertEqual(summary["clock_tree"]["connections_only_drawn"], ["UART_MUX->UART_CLK"])
        self.assertIn("clock.BUS.clocks.TIMER1", edges)
        # Only what is compiled for the part: UART9 is dropped, OPT is undecided and left out.
        self.assertEqual(sorted(record["attributes"]["name"] for record in value["resources"] if record["kind"] == "peripheral_clock"), ["UART1"])
        self.assertEqual(summary["sdk"]["functions_under_an_undecided_condition"], ["SDK_RCC_OPT_EnableClock"])
        gate = records["PART.rcc.UART1"]["attributes"]
        self.assertEqual((gate["bus"], gate["bus_group"], gate["enable_field"], gate["reset_api"]),
                         ("APB1", "LL_APB1_GRP1", "RCC_APB1ENR_UART1EN", "SDK_RCC_UART1_Reset"))
        # Linked by its reviewed enable function and by the manual's line; by the mapping to the member the part defines.
        self.assertEqual(gate["instances"], "TIMER1,UART1")
        self.assertEqual(edges["rcc.UART1.gates.UART1"]["to"], "PART.UART1")
        self.assertIn("rcc-field-manual-1", edges["rcc.UART1.gates.UART1"]["evidence"])
        self.assertIn("mapping-UART1", edges["rcc.UART1.gates.TIMER1"]["evidence"])
        self.assertEqual(summary["peripheral_clocks"]["coupled_by_the_mapping"], {"UART1": ["TIMER1"]})
        # The record names the descriptor elements that clock an instance it serves, and the manual chapter.
        self.assertEqual(gate["clock_elements"], "BUS,UART_MUX,UART_CLK")
        self.assertEqual(gate["manual"], "RM1 chapter 7")
        self.assertEqual(summary["mapping"]["names_used_more_than_once"], ["TWICE"])
        kernel = records["PART.rcc.kernel.UART"]["attributes"]
        self.assertEqual((kernel["selector"], kernel["inputs"], kernel["shared_by"], kernel["sources_without_input"]),
                         ("UART_MUX", "PCLK,RCDIV", "UART1,UART2", "SDK_RCC_UART_CLK_SRC_NONE"))
        # The profile: selections and arithmetic.
        self.assertEqual(summary["profile"]["frequencies"], {
            "XTAL": "24000000", "SYNTH": "24000000", "RC": "48000000", "RC_DIV": "16000000", "SYS_MUX": "24000000", "SYS": "24000000",
            "BUS_DIV": "12000000", "BUS": "12000000", "UART_MUX": "12000000", "UART_CLK": "12000000",
            "SLOW": "32000", "WDT": "32000", "CORE": "24000000"})
        self.assertEqual(summary["profile"]["not_determined"], ["LONG_DIV"])
        assigned = {name: records[f"PART.clock-profile.xtal24.{name}"] for name in summary["profile"]["frequencies"]}
        # What a component selects is stated for the case that the instance is assigned to its capability and the
        # initialization succeeded, and is cited at the line that selects it. The statement says that it is stated
        # for no other case and that a reset selection may be the same.
        self.assertEqual(assigned["UART_CLK"]["attributes"]["holds_while"],
                         "UART1 is assigned to the capability serial and that capability's initialization of it has completed successfully")
        self.assertIn("component-unit-c-4", assigned["UART_MUX"]["evidence"])
        stated = next(record["statement"] for record in value["evidence"] if record["id"] == "clockprofile-UART_CLK")
        self.assertIn("It is stated for the case that UART1 is assigned to the capability serial", stated)
        self.assertIn("a selector's reset selection, which the graph does not record, may be the same input", stated)
        self.assertNotIn("only while", stated)
        self.assertEqual(assigned["UART_CLK"]["conditions"], {**CONDITIONS, "clock_profile": "xtal24"})
        self.assertNotIn("holds_while", assigned["BUS"]["attributes"])
        # What feeds the system clock runs, the crystal through the source it is the reference of. A clock derived
        # from the system clock is gated when an instance it names has a peripheral clock record or when it rests on
        # a component's selection, and captured otherwise: CORE names an instance without such a record. Anything
        # else is not stated to run.
        self.assertEqual({name: record["attributes"]["running"] for name, record in assigned.items()}, {
            "XTAL": "captured", "SYNTH": "captured", "SYS_MUX": "captured", "SYS": "captured", "BUS_DIV": "captured",
            "CORE": "captured", "BUS": "gated", "UART_MUX": "gated", "UART_CLK": "gated", "RC": "not_stated",
            "RC_DIV": "not_stated", "SLOW": "not_stated", "WDT": "not_stated"})
        self.assertEqual(summary["profile"]["by_running"]["gated"], ["BUS", "UART_CLK", "UART_MUX"])
        # A gated clock lists the peripheral clock records of the instances it names and cites the manual's lines
        # on enable bits, two lines here; it does not say that one is enabled. BUS is gated by its instance alone.
        cited = {record["id"]: record for record in value["evidence"]}
        self.assertEqual(assigned["BUS"]["attributes"]["peripheral_clocks"], "UART1")
        self.assertIn("profile-manual-2", assigned["BUS"]["evidence"])
        self.assertEqual((cited["profile-manual-2"]["start_line"], cited["profile-manual-2"]["end_line"]), (2, 3))
        self.assertIn("no assignment says whether one is enabled", cited["clockprofile-BUS"]["statement"])
        self.assertNotIn("peripheral_clocks", assigned["CORE"]["attributes"])
        self.assertNotIn("profile-manual-2", assigned["CORE"]["evidence"])
        # A clock derived from a nominal source names it.
        self.assertEqual((assigned["WDT"]["attributes"]["nominal_source"], assigned["WDT"]["attributes"]["basis"]), ("SLOW", "derived"))
        self.assertNotIn("nominal_source", assigned["BUS"]["attributes"])
        service = value["targets"][0]["services"][0]
        self.assertEqual(service["properties"], {"profile": "xtal24", "sysclk_hz": "24000000", "bus_hz": "12000000",
                                                  "records": "PART.rcc.NAME under xtal24"})
        self.assertIn("clockprofile-UART_CLK", value["capabilities"][0]["evidence"])
        self.assertEqual(value["capabilities"][0]["roles"], ROLES)
        # Every citation names lines of a pinned source with the hash of exactly those lines.
        sources = {source["id"]: inventory.Source(source, self.root) for source in value["sources"]}
        for record in value["evidence"][1:]:
            excerpt = sources[record["source"]].excerpt(record["start_line"], record["end_line"])
            self.assertEqual(record["excerpt_sha256"], inventory.sha256(excerpt.encode("utf-8")), record["id"])

    def test_an_element_longer_than_one_citation_is_cited_in_parts(self):
        value, summary = self.run_stage()
        node = next(record for record in value["resources"] if record["id"] == "PART.clock.LONG_DIV")
        self.assertEqual((node["attributes"]["choice_first"], node["attributes"]["choice_last"], node["attributes"]["choice_step"]),
                         ("1", "400", "1"))
        self.assertNotIn("choices", node["attributes"])
        cited = {record["id"]: record for record in value["evidence"]}
        parts = [cited[identifier] for identifier in node["evidence"] if identifier.startswith("clock-LONG_DIV")]
        self.assertEqual([part["id"] for part in parts], ["clock-LONG_DIV"] + [f"clock-LONG_DIV-part{index}" for index in range(2, len(parts) + 1)])
        # The parts cover the whole element: they follow one another, and none is longer than a citation may be
        # in lines or, with its record, in bytes.
        lines = (self.root / "clock.json").read_text(encoding="utf-8").splitlines()
        first = next(index + 1 for index, line in enumerate(lines) if '"id": "LONG_DIV"' in line) - 1
        last = next(index + 1 for index, line in enumerate(lines) if '"outputClockId": "long_ck"' in line) + 1
        self.assertGreater(len(parts), 1)
        self.assertEqual((parts[0]["start_line"], parts[-1]["end_line"]), (first, last))
        for before, after in zip(parts, parts[1:]):
            self.assertEqual(after["start_line"], before["end_line"] + 1)
        for part in parts:
            excerpt = "\n".join(lines[part["start_line"] - 1:part["end_line"]])
            self.assertLess(part["end_line"] - part["start_line"], 1024)
            self.assertLessEqual(len(json.dumps({"claim": part, "excerpt": excerpt})), 17000)
        self.assertGreater(last - first, 1024)
        # The descriptor's repeated value is counted once and said.
        self.assertEqual(summary["clock_tree"]["elements_that_repeat_a_choice"], ["LONG_DIV"])
        self.assertIn("400 distinct choices", cited["clock-LONG_DIV"]["statement"])
        self.assertIn("401 entries and one value more than once", cited["clock-LONG_DIV"]["statement"])

    def test_only_the_stated_fields_of_existing_records_change(self):
        before = copy.deepcopy(self.graph().value)
        after, _ = self.run_stage()
        for section in ("resources", "edges", "implementations", "evidence", "sources"):
            self.assertEqual(after[section][:len(before[section])], before[section], section)
        was, now = before["capabilities"][0], after["capabilities"][0]
        self.assertEqual({key: value for key, value in now.items() if key != "evidence"},
                         {key: value for key, value in was.items() if key != "evidence"})
        self.assertEqual(now["evidence"][:len(was["evidence"])], was["evidence"])
        was, now = before["targets"][0], after["targets"][0]
        self.assertEqual({key for key in now if now[key] != was[key]}, {"services", "evidence", "assumptions"})
        self.assertEqual(now["evidence"][:len(was["evidence"])], was["evidence"])

    def test_a_selection_must_be_what_its_cited_line_states(self):
        def altered(index, **change):
            self.config = copy.deepcopy(CONFIG)
            self.config["profile"]["selections"][index].update(change)

        # The citation still finds its line, and the line says another value.
        altered(0, value=32000000)
        self.refused("is not what its cited line states: the line does not set XTAL_VALUE to 32000000")
        # A right symbol with a wrong value, and a symbol of another input.
        altered(3, value=4)
        self.refused("is 4 and its symbol SDK_RCC_BUS_PRESCALER2 says something else")
        altered(2, input="RC")
        self.refused("is RC and its symbol SDK_RCC_SYS_SRC_SYNTH says something else")
        # The line of another field: the symbol is there, the field is not.
        altered(3, cite=[["captured", r"^other_prescaler = ", "Another prescaler."]])
        self.refused("the line does not set bus_prescaler to SDK_RCC_BUS_PRESCALER2")
        # A field that is only part of a name on the line, or that the line does not assign.
        altered(3, field="bus")
        self.refused("the line does not set bus to SDK_RCC_BUS_PRESCALER2")
        self.files["captured.c"] = CAPTURED.replace("bus_prescaler = SDK_RCC_BUS_PRESCALER2;", "bus_prescaler.other = SDK_RCC_BUS_PRESCALER2;")
        altered(3, cite=[["captured", r"^bus_prescaler\b", "Captured prescaler."]])
        self.refused("the line does not set bus_prescaler to SDK_RCC_BUS_PRESCALER2")
        altered(3)
        # The value stands only in a comment or in a string literal on the line.
        for line in ("bus_prescaler = 0; /* SDK_RCC_BUS_PRESCALER2 */", 'bus_prescaler = name("SDK_RCC_BUS_PRESCALER2")'):
            self.files["captured.c"] = CAPTURED.replace("bus_prescaler = SDK_RCC_BUS_PRESCALER2;", line)
            self.refused("the line does not set bus_prescaler to SDK_RCC_BUS_PRESCALER2")
        self.files["captured.c"] = CAPTURED
        # The assignment form fills its line and assigns the value alone. A comparison, a compound assignment, an
        # expression around the value, a cast, a second statement and a condition before it are refused; so is a
        # line that the joining of lines puts into a comment or a literal, or that stands under a zero condition.
        made = "bus_prescaler = SDK_RCC_BUS_PRESCALER2;"
        for line, pattern in (("if (bus_prescaler == SDK_RCC_BUS_PRESCALER2) {}", r"bus_prescaler =="),
                              ("check(bus_prescaler != SDK_RCC_BUS_PRESCALER2);", r"bus_prescaler !="),
                              ("bus_prescaler |= SDK_RCC_BUS_PRESCALER2;", r"^bus_prescaler \|="),
                              ("bus_prescaler = SDK_RCC_BUS_PRESCALER2 + 0 * SDK_RCC_OTHER;", r"^bus_prescaler = "),
                              ("bus_prescaler = SDK_RCC_OTHER + 0 * SDK_RCC_BUS_PRESCALER2;", r"^bus_prescaler = "),
                              ("bus_prescaler = flag ? SDK_RCC_OTHER : SDK_RCC_BUS_PRESCALER2;", r"^bus_prescaler = "),
                              ("bus_prescaler = (unsigned)SDK_RCC_BUS_PRESCALER2;", r"^bus_prescaler = "),
                              ("bus_prescaler = SDK_RCC_OTHER; " + made, r"^bus_prescaler = "),
                              (made + " bus_prescaler = SDK_RCC_OTHER;", r"^bus_prescaler = "),
                              ("bus_prescaler = SDK_RCC_BUS_PRESCALER2, bus_prescaler = SDK_RCC_OTHER;", r"^bus_prescaler = "),
                              ("if (flag) " + made, r"bus_prescaler = "),
                              ("// off \\\n" + made, r"^bus_prescaler = "),
                              ("// off \\ \n" + made, r"^bus_prescaler = "),
                              ("/\\\n/ " + made, r"bus_prescaler = "),
                              ("/\\\n* " + made + " */", r"bus_prescaler = "),
                              ("const char *text = \"a \\\n" + made + "\";", r"^bus_prescaler = "),
                              ("const char *text = \"x\\\\\n\"; " + made + " //\";", r"bus_prescaler = "),
                              ("#if 0\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if (0)\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if 0U\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if 0x0\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if 00\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if 0b0\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if ((0UL))\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#define MAKE \\\n" + made, r"^bus_prescaler = "),
                              ("#define UNUSED /*\n*/ " + made, r"bus_prescaler = "),
                              ("#define UNUSED /* a\n b\n*/ " + made, r"bus_prescaler = "),
                              (made + " \\\nbus_prescaler = SDK_RCC_OTHER;", r"^bus_prescaler = SDK_RCC_BUS"),
                              ("#if \\\n0\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if 0\n#elif 0\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if defined(X)\n#elif 0\n" + made + "\n#endif", r"^bus_prescaler = "),
                              ("#if 0\n#ifdef X\n#endif\n" + made + "\n#else\n#endif", r"^bus_prescaler = ")):
            self.files["captured.c"] = CAPTURED.replace(made, line)
            altered(3, cite=[["captured", pattern, "Captured prescaler."]])
            self.refused("the line does not set bus_prescaler to SDK_RCC_BUS_PRESCALER2")
        # The live branch after a block that is never compiled is an assignment like any other, and so is a member
        # of a structure.
        for line in ("#if 0\nbus_prescaler = SDK_RCC_OTHER;\n#else\n" + made + "\n#endif",
                     "#if 0\n#elif 0\n#else\n" + made + "\n#endif", "bus_prescaler =\\\n SDK_RCC_OTHER;\n" + made,
                     "/* a comment\n over lines */\n" + made, "#define UNUSED /* a\n b */\n" + made,
                     "bus_prescaler = SDK_RCC_BUS_PRESCALER2; /* a comment\n that goes on */"):
            self.files["captured.c"] = CAPTURED.replace(made, line)
            altered(3, cite=[["captured", r"^bus_prescaler = .*PRESCALER2", "Captured prescaler."]])
            self.config["timer_clock"]["cite"] = [["captured", r"^bus_prescaler = .*PRESCALER2", "Captured prescaler."]]
            self.run_stage()
        self.files["captured.c"] = CAPTURED.replace(made, "config.buses->bus_prescaler = SDK_RCC_BUS_PRESCALER2;")
        altered(3, cite=[["captured", r"bus_prescaler = ", "Captured prescaler."]])
        self.config["timer_clock"]["cite"] = [["captured", r"bus_prescaler = ", "Captured prescaler."]]
        self.run_stage()
        # The call form: every argument is one plain name or number and the value is one of them. A conditional
        # with a comma inside it, and a #define that only looks like a call, are refused.
        call = "select(SDK_RCC_SYS_SRC_SYNTH);"
        for line, pattern in (("select(1 ? 0, SDK_RCC_SYS_SRC_SYNTH, SDK_RCC_OTHER : SDK_RCC_OTHER);", r"^select\("),
                              ("select(SDK_RCC_OTHER | SDK_RCC_SYS_SRC_SYNTH);", r"^select\("),
                              ("select(other(SDK_RCC_SYS_SRC_SYNTH));", r"^select\("),
                              ("select(SDK_RCC_SYS_SRC_SYNTH); select(SDK_RCC_OTHER);", r"^select\("),
                              ("config.select(SDK_RCC_SYS_SRC_SYNTH);", r"select\("),
                              ("config->select(SDK_RCC_SYS_SRC_SYNTH);", r"select\("),
                              ("#define UNUSED /*\n*/ select(SDK_RCC_SYS_SRC_SYNTH);", r"select\("),
                              ("#define select(SDK_RCC_SYS_SRC_SYNTH) 0", r"^#define select\(")):
            self.files["captured.c"] = CAPTURED.replace(call, line)
            altered(2, cite=[["captured", pattern, "Captured selection."]])
            self.refused("is not what its cited line states: the line does not")
        for line in ("select(mode, SDK_RCC_SYS_SRC_SYNTH, config.wait);", "select(SDK_RCC_SYS_SRC_SYNTH, config->wait);",
                     "select(SDK_RCC_SYS_SRC_SYNTH, 123abc);", "select(SDK_RCC_SYS_SRC_SYNTH, 1 + 2);", "select(SDK_RCC_SYS_SRC_SYNTH, 5uLl);"):
            self.files["captured.c"] = CAPTURED.replace(call, line)
            altered(2, cite=[["captured", r"select\(", "Captured selection."]])
            self.refused("is not what its cited line states: the line does not")
        for line in ("select(mode, SDK_RCC_SYS_SRC_SYNTH, 0x10UL, 3);", "if (select(SDK_RCC_SYS_SRC_SYNTH) != 0) { return; }"):
            self.files["captured.c"] = CAPTURED.replace(call, line)
            altered(2, cite=[["captured", r"select\(", "Captured selection."]])
            self.run_stage()
        self.files["captured.c"] = CAPTURED
        # The definition form: the field defined as the value alone.
        for line in ("#define XTAL_VALUE (24000000UL) * 2U", "#define XTAL_VALUE 24000000UL + 1U", "#define XTAL_VALUE (24000000UL)",
                     "#define XTAL_VALUE 24000000UL /*\n*/ * 2U", "#define XTAL_VALUE /*\n*/ 24000000UL",
                     "#define XTAL_VALUE_OTHER 24000000UL\n#define XTAL_VALUE XTAL_VALUE_OTHER"):
            self.files["conf.h"] = CONF.replace("#define XTAL_VALUE 24000000UL", line)
            altered(0)
            self.refused("is not what its cited line states: the line does not")
        self.files["conf.h"] = CONF
        # A symbol that ends in kilohertz says its number.
        altered(4, symbol="SDK_RCC_SLOW_32KHZ")
        self.files["device.h"] = DEVICE.replace("#define SLOW_VALUE 32000", "#define SLOW_VALUE SDK_RCC_SLOW_32KHZ")
        self.run_stage()
        altered(4, symbol="SDK_RCC_SLOW_32KHZ", value=31000)
        self.refused("is 31000 and its symbol SDK_RCC_SLOW_32KHZ says something else")
        self.files["device.h"] = DEVICE
        # A number is not found inside a name or another number.
        altered(3, symbol=None)
        del self.config["profile"]["selections"][3]["symbol"]
        self.refused("the line does not set bus_prescaler to 2")
        # A selection cites the line that makes it; supporting lines do not stand in for it.
        altered(3, cite=[])
        self.refused("the selection of BUS_DIV cites no line that makes it")
        altered(3, cite=[], support=[["captured", r"^bus_prescaler = ", "Captured prescaler."]])
        self.refused("the selection of BUS_DIV cites no line that makes it")
        # A selection that cites lines has to name the field they set.
        altered(0, field=None)
        del self.config["profile"]["selections"][0]["field"]
        self.refused("does not name the field they set")

    def test_an_element_with_a_single_choice_needs_no_selection(self):
        elements = {element["id"]: element for element in self.descriptor["tree"]["elements"]}
        elements["SYNTH"]["oneOf"] = [{"const": 24000000}]
        elements["SYS_MUX"]["possible_Input"] = elements["SYS_MUX"]["possible_Input"][:1]
        elements["RC"]["outputTargets"] = ["RC_DIV", "LONG_DIV"]
        selections = self.config["profile"]["selections"]
        self.config["profile"]["selections"] = [entry for entry in selections if entry["element"] not in ("SYNTH", "SYS_MUX")]
        value, summary = self.run_stage()
        records = {record["id"]: record for record in value["resources"]}
        self.assertEqual(summary["profile"]["frequencies"]["SYS"], "24000000")
        for name in ("SYNTH", "SYS_MUX"):
            attributes = records[f"PART.clock-profile.xtal24.{name}"]["attributes"]
            self.assertEqual((attributes["basis"], attributes["running"]), ("fixed", "captured"))
            self.assertNotIn("selection", attributes)
        # With two choices and no selection the system clock is not determined.
        elements["SYNTH"]["oneOf"] = [{"const": 24000000}, {"const": 49000000}]
        self.refused("the profile does not determine the system clock SYS")

    def test_a_rate_that_does_not_divide_its_clock_is_refused(self):
        self.refused("does not divide", baud=("9600", "7000"))

    def test_a_frequency_above_the_descriptor_limit_is_refused(self):
        # A crystal inside its own range that puts the system clock above the system clock's limit.
        self.files["captured.c"] = CAPTURED.replace("synth_out = SDK_RCC_SYNTH_OUT_24MHZ", "synth_out = SDK_RCC_SYNTH_OUT_49MHZ")
        self.config["profile"]["selections"][1].update(value=49000000, symbol="SDK_RCC_SYNTH_OUT_49MHZ")
        self.config["profile"]["expect"] = {}
        self.refused("above the descriptor's limit")

    def test_a_selection_outside_the_descriptor_is_refused(self):
        for index, change, text in ((0, {"value": 60000000}, "outside the descriptor's range"), (2, {"input": "PLL"}, "has no input"),
                                    (3, {"value": 3}, "has no choice")):
            self.config = copy.deepcopy(CONFIG)
            self.config["profile"]["selections"][index].update(change)
            self.refused(text)

    def test_the_captured_value_must_be_the_derived_one(self):
        self.files["captured.c"] = CAPTURED.replace("= 24000000 */", "= 48000000 */")
        self.config["profile"]["expect"]["SYS"]["pattern"] = r"SYSCLK\(Hz\) = 48000000"
        self.refused("the captured configuration states otherwise")

    def test_the_timer_clock_rule_holds_only_for_the_stated_prescalers(self):
        self.config["timer_clock"]["requires"] = {"BUS_DIV": "1"}
        self.refused("the timer clock rule holds only while BUS_DIV is 1")
        # The rule names exactly the prescalers of its timer clocks, found by descriptor role.
        for requires in ({}, {"RC_DIV": "3"}, {"BUS_DIV": "2", "RC_DIV": "3"}):
            self.config["timer_clock"]["requires"] = requires
            self.refused("the timer clock rule has to name the prescalers of its timer clocks, ['BUS_DIV']")
        del self.config["timer_clock"]["requires"]
        self.refused("the timer clock rule has to name the prescalers of its timer clocks")
        self.config = copy.deepcopy(CONFIG)
        for elements in (["SYS"], [], ["BUS", "SYS"]):
            self.config["timer_clock"]["elements"] = elements
            self.refused("the timer clock rule has to list the timer clock elements of the descriptor, ['BUS']")

    def test_a_component_selection_needs_the_line_that_makes_it(self):
        # The source is named only in a comment, with parentheses or without, or passed to another call.
        for line in ("  /* {{clock_source}} */\n", "  /* ignored({{clock_api}}({{clock_source}})) */\n",
                     "  (void)other({{clock_source}});\n", "  // {{clock_api}}({{clock_source}});\n",
                     "  /*\n  {{clock_api}}({{clock_source}});\n  */\n", "  /*\n   * {{clock_api}}({{clock_source}});\n   */\n",
                     "  const char *text = \"{{clock_api}}({{clock_source}})\";\n",
                     "  // off \\\n  {{clock_api}}({{clock_source}});\n",
                     "  const char *text = \"a \\\n  {{clock_api}}({{clock_source}});\";\n",
                     "#if 0\n  {{clock_api}}({{clock_source}});\n#endif\n",
                     "  /\\\n/ {{clock_api}}({{clock_source}});\n", "  /\\\n* {{clock_api}}({{clock_source}}); */\n",
                     "  const char *text = \"x\\\\\n\"; {{clock_api}}({{clock_source}}); //\";\n",
                     "#if \\\n0\n  {{clock_api}}({{clock_source}});\n#endif\n",
                     "#define make() {{clock_api}}({{clock_source}})\n", "#define make() \\\n  {{clock_api}}({{clock_source}})\n",
                     "  other_{{clock_api}}({{clock_source}});\n",
                     "#define unused /*\n*/  {{clock_api}}({{clock_source}});\n"):
            self.files["unit.c.in"] = UNIT.replace("  (void){{clock_api}}({{clock_source}});\n", line)
            self.refused("no template line of it calls the kernel_clock_api with it")
        self.files["unit.c.in"] = UNIT
        # No capability lists a card that reads the attribute, or its role cannot be filled by this resource.
        self.listed = []
        self.refused("no component template or captured line makes that selection")
        self.listed = ["card"]
        self.roles = [{**ROLES[0], "attributes": {"instance": "UART2"}}]
        self.refused("no component template or captured line makes that selection")

    def test_a_captured_initialization_makes_a_selection_for_the_capabilities_that_list_its_card(self):
        captured = {"value": "SDK_RCC_UART_CLK_SRC_PCLK", "card": "startup",
                    "cite": [["captured", r"SDK_RCC_UART_SetKernelClkSource\(", "Captured line."]]}
        self.config["profile"]["captured_component_selections"] = [captured]
        value, _ = self.run_stage()
        selector = next(record for record in value["resources"] if record["id"] == "PART.clock-profile.xtal24.UART_MUX")
        # Both the component and the captured line are cited, and both capabilities are named.
        self.assertEqual(selector["attributes"]["holds_while"],
                         "UART1 is assigned to one of the capabilities console, serial and that capability's initialization of it "
                         "has completed successfully")
        self.assertLessEqual({"component-unit-c-4", "profile-captured-6"}, set(selector["evidence"]))
        # The captured line alone is enough.
        self.listed = []
        value, _ = self.run_stage()
        selector = next(record for record in value["resources"] if record["id"] == "PART.clock-profile.xtal24.UART_MUX")
        self.assertEqual(selector["attributes"]["holds_while"],
                         "UART1 is assigned to the capability console and that capability's initialization of it has completed successfully")
        # A line that does not call the paired setter with the enumerator is refused, and so is a card nothing lists.
        self.config["profile"]["captured_component_selections"] = [{**captured, "cite": [["captured", r"^select\(", "Another line."]]}]
        self.refused("does not call SDK_RCC_UART_SetKernelClkSource with it")
        # The call stands inside a comment over several lines, or inside a string literal.
        self.config["profile"]["captured_component_selections"] = [captured]
        call = "SDK_RCC_UART_SetKernelClkSource(SDK_RCC_UART_CLK_SRC_PCLK);\n"
        for text in ("/*\n" + call + "*/\n", "SDK_RCC_UART_SetKernelClkSource_name = \"" + call.strip() + "\";\n",
                     "// off \\\n" + call, "const char *text = \"a \\\n" + call.strip() + "\";\n", "#if 0\n" + call + "#endif\n",
                     "/\\\n/ " + call, "#if 0\n#elif 0\n" + call + "#endif\n", "#define make() " + call,
                     "#define UNUSED /*\n*/ " + call):
            self.files["captured.c"] = CAPTURED.replace(call, text)
            self.refused("does not call SDK_RCC_UART_SetKernelClkSource with it")
        self.files["captured.c"] = CAPTURED
        self.config["profile"]["captured_component_selections"] = [{**captured, "card": "absent"}]
        self.refused("no capability lists the card absent")
        # A captured initialization cites its own line; the line of a component template does not stand in for it.
        self.listed = ["card"]
        self.config["profile"]["captured_component_selections"] = [{**captured, "cite": []}]
        self.refused("the captured selection of SDK_RCC_UART_CLK_SRC_PCLK cites no line that makes it")

    def test_a_selection_the_configuration_states_for_a_component(self):
        # The resource carries no source of its own, so the configuration states the selection of the paired
        # selector. Its symbol is an enumerator the label map gives another name than the descriptor's input.
        self.kernel_source = None
        self.files["rcc.h"] = HEADER.replace("SDK_RCC_UART_CLK_SRC_RCDIV = 1", "SDK_RCC_UART_CLK_SRC_LSE = 1")
        self.files["captured.c"] = CAPTURED.replace("SDK_RCC_UART_CLK_SRC_PCLK);", "SDK_RCC_UART_CLK_SRC_LSE);")
        self.config["kernel_clocks"]["selectors"]["UART"]["labels"] = {"LSE": "RCDIV"}
        entry = {"element": "UART_MUX", "input": "RCDIV", "basis": "component", "capability": "serial", "instance": "UART1",
                 "symbol": "SDK_RCC_UART_CLK_SRC_LSE", "field": "SDK_RCC_UART_SetKernelClkSource",
                 "cite": [["captured", r"^SDK_RCC_UART_SetKernelClkSource\(", "Captured call."]]}
        self.config["profile"]["selections"].append(entry)
        value, _ = self.run_stage(baud=("8000", "16000"))
        records = {record["id"]: record for record in value["resources"]}
        held = "UART1 is assigned to the capability serial and that capability's initialization of it has completed successfully"
        selector, clock = records["PART.clock-profile.xtal24.UART_MUX"], records["PART.clock-profile.xtal24.UART_CLK"]
        self.assertEqual((selector["attributes"]["frequency_hz"], selector["attributes"]["selection"], selector["attributes"]["sdk_symbol"],
                          selector["attributes"]["holds_while"]), ("16000000", "RCDIV", "SDK_RCC_UART_CLK_SRC_LSE", held))
        # The clock derived through it is stated for the same case; its source is not stated to run.
        self.assertEqual((clock["attributes"]["holds_while"], clock["attributes"]["running"]), (held, "not_stated"))
        # The symbol has to be the enumerator the pairing gives the selected input.
        entry["symbol"] = "SDK_RCC_UART_CLK_SRC_PCLK"
        self.refused("is RCDIV and its symbol SDK_RCC_UART_CLK_SRC_PCLK says something else")
        entry["symbol"] = "SDK_RCC_UART_CLK_SRC_LSE"
        # The field has to be a function the SDK declares, not a variable that happens to hold the value.
        entry["field"] = "bus_prescaler"
        self.refused("which is no function the SDK header declares for this part")
        entry["field"] = "SDK_RCC_UART_SetKernelClkSource"
        # The instance has to be one a role of the capability can be given, and both have to be named.
        entry["instance"] = "UART7"
        self.refused("no role of serial takes the instance UART7")
        entry["instance"] = "TIMER1"
        self.refused("no role of serial takes the instance TIMER1")
        del entry["instance"]
        self.refused("names no capability or no instance")
        entry.update(instance="UART1")
        del entry["capability"]
        self.refused("names no capability or no instance")

    def test_enumerators_and_selector_inputs_must_be_the_same_set(self):
        self.files["rcc.h"] = HEADER.replace("SDK_RCC_UART_CLK_SRC_RCDIV = 1", "SDK_RCC_UART_CLK_SRC_LSE = 1")
        self.refused("the SDK offers")
        self.config["kernel_clocks"]["selectors"]["UART"]["labels"] = {"LSE": "RCDIV"}
        value, _ = self.run_stage()
        self.assertTrue(any(record["id"] == "PART.rcc.kernel.UART" for record in value["resources"]))
        # An enumerator without an input has to be one the SDK declares, and has to be named.
        self.config["kernel_clocks"]["selectors"]["UART"]["without_input"] = ["NONE", "OFF"]
        self.refused("names an enumerator without input that the SDK does not declare")
        self.config["kernel_clocks"]["selectors"]["UART"]["without_input"] = []
        self.refused("the SDK offers")

    def test_every_selector_is_paired_or_named(self):
        self.config["kernel_clocks"]["selectors_without_setter"] = {}
        self.refused("selectors no kernel clock setter is paired with")
        self.config = copy.deepcopy(CONFIG)
        self.config["kernel_clocks"]["selectors"] = {}
        self.refused("does not pair")

    def test_a_stated_link_cites_the_enable_field_of_a_known_record(self):
        self.config["gates"]["stated_links"] = {"UART9": CONFIG["gates"]["stated_links"]["UART1"]}
        self.refused("stated links name peripheral clocks the SDK does not declare")
        self.config = copy.deepcopy(CONFIG)
        self.files["manual.txt"] = "Bit 3 OTHEREN: UART1 clock enable\n"
        self.config["gates"]["stated_links"]["UART1"][0]["pattern"] = r"^Bit 3 OTHEREN"
        self.refused("does not cite the record's enable field")
        # An instance no resource stands for is refused, except on a re-targeted graph, which may lack it.
        self.files["manual.txt"] = MANUAL
        self.config = copy.deepcopy(CONFIG)
        self.config["gates"]["stated_links"]["UART1"][0]["instances"] = ["UART7"]
        self.refused("no resource stands for the instance UART7")
        self.config["composed"] = True
        value, _ = self.run_stage()
        self.assertFalse(any(edge["to"].endswith("UART7") for edge in value["edges"]))

    def test_a_chapter_pointer_can_be_withheld(self):
        self.config["mapping"]["withheld_chapters"] = {"UART1": "It names the wrong chapter."}
        value, summary = self.run_stage()
        gate = next(record for record in value["resources"] if record["id"] == "PART.rcc.UART1")
        self.assertNotIn("manual", gate["attributes"])
        self.assertEqual(summary["peripheral_clocks"]["chapter_pointers_withheld"], ["UART1"])

    def test_a_retargeted_graph_may_lack_a_capability_of_the_rate_basis(self):
        self.config["rate_basis"].append({"capabilities": ["absent"], "elements": ["UART_CLK"]})
        self.refused("rate basis names the unknown capability absent")
        self.config["composed"] = True
        _, summary = self.run_stage()
        self.assertEqual(summary["rate_basis"]["not_on_this_target"], ["absent"])

    def test_a_drawn_connection_into_a_selector_must_be_one_of_its_inputs(self):
        self.descriptor["tree"]["transitions"].append({"id": "t3", "sourceTaskId": "RC_DIV_output", "targetTaskId": "SYS_MUX__RC_DIV_input"})
        self.refused("which does not list it as an input")

    def test_a_declared_function_needs_a_compiled_definition(self):
        self.files["rcc.c"] = BODY.replace("#if defined(USE_RCC) && (USE_RCC == 1)", "#if defined(UART9)")
        self.refused("its definition is not compiled")

    def test_the_stage_does_not_run_twice(self):
        value, _ = self.run_stage()
        graph = inventory.Graph(value, "T", {})
        with self.assertRaises(SystemExit) as refusal:
            clocks.run(graph, {}, {}, TARGET, self.config, self.root)
        self.assertIn("already pinned", str(refusal.exception))

    def test_a_source_the_configuration_pins_by_hash_must_match(self):
        self.config["sources"][0]["sha256"] = "0" * 64
        self.refused("differs from the hash the configuration pins")


class Conditionals(unittest.TestCase):
    def kept(self, text, defined=(), undetermined=(), settled=None):
        data = text.encode("utf-8")
        source = inventory.Source({"id": "c", "sha256": inventory.sha256(data)}, data=data)
        return clocks.Conditional(source, set(defined), set(undetermined), settled).state

    def test_branches(self):
        text = "a\n#if defined(X)\nb\n#elif defined(Y)\nc\n#else\nd\n#endif\ne\n#ifndef Z\nf\n#endif\n"
        self.assertEqual(self.kept(text, {"X"}), [True, True, True, True, False, True, False, True, True, True, True, True])
        self.assertEqual(self.kept(text, {"Y"})[2:7:2], [False, True, False])
        self.assertEqual(self.kept(text, {"Z"})[2:7:2] + self.kept(text, {"Z"})[10:11], [False, False, True, False])

    def test_a_test_of_two_macros_has_no_verdict_until_it_is_settled(self):
        text = "#if defined(A) && (A == 1)\nx\n#else\ny\n#endif\n#if defined (B)\n#ifdef C\nz\n#endif\n#endif\n"
        self.assertEqual(self.kept(text, {"A", "B", "C"})[1:4:2], [None, None])
        self.assertEqual(self.kept(text, {"A"}, settled={"defined(A)  &&  (A == 1)": True})[1:4:2], [True, False])
        # A settled test decides an #elif as it decides an #if.
        later = "\n".join(["#if defined(X)", "x", "#elif defined(A) && (A == 1)", "y", "#else", "z", "#endif", ""])
        self.assertEqual(self.kept(later, {"A"})[1:6:2], [False, None, None])
        self.assertEqual(self.kept(later, {"A"}, settled={"defined(A) && (A == 1)": True})[1:6:2], [False, True, False])
        # A macro the configuration calls undetermined decides nothing, and what is nested under it neither.
        self.assertEqual(self.kept(text, {"C"}, undetermined={"B"})[7], None)
        self.assertEqual(self.kept(text, {"B"})[7], False)

    def test_spans_are_bounded_in_lines_as_well_as_bytes(self):
        data = ("x\n" * 2500).encode("utf-8")
        source = inventory.Source({"id": "c", "sha256": inventory.sha256(data)}, data=data)
        inventory.use_schema({"schema": 5}, 5)
        spans = clocks.Clocks.spans(source, 1, 2500)
        self.assertEqual((spans[0][0], spans[-1][1]), (1, 2500))
        self.assertTrue(all(after[0] == before[1] + 1 for before, after in zip(spans, spans[1:])))
        self.assertTrue(all(last - first < 1024 for first, last in spans))
        self.assertEqual(clocks.Clocks.spans(source, 5, 9), [(5, 9)])

    def test_unbalanced_directives_are_refused(self):
        for text in ("#endif\n", "#if defined(A)\n", "#else\n"):
            with self.assertRaises(SystemExit):
                self.kept(text)


if __name__ == "__main__":
    unittest.main()
