# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Regenerate the STM32G5 capability graph from its pinned inputs, or check it.

The graph is written in four stages, each reading only pinned sources and the
package's own inputs:

1. a first layer: the target, the pinned sources with their hashes and scopes,
   from `imports/play-layer.json`;
2. the shared inventory stage (`knowledge/tools/author_device_inventory.py`)
   with `imports/device-inventory.json`: every pad, descriptor signal route and
   interrupt of the STM32G564RET3 in LQFP64;
3. the PLAY stage below: PLAY1 and its 64 sub-resources, the PLAY pad routes,
   the NUCLEO-G564RE pad rule read from the board composition, three PLAY
   capabilities, their integration card and their generated components;
4. the shared briefs stage (`author_capability_briefs.py`) with
   `imports/capability-briefs.json`, which raises the graph to schema 6.

Every record the PLAY stage writes cites the source lines it states. Sources
BKPT authored are read from the package (`package_file`); manufacturer
sources from the source root. Nothing is copied.

    python -B stm32g5/tests/regenerate_graph.py --source-root ROOT --check
    python -B stm32g5/tests/regenerate_graph.py --source-root ROOT --out NEW.json

ROOT defaults to BKPT_KNOWLEDGE_SOURCE_ROOT. `--check` regenerates in memory
and compares the bytes with `graph.json`. `--stage-root DIR` also copies
every pinned source to its graph path under DIR, a temporary source root for
`bkpt-fw architecture build`.
"""
import argparse
import copy
import json
import os
import pathlib
import re
import sys

PACKAGE = pathlib.Path(__file__).resolve().parents[1]
TOOLS = PACKAGE.parent / "tools"
sys.path.insert(0, str(TOOLS))
from board_profiles import add_profiles
import author_capability_briefs as briefs  # noqa: E402
import author_device_inventory as inventory  # noqa: E402


def load(relative):
    return json.loads((PACKAGE / relative).read_text(encoding="utf-8"))


def source_bytes(record, root: pathlib.Path) -> bytes:
    path = PACKAGE / record["package_file"] if "package_file" in record else root / record["path"]
    data = path.read_bytes()
    if inventory.sha256(data) != record["sha256"]:
        raise ValueError(f"source hash mismatch: {path}")
    return data


def truth_table(expression: str) -> str:
    """The SDK's documented encoding: bit i is set when the expression holds
    with A = bit 0, B = bit 1, C = bit 2 and D = bit 3 of i."""
    expressions = {
        "A": lambda a, b: a,
        "!A": lambda a, b: not a,
        "A & B": lambda a, b: a and b,
        "A | B": lambda a, b: a or b,
        "A ^ B": lambda a, b: a != b,
        "!(A & B)": lambda a, b: not (a and b),
        "!(A | B)": lambda a, b: not (a or b),
        "!(A ^ B)": lambda a, b: a == b,
    }
    operation = expressions[expression]
    value = sum(1 << i for i in range(16) if operation(bool(i & 1), bool(i & 2)))
    return f"0x{value:04X}"


class Stage:
    def __init__(self, graph, sources, layer, inv):
        self.graph, self.sources, self.layer, self.inv = graph, sources, layer, inv
        self.value = graph.value
        self.prefix = inv["resource_prefix"]
        self.conditions = inv["conditions"]
        self.target = layer["target"]["id"]
        self.resources = {record["id"]: record for record in self.value["resources"]}
        self.evidence = {record["id"]: record for record in self.value["evidence"]}

    # -- citations ---------------------------------------------------------
    def locate(self, source, text, nth=None, regex=False, after=0):
        lines = source.lines
        hits = [i + 1 for i in range(after, len(lines))
                if (re.search(text, lines[i]) if regex else text in lines[i])]
        if regex and after:
            return hits[0] if hits else None
        if not regex and nth is None and len(hits) != 1:
            raise SystemExit(f"{source.id}: expected one line containing {text!r}, found {len(hits)}")
        nth = nth or 0
        if len(hits) <= nth:
            raise SystemExit(f"{source.id}: fewer than {nth + 1} lines containing {text!r}")
        return hits[nth]

    def cite_spec(self, spec):
        source = self.sources[spec["source"]]
        start = self.locate(source, spec["start"], spec.get("nth"))
        end = start
        if "end" in spec:
            if spec.get("regex_end"):
                end = self.locate(source, spec["end"], regex=True, after=start)
            else:
                hits = [i + 1 for i in range(start - 1, len(source.lines)) if spec["end"] in source.lines[i]]
                if not hits:
                    raise SystemExit(f"{spec['id']}: end not found")
                end = hits[0]
        return self.graph.cite(spec["id"], source, start, end, spec["statement"])

    def template(self, identifier, source_id, statement):
        source = self.sources[source_id]
        cited = self.graph.cite(identifier, source, 1, len(source.lines), statement)
        self.evidence[cited] = self.value["evidence"][-1]
        self.evidence[cited]["role"] = "template"
        return cited

    # -- stage ---------------------------------------------------------------
    def run(self):
        layer = self.layer
        labels = {record["id"]: record["label"] for record in layer["sources"] if "label" in record}
        self.graph.labels = labels
        policy = layer["policy"]
        policy_source = self.sources[policy["source"]]
        for rule in range(1, policy["rules"] + 1):
            line = rule + 1
            if not policy_source.lines[line - 1].startswith(f"{rule}. "):
                raise SystemExit(f"policy line {line} is not rule {rule}")
            self.graph.cite(f"{policy['prefix']}-{rule}", policy_source, line, line,
                            f"{policy['label']}, rule {rule}: a BKPT decision, not a manufacturer statement.")
        for spec in layer["cites"]:
            self.cite_spec(spec)
        for kind in ("definition", "value"):
            for source_id, symbol in layer["symbols"][kind]:
                start, end, found = inventory.declaration(self.sources[source_id], symbol)
                if kind == "definition" and found != "definition":
                    raise SystemExit(f"{symbol}: {source_id} holds a {found}, not its definition")
                self.graph.cite_symbol("sdk", self.sources[source_id], symbol)
        self.check_truth_tables()
        board = self.board()
        self.target_record(board)
        self.play_resources()
        self.pad_rule(board)
        self.cards()
        self.capabilities()

    def check_truth_tables(self):
        for name, entry in self.layer["truth_tables"].items():
            computed = truth_table(entry["expression"])
            if computed != entry["table"]:
                raise SystemExit(f"truth table of {name} ({entry['expression']}) is {computed}, input says {entry['table']}")

    def board(self):
        """Board parts on device package positions, each cited at its block of
        the captured board composition."""
        spec = self.layer["board"]
        source = self.sources[spec["source"]]
        start = self.locate(source, spec["section"])
        end = next(i + 1 for i in range(start, len(source.lines)) if source.lines[i].endswith('"""'))
        text = "\n".join(source.lines[start:end - 1] + [source.lines[end - 1][:-3]])
        composition = json.loads(text)
        opener = next(i + 1 for i in range(start, end) if source.lines[i].strip() == '"constraints": [')
        closer = next(i + 1 for i in range(opener, end) if source.lines[i] in ("  ]", "  ],"))
        whole = self.graph.cite("board-composition", source, opener, closer, spec["composition_statement"])
        blocks, depth, block_start = [], 0, None
        for number in range(opener + 1, closer):
            line = source.lines[number - 1]
            if depth == 0 and line.strip() == "{":
                block_start = number
            depth += line.count("{") + line.count("[") - line.count("}") - line.count("]")
            if depth == 0 and block_start is not None:
                blocks.append((block_start, number))
                block_start = None
        if len(blocks) != len(composition["constraints"]):
            raise SystemExit("board composition blocks and constraints disagree")
        parts = {}
        for (first, last), constraint in zip(blocks, composition["constraints"]):
            part = constraint["part"]
            if not part.get("enabled", False):
                continue
            position = constraint["device"]["pin"]
            identifier = f"board-{part['reference']}-{part['pin']}"
            cited = self.graph.cite(identifier, source, first, last, spec["statement"].format(
                reference=part["reference"], name=part["name"], part_pin=part["pin"], position=position))
            parts.setdefault(position, []).append((part["reference"], cited))
        return {"parts": parts, "composition": whole}

    def pin(self, pad):
        return self.resources[f"{self.prefix}.{pad}"]

    def target_record(self, board):
        spec = self.layer["target"]
        target = next(record for record in self.value["targets"] if record["id"] == self.target)
        target["evidence"] = list(dict.fromkeys(target["evidence"] + spec["evidence"]))
        for assumption in spec["assumptions"]:
            target["assumptions"].append(assumption)
        for entry in self.layer["reservations"]:
            pin = self.pin(entry["pad"])
            position = pin["attributes"]["package_position"]
            held = board["parts"].get(position)
            if not held:
                raise SystemExit(f"reservation {entry['pad']}: no board part on position {position}")
            target["reservations"].append({"resource": pin["id"], "count": 1, "adoptable_by": []})
            target["evidence"].extend(cited for _, cited in held if cited not in target["evidence"])
            target["evidence"].extend(e for e in entry["evidence"] if e not in target["evidence"])

    def play_resources(self):
        play = self.layer["play"]
        instance_id = f"{self.prefix}.{play['instance']}"
        record = self.resources[instance_id]
        if record["kind"] != "descriptor_instance":
            raise SystemExit(f"{instance_id} is not an unpromoted descriptor instance")
        record["kind"] = "play"
        record["capacity"] = play["count"]
        record["attributes"] = dict(play["instance_attributes"])
        record["evidence"] = list(dict.fromkeys(record["evidence"] + play["instance_evidence"]))
        for name, spec in play["kinds"].items():
            for n in range(play["count"]):
                attributes = {key: value.format(n=n) for key, value in spec["attributes"].items()}
                evidence = [self.graph.cite_symbol("sdk", self.sources["play-h"], symbol)
                            for key, symbol in attributes.items() if key.endswith("_symbol")]
                attributes["ownership_policy"] = "exclusive"
                self.graph.resource(f"{instance_id}.{spec['id'].format(n=n)}", spec["kind"], attributes,
                                    self.conditions, evidence + ["policy-2", "policy-5"])
        self.resources = {r["id"]: r for r in self.value["resources"]}
        for edge in [e for e in self.value["edges"] if e["from"] == instance_id and e["relation"] == "signal"]:
            signal = edge["attributes"]["signal"]
            match = re.fullmatch(r"PLAY1_(IN|OUT)(\d+)", signal)
            if not match:
                continue
            direction, n = match.groups()
            sub = f"{instance_id}.{direction}{n}"
            if sub not in self.resources:
                raise SystemExit(f"route {edge['id']} names {sub}, which the SDK does not enumerate")
            pad = edge["to"].split(".", 1)[1]
            self.graph.edge(f"play.{direction}{n}.{pad}", sub, edge["to"], play["route_relation"],
                            {"signal": signal, "alternate": edge["attributes"]["alternate"]},
                            self.conditions, edge["evidence"] + ["policy-3"])

    def pad_rule(self, board):
        """Policy rule 4: a PLAY pad of this board is one whose package
        position carries no board part; the user button is allowed for inputs."""
        play = self.layer["play"]
        allowed_input_parts = set(self.layer["board"]["input_parts"])
        for edge in [e for e in self.value["edges"] if e["relation"] == play["route_relation"]]:
            direction = "input" if ".IN" in edge["from"] else "output"
            pin = self.resources[edge["to"]]
            position = pin["attributes"]["package_position"]
            parts = board["parts"].get(position, [])
            if parts and not (direction == "input" and {ref for ref, _ in parts} <= allowed_input_parts):
                continue
            pin["attributes"][play["pad_attribute"][direction]] = "true"
            cites = [cited for _, cited in parts] or [board["composition"]]
            for identifier in cites + ["policy-4", "board-identity"]:
                if identifier not in pin["evidence"]:
                    pin["evidence"].append(identifier)

    def foundation_edit(self, edit):
        manifest = json.loads((PACKAGE / self.layer["target"]["foundation_manifest"]).read_text(encoding="utf-8"))
        entry = next((f for f in manifest["files"] if f["destination"] == edit["path"]), None)
        if entry is None:
            raise SystemExit(f"foundation has no {edit['path']}")
        data = (self.root / entry["source"]).read_bytes()
        if inventory.sha256(data) != entry["sha256"]:
            raise SystemExit(f"foundation input {entry['source']} differs from its manifest hash")
        if data.decode("utf-8").count(edit["old"]) != 1:
            raise SystemExit(f"edit anchor is not unique in {edit['path']}: {edit['old'][:60]!r}")
        record = {"path": edit["path"], "expected_sha256": entry["sha256"], "old": edit["old"], "new": edit["new"]}
        if edit.get("mode", "insert_before") == "insert_before":
            record["mode"] = "insert_before"
        return record

    def cards(self):
        spec = self.layer["integration"]
        self.value["implementations"].append({
            "id": spec["id"], "target": self.target, "guidance": spec["guidance"],
            "evidence": spec["evidence"], "edits": [self.foundation_edit(edit) for edit in spec["edits"]]})
        unit_edits = [self.foundation_edit(edit) for edit in self.layer["unit_edits"]]
        maps = self.layer["maps"]
        tables = self.layer["truth_tables"]
        for capability in self.layer["capabilities"]:
            files = []
            for template in capability["templates"]:
                kind = "header" if template.endswith("-h") else "source"
                cited = self.template(template, template,
                                      f"BKPT-authored {kind} template of the {capability['id']} unit; an implementation, not manufacturer evidence.")
                files.append({"path": "bkpt_components/{{unit}}." + ("h" if kind == "header" else "c"), "template": cited})
            bindings = {
                "unit": {"source": {"type": "instance"}},
                "function": {"source": {"type": "parameter", "name": "function"}},
                "truth_table": {"source": {"type": "parameter", "name": "function"},
                                "map": {name: tables[name]["table"] for name in capability["functions"]}},
                "min_pulse_width": {"source": {"type": "parameter", "name": "min_pulse_width"}},
                "lut_symbol": self.attribute("lut", "lut_symbol"), "lut_name": self.attribute("lut", "name"),
                "lut_direct": self.attribute("lut", "direct_output_symbol"),
                "out_symbol": self.attribute("out", "output_symbol"), "out_name": self.attribute("out", "name"),
            }
            claims = ["play1.{{lut_name}}", "play1.{{out_name}}", "pad.{{pad_out_name}}"]
            pads = ["out"]
            for letter in capability["inputs"]:
                role = f"in_{letter}"
                bindings.update({
                    f"{letter}_source": self.attribute(role, "source_symbol"),
                    f"{letter}_lut_source": self.attribute(role, "lut_source_symbol"),
                    f"{letter}_mux": self.attribute(role, "mux_symbol"),
                    f"{letter}_name": self.attribute(role, "name"),
                    f"pull_{letter}": {"source": {"type": "parameter", "name": f"pull_{letter}"}, "map": maps["pull"]},
                })
                claims += [f"play1.{{{{{letter}_name}}}}", f"pad.{{{{pad_{letter}_name}}}}"]
                pads.append(letter)
            for letter in pads:
                role = f"pad_{letter}"
                bindings.update({
                    f"pad_{letter}_port": self.attribute(role, "port_symbol"),
                    f"pad_{letter}_pin": self.attribute(role, "pin_symbol"),
                    f"pad_{letter}_clock": self.attribute(role, "clock_enable_api"),
                    f"pad_{letter}_name": self.attribute(role, "pin"),
                    f"pad_{letter}_af": {"source": {"type": "route_attribute",
                                                    "from": "out" if letter == "out" else f"in_{letter}", "to": role,
                                                    "relation": self.layer["play"]["route_relation"], "attribute": "alternate"},
                                         "map": maps["alternate"]},
                })
            if capability["trigger"]:
                bindings.update({"trigger_symbol": self.attribute("trigger", "trigger_symbol"),
                                 "trigger_lut_source": self.attribute("trigger", "lut_source_symbol"),
                                 "trigger_name": self.attribute("trigger", "name")})
                claims.append("play1.{{trigger_name}}")
            interface = self.interface(capability)
            evidence = list(dict.fromkeys(capability["evidence"] + self.layer["common_unit_evidence"]))
            self.value["implementations"].append({
                "id": capability["card"], "target": self.target, "guidance": capability["guidance"].replace("{{unit}}", "the unit"),
                "evidence": evidence, "edits": [],
                "component": {"bindings": bindings, "files": files, "edits": copy.deepcopy(unit_edits),
                              "claims": claims, "interface": interface}})

    @staticmethod
    def attribute(role, attribute):
        return {"source": {"type": "resource_attribute", "role": role, "attribute": attribute}}

    def interface(self, capability):
        pads_in = " and ".join(f"{{{{pad_{x}_name}}}} ({x.upper()}, PLAY1 {{{{{x}_name}}}}, pull {{{{pull_{x}}}}})"
                               for x in capability["inputs"])
        inputs = "A on lookup table input 0" + (", B on input 1" if len(capability["inputs"]) == 2 or capability["trigger"] else "")
        fields = {
            "setup": ("Initialized from main after mx_system_init by {{unit}}_init(); a failure makes main return -1. "
                      "Header bkpt_components/{{unit}}.h. No manual PLAY1, PLAY clock, LSI or pad setup."),
            "pads": (f"In: {pads_in}. Out: {{{{pad_out_name}}}} through PLAY1 {{{{out_name}}}} from {{{{lut_name}}}}'s direct output. "
                     "Alternate function 14, high speed, push-pull; the output pad is connected only after PLAY1 starts."),
            "function": (f"{{{{function}}}} by truth table {{{{truth_table}}}}, {inputs}"
                         + ("; B is software trigger {{trigger_name}}" if capability["trigger"] else "")
                         + ". Unused table inputs keep the SDK default source; the table ignores them."),
            "filter": ("Each pad input passes a PLAY1 input filter with a minimum pulse width of {{min_pulse_width}} cycles of "
                       "the PLAY1 kernel clock, LSI with divider 1, edge detection bypassed. No timing is claimed."),
            "scope": ("PLAY1 is shared by generated PLAY units only: nothing else may configure PLAY1, its kernel clock or these "
                      "pads. Each unit's init stops and restarts PLAY1. No registered outputs, clock gates, edge triggers, "
                      "interrupts, low-power or security attributes; no timing, glitch or electrical behavior established."),
            "declarations": "hal_status_t {{unit}}_init(void);",
        }
        if capability["trigger"]:
            fields["trigger"] = ("{{unit}}_set_trigger(level), 0 or 1: HAL_OK once the SDK reports the write done; HAL_BUSY "
                                 "while any PLAY1 software trigger write is pending; HAL_TIMEOUT after 10 ms of HAL ticks. "
                                 "{{unit}}_get_trigger(&level) reads it. Only after init succeeds, one thread, never from an "
                                 "interrupt; set triggers after every PLAY unit is initialized.")
            fields["declarations"] += ("\nhal_status_t {{unit}}_set_trigger(uint32_t level);"
                                       "\nhal_status_t {{unit}}_get_trigger(uint32_t *level);")
        return fields

    def capabilities(self):
        params = self.layer["parameters"]
        relation = self.layer["play"]["route_relation"]
        attribute = self.layer["play"]["pad_attribute"]
        for capability in self.layer["capabilities"]:
            parameters = {"function": {"type": "choice", "values": capability["functions"]},
                          "min_pulse_width": params["min_pulse_width"]}
            roles = [{"id": "play", "kind": "play", "attributes": {}, "parameter_attributes": {}, "count": 1,
                      "mode": "bkpt_play_units"}]
            links = []
            for letter in capability["inputs"]:
                parameters[f"pull_{letter}"] = params["pull"]
                roles += [{"id": f"in_{letter}", "kind": "play_input", "attributes": {}, "parameter_attributes": {}, "count": 1},
                          {"id": f"pad_{letter}", "kind": "pin", "attributes": {attribute["input"]: "true"},
                           "parameter_attributes": {}, "count": 1}]
                links.append({"from": f"in_{letter}", "to": f"pad_{letter}", "relation": relation})
            if capability["trigger"]:
                roles.append({"id": "trigger", "kind": "play_sw_trigger", "attributes": {}, "parameter_attributes": {}, "count": 1})
            roles += [{"id": "lut", "kind": "play_lut", "attributes": {}, "parameter_attributes": {}, "count": 1},
                      {"id": "out", "kind": "play_output", "attributes": {}, "parameter_attributes": {}, "count": 1},
                      {"id": "pad_out", "kind": "pin", "attributes": {attribute["output"]: "true"},
                       "parameter_attributes": {}, "count": 1}]
            links.append({"from": "out", "to": "pad_out", "relation": relation})
            evidence = list(dict.fromkeys(["policy-2", "policy-3", "policy-4", "policy-5", "policy-7", "play-truth-formula",
                                           "play-min-pulse-range", "board-composition", "board-identity"] + capability["evidence"]))
            self.value["capabilities"].append({
                "id": capability["id"], "target": self.target, "description": capability["description"],
                "parameters": dict(sorted(parameters.items())), "roles": roles, "links": links,
                "implementations": [self.layer["integration"]["id"], capability["card"]],
                "domain_complete": False, "closure_evidence": [], "evidence": evidence})


def first_layer(layer, inv, root: pathlib.Path):
    sources = []
    for record in layer["sources"]:
        data = source_bytes(record, root)
        sources.append({"id": record["id"], "path": record["path"], "sha256": inventory.sha256(data),
                        "revision": record["revision"], "authority": record["authority"], "scope": record["scope"]})
    spec = layer["target"]
    manifest = (PACKAGE / spec["foundation_manifest"]).read_bytes()
    target = {"id": spec["id"], "board": spec["board"], "part": spec["part"], "package": spec["package"],
              "sdk": spec["sdk"], "foundation": spec["foundation"], "foundation_sha256": inventory.sha256(manifest),
              "context": dict(spec["context"]), "reservations": [], "assumptions": [], "evidence": []}
    return {"schema": 4, "id": layer["id"], "sources": sources, "evidence": [], "targets": [target],
            "resources": [], "edges": [], "capabilities": [], "implementations": []}


def generate(root: pathlib.Path):
    layer, inv, brief_config = load("imports/play-layer.json"), load("imports/device-inventory.json"), load("imports/capability-briefs.json")
    base = first_layer(layer, inv, root)
    graph, sources, summary = inventory.build_from(inv, base, root)
    inventory.use_schema(graph.value, layer["graph_schema"])
    for record in layer["sources"]:
        if record["id"] not in sources:
            pinned = next(s for s in graph.value["sources"] if s["id"] == record["id"])
            sources[record["id"]] = inventory.Source(pinned, data=source_bytes(record, root))
    stage = Stage(graph, sources, layer, inv)
    stage.root = root
    stage.run()
    add_profiles(graph, load("imports/hal2-units.json"), source_bytes, root, inventory)
    briefs.run(graph, summary, brief_config)
    target = graph.value["targets"][0]
    if not target["evidence"]:
        raise SystemExit("target cites nothing")
    return graph, summary


def stage_root(root: pathlib.Path, folder: pathlib.Path):
    """A source root for `bkpt-fw architecture build`, which reads every
    source as a file inside one folder: a copy of each pinned source at its
    graph path. Use a temporary folder; nothing here belongs in the package."""
    if folder.exists() and any(folder.iterdir()):
        raise ValueError("stage the sources in an empty directory")
    records = load("imports/play-layer.json")["sources"] + load("imports/hal2-units.json")["sources"]
    for record in records:
        path = pathlib.PurePosixPath(record["path"])
        if path.is_absolute() or ".." in path.parts or "\\" in record["path"] or ":" in record["path"]:
            raise ValueError(f"unsafe source path: {path}")
        destination = folder / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as output:
            output.write(source_bytes(record, root))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--source-root", type=pathlib.Path, default=os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT"))
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--out", type=pathlib.Path)
    group.add_argument("--check", action="store_true")
    parser.add_argument("--stage-root", type=pathlib.Path)
    arguments = parser.parse_args()
    if arguments.source_root is None:
        raise SystemExit("give --source-root or set BKPT_KNOWLEDGE_SOURCE_ROOT")
    graph, summary = generate(arguments.source_root)
    data = inventory.encoded(graph)
    if arguments.stage_root:
        stage_root(arguments.source_root, arguments.stage_root)
    if arguments.check:
        current = (PACKAGE / "graph.json").read_bytes()
        if current != data:
            raise SystemExit(f"graph.json differs from the regenerated graph (sha256 {inventory.sha256(data)})")
        print(json.dumps({"graph": "unchanged", "sha256": inventory.sha256(data)}))
        return
    if arguments.out.exists():
        raise SystemExit("refusing to overwrite an existing graph")
    arguments.out.write_bytes(data)
    value = graph.value
    print(json.dumps({"sha256": inventory.sha256(data), "added": graph.added, **summary,
                      "totals": {key: len(value[key]) for key in ("sources", "evidence", "resources", "edges",
                                                                   "capabilities", "implementations")}}, indent=1))


if __name__ == "__main__":
    sys.exit(main())
