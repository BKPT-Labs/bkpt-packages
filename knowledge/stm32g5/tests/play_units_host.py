# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Host regression of the STM32G5 PLAY graph and its generated units.

    python -B -m unittest stm32g5.tests.play_units_host      (from knowledge/)
    python -B stm32g5/tests/play_units_host.py

What is checked:

* the graph: every PLAY card's truth table map equals the table the SDK's
  documented encoding gives for the function's expression; every PLAY pad route
  joins a sub-resource to a pad the descriptor routes; the board rule admits
  only pads the board composition leaves free (the user button for inputs);
* the units: each PLAY template is rendered with the bindings the graph's own
  cards name, for resources chosen the way a request would take them, compiled
  with a host C compiler (CC, or gcc) next to play_hal_model.c, and driven by
  play_units_driver.c: every function of every capability over all input
  combinations, three units composed on one PLAY1, a unit initialized twice,
  LSI, truth table write and kernel clock faults, software trigger misuse and a
  later unit's restart of PLAY1;
* with BKPT_KNOWLEDGE_SOURCE_ROOT set: graph.json is byte for byte what
  regenerate_graph.py writes from the pinned inputs.

The model is not the hardware: see play_hal_model.c. Build outputs go to a
temporary folder that is removed.
"""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
PACKAGE = HERE.parent
sys.path.insert(0, str(HERE))
import regenerate_graph  # noqa: E402

SCENARIOS = ("compose", "twice", "lsi", "truth_write", "kernel_clock", "triggers", "restart")


def graph():
    return json.loads((PACKAGE / "graph.json").read_text(encoding="utf-8"))


def layer():
    return json.loads((PACKAGE / "imports/play-layer.json").read_text(encoding="utf-8"))


# Expected output bits in input order 00, 01, 10, 11 (A is bit 0).
EXPECTED_NIBBLE = {
    "and": 0b1000, "or": 0b1110, "xor": 0b0110,
    "nand": 0b0111, "nor": 0b0001, "xnor": 0b1001,
    "buffer": 0b1010, "invert": 0b0101,
}


class Graph:
    def __init__(self, value):
        self.value = value
        self.resources = {r["id"]: r for r in value["resources"]}
        self.cards = {i["id"]: i for i in value["implementations"]}
        self.capabilities = {c["id"]: c for c in value["capabilities"]}

    def routes(self, relation="play_pad"):
        return [e for e in self.value["edges"] if e["relation"] == relation]

    def assign(self, capability, taken):
        """Resources for each role, in resource order, as a request without bindings could get them."""
        cap = self.capabilities[capability]
        chosen = {}
        links = {link["from"]: link for link in cap["links"]}
        for role in cap["roles"]:
            if role["id"] in chosen:
                continue
            candidates = sorted(r["id"] for r in self.value["resources"]
                                if r["kind"] == role["kind"]
                                and all(r["attributes"].get(k) == v for k, v in role["attributes"].items()))
            for candidate in candidates:
                if (candidate in taken or candidate in chosen.values()) and role["kind"] != "play":
                    continue
                link = links.get(role["id"])
                if link is None:
                    chosen[role["id"]] = candidate
                    break
                pad_role = next(r for r in cap["roles"] if r["id"] == link["to"])
                pads = [e["to"] for e in self.routes(link["relation"]) if e["from"] == candidate
                        and e["to"] not in taken and e["to"] not in chosen.values()
                        and all(self.resources[e["to"]]["attributes"].get(k) == v for k, v in pad_role["attributes"].items())]
                if pads:
                    chosen[role["id"]], chosen[link["to"]] = candidate, pads[0]
                    break
            if role["id"] not in chosen:
                raise AssertionError(f"{capability}: no resource for {role['id']}")
        taken.update(v for v in chosen.values())
        return chosen

    def bindings(self, capability, card_id, unit, parameters, chosen):
        card = self.cards[card_id]
        values = {}
        for name, binding in card["component"]["bindings"].items():
            source = binding["source"]
            if source["type"] == "instance":
                raw = unit
            elif source["type"] == "parameter":
                raw = parameters[source["name"]]
            elif source["type"] == "resource_attribute":
                raw = self.resources[chosen[source["role"]]]["attributes"][source["attribute"]]
            else:
                edge = min((e for e in self.value["edges"] if e["from"] == chosen[source["from"]]
                            and e["to"] == chosen[source["to"]] and e["relation"] == source["relation"]), key=lambda e: e["id"])
                raw = edge["attributes"][source["attribute"]]
            values[name] = binding.get("map", {}).get(raw, raw) if binding.get("map") else raw
        return values


def expand(text, values):
    def replace(match):
        if match.group(1) not in values:
            raise AssertionError(f"template names unbound {match.group(1)}")
        return values[match.group(1)]
    out = re.sub(r"\{\{([A-Za-z0-9_.:-]+)\}\}", replace, text)
    assert "{{" not in out and "}}" not in out
    return out


def compiler():
    named = os.environ.get("CC") or shutil.which("gcc")
    if not named:
        raise unittest.SkipTest("no host C compiler: set CC")
    env = dict(os.environ)
    env["PATH"] = str(pathlib.Path(named).parent) + os.pathsep + env.get("PATH", "")
    return named, env


def pad_c(pad_id):
    name = pad_id.split(".")[-1]
    return f"HAL_GPIO{name[1]}", f"HAL_GPIO_PIN_{int(name[2:])}"


def build_and_run(units, label):
    """units: list of (capability, function, pulls) rendered together into one image."""
    g = Graph(graph())
    lay = layer()
    caps = {c["id"]: c for c in lay["capabilities"]}
    cc, env = compiler()
    work = pathlib.Path(tempfile.mkdtemp(prefix="bkpt-g5-play-"))
    try:
        shutil.copyfile(HERE / "play_hal_model.h", work / "stm32_hal.h")
        shutil.copyfile(HERE / "play_hal_model.h", work / "play_hal_model.h")
        taken, sources, rows, pulls_c = set(), [], [], {"none": "HAL_GPIO_PULL_NO", "up": "HAL_GPIO_PULL_UP", "down": "HAL_GPIO_PULL_DOWN"}
        for index, (capability, function, pulls) in enumerate(units):
            spec = caps[capability]
            chosen = g.assign(capability, taken)
            unit = f"unit{index}_{capability}"
            parameters = {"function": function, "min_pulse_width": str(17 * index + 3)}
            parameters.update({f"pull_{x}": pulls[k] for k, x in enumerate(spec["inputs"])})
            values = g.bindings(capability, spec["card"], unit, parameters, chosen)
            for file in g.cards[spec["card"]]["component"]["files"]:
                record = next(s for s in lay["sources"] if s["id"] == file["template"])
                text = expand((PACKAGE / record["package_file"]).read_text(encoding="utf-8"), values)
                path = work / expand(file["path"], values).split("/")[-1]
                path.write_text(text, encoding="utf-8", newline="\n")
                if path.suffix == ".c":
                    sources.append(path)
            expected = EXPECTED_NIBBLE[function]
            ins = [pad_c(chosen[f"pad_{x}"]) for x in spec["inputs"]] + [("HAL_GPIOA", "0")] * (2 - len(spec["inputs"]))
            out = pad_c(chosen["pad_out"])
            trigger = int(g.resources[chosen["trigger"]]["attributes"]["name"][6:]) if spec["trigger"] else 99
            setter = f"{unit}_set_trigger" if spec["trigger"] else "NULL"
            getter = f"{unit}_get_trigger" if spec["trigger"] else "NULL"
            rows.append(f'  {{"{unit}", {unit}_init, {setter}, {getter}, {len(spec["inputs"])}, {{{ins[0][0]}, {ins[1][0]}}}, '
                        f'{{{ins[0][1]}, {ins[1][1]}}}, {{{", ".join(pulls_c[p] for p in (pulls + ["none"])[:2])}}}, {trigger}U, '
                        f'{out[0]}, {out[1]}, {expected}U}},')
            with (work / "includes.h").open("a", encoding="utf-8", newline="\n") as handle:
                handle.write(f'#include "{unit}.h"\n')
        routes = []
        for edge in g.routes():
            port, pin = pad_c(edge["to"])
            sub = edge["from"].split(".")[-1]
            number = int(re.sub(r"\D", "", sub))
            routes.append(f"  {{{port}, {pin}, {number if sub.startswith('IN') else -1}, {number if sub.startswith('OUT') else -1}}},")
        scenario = "\n".join([
            '#include "play_hal_model.h"', '#include "includes.h"',
            f'#define SCENARIO "{label}"', f"#define UNIT_COUNT {len(units)}U",
            "typedef struct { const char *name; hal_status_t (*init)(void); hal_status_t (*set)(uint32_t);"
            " hal_status_t (*get)(uint32_t *); int inputs; hal_gpio_t in_port[2]; uint32_t in_pin[2]; int in_pull[2];"
            " unsigned trigger; hal_gpio_t out_port; uint32_t out_pin; unsigned expected; } unit_t;",
            "static const unit_t UNITS[] = {", *rows, "};",
            "static const model_route_t ROUTES[] = {", *routes, "};", ""])
        (work / "scenario.h").write_text(scenario, encoding="utf-8", newline="\n")
        exe = work / "driver.exe"
        command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(work),
                   str(HERE / "play_units_driver.c"), str(HERE / "play_hal_model.c"), *map(str, sources), "-o", str(exe)]
        built = subprocess.run(command, capture_output=True, text=True, env=env, cwd=work)
        if built.returncode != 0:
            raise AssertionError(f"host build failed:\n{built.stderr[-4000:]}")
        codes, output = 0, ""
        for scenario in SCENARIOS:
            ran = subprocess.run([str(exe), scenario], capture_output=True, text=True, env=env, cwd=work, timeout=60)
            codes |= ran.returncode
            output += ran.stdout
        return codes, output
    finally:
        shutil.rmtree(work, ignore_errors=True)


class PlayGraph(unittest.TestCase):
    def setUp(self):
        self.g = Graph(graph())
        self.layer = layer()

    def test_truth_table_maps_follow_the_documented_encoding(self):
        for spec in self.layer["capabilities"]:
            table = self.g.cards[spec["card"]]["component"]["bindings"]["truth_table"]["map"]
            for function in spec["functions"]:
                expression = self.layer["truth_tables"][function]["expression"]
                self.assertEqual(table[function], regenerate_graph.truth_table(expression), function)
                # Independent of inputs C and D: bits repeat every 4 combinations.
                value = int(table[function], 16)
                self.assertEqual(value, (value & 0xF) * 0x1111, function)

    def test_pad_routes_are_descriptor_routes(self):
        signal = {(e["attributes"]["signal"], e["to"]) for e in self.g.routes("signal")}
        for edge in self.g.routes():
            self.assertIn((edge["attributes"]["signal"], edge["to"]), signal)
            self.assertEqual(edge["attributes"]["alternate"], "AF14")
            sub = edge["from"].split(".")[-1]
            self.assertEqual(edge["attributes"]["signal"], f"PLAY1_{sub}")

    def test_board_rule(self):
        board = {"PC13"}                                   # user button: inputs only
        never = {"PA3", "PC14", "PC15", "PG3", "PA13", "PA14", "PB3", "PH0", "PH1", "PA2"}
        for r in self.g.value["resources"]:
            name = r["id"].split(".")[-1]
            attributes = r["attributes"]
            if name in never:
                self.assertNotIn("play_input_pad", attributes, name)
                self.assertNotIn("play_output_pad", attributes, name)
        self.assertEqual(self.g.resources["STM32G564RET3.PC13"]["attributes"].get("play_input_pad"), "true")
        inputs = sorted(r["id"].split(".")[-1] for r in self.g.value["resources"] if r["attributes"].get("play_input_pad"))
        outputs = sorted(r["id"].split(".")[-1] for r in self.g.value["resources"] if r["attributes"].get("play_output_pad"))
        self.assertEqual(inputs, ["PA9", "PC13", "PE2"])
        self.assertEqual(outputs, ["PB1", "PB14", "PD11", "PD12"])
        self.assertTrue(board <= set(inputs))

    def test_schema_and_briefs(self):
        self.assertEqual(self.g.value["schema"], 6)
        for cap in self.g.value["capabilities"]:
            self.assertTrue(0 < len(cap["brief"]) <= 96)
        target = self.g.value["targets"][0]
        manifest = (PACKAGE / "foundation-manifest.json").read_bytes()
        self.assertEqual(target["foundation_sha256"], regenerate_graph.inventory.sha256(manifest))

    @unittest.skipUnless(os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT"), "set BKPT_KNOWLEDGE_SOURCE_ROOT to regenerate")
    def test_graph_regenerates_byte_for_byte(self):
        built, _ = regenerate_graph.generate(pathlib.Path(os.environ["BKPT_KNOWLEDGE_SOURCE_ROOT"]))
        self.assertEqual(regenerate_graph.inventory.encoded(built), (PACKAGE / "graph.json").read_bytes())


class PlayUnits(unittest.TestCase):
    def run_case(self, units, label):
        code, output = build_and_run(units, label)
        self.assertEqual(code, 0, output)
        self.assertEqual(output.count(" 0 failures"), len(SCENARIOS), output)

    def test_every_function_alone(self):
        for spec in layer()["capabilities"]:
            for function in spec["functions"]:
                for pull in ("none", "up", "down"):
                    with self.subTest(capability=spec["id"], function=function, pull=pull):
                        self.run_case([(spec["id"], function, [pull] * len(spec["inputs"]))], f"{spec['id']}-{function}-{pull}")

    def test_composition_on_one_play(self):
        self.run_case([("play_logic_gate", "xor", ["down", "up"]),
                       ("play_software_gate", "xnor", ["down"])], "composition")


if __name__ == "__main__":
    unittest.main()
