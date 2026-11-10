# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Extend a reviewed capability graph with a whole-device inventory.

The inventory is a literal transcription of machine-readable manufacturer
sources that the base graph already pins:

* every configurable pad of the pinout descriptor becomes a pin resource;
* every non-GPIO signal of the descriptor becomes a route from its instance to
  its pad, carrying the signal name, its type and its alternate-function id;
* every external interrupt of the device header becomes an interrupt resource
  with the handler the startup vector table places in that slot.

Nothing is inferred from a name. Each generated record cites the exact source
lines it was read from, and the build refuses a source whose hash differs from
the one the base graph recorded. A signal keeps its whole descriptor name: what
the name means is for a reviewed capability to say, not for this tool.

Existing sources, evidence, edges, capabilities and implementations keep their
values. An existing pin gains only the descriptor attributes it lacked, and the
target gains one assumption. The output is a candidate: it needs an independent
review before `bkpt-fw architecture build` will accept it.

Usage:
    python author_device_inventory.py --config CONFIG.json --source-root ROOT --out NEW.json
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys

# A line that starts with one of these is a statement, never a declaration.
STATEMENT_KEYWORDS = "return|if|else|while|for|switch|case|do|goto|sizeof|typedef"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Source:
    """A pinned source file, addressed by one-based line like the catalog builder."""

    def __init__(self, record, root: pathlib.Path = None, data: bytes = None):
        self.id = record["id"]
        if data is None:
            data = (root / record["path"]).read_bytes()
        if sha256(data) != record["sha256"]:
            raise SystemExit(f"source {self.id} differs from the hash the graph pins")
        # The catalog builder splits on LF and drops one trailing CR per line.
        self.lines = [line[:-1] if line.endswith("\r") else line for line in data.decode("utf-8").split("\n")]
        if self.lines and self.lines[-1] == "":
            self.lines.pop()

    def excerpt(self, start: int, end: int) -> str:
        return "\n".join(self.lines[start - 1:end])

    def find(self, pattern: str):
        """The one line matching `pattern`. More or fewer than one is an error."""
        matches = [index + 1 for index, line in enumerate(self.lines) if re.search(pattern, line)]
        if len(matches) != 1:
            raise SystemExit(f"{self.id}: expected one line matching {pattern!r}, found {len(matches)}")
        return matches[0]


def children(source: Source, key: str):
    """Yield (start_line, end_line, value, name) for each direct child of a
    top-level array or object in a pretty-printed JSON source."""
    opener = re.compile(r'^  "' + re.escape(key) + r'": ([\[{])\s*$')
    start = next((i for i, line in enumerate(source.lines) if opener.match(line)), None)
    if start is None:
        raise SystemExit(f"{source.id}: no top-level {key!r} section")
    depth, child_start, in_string, escaped = 0, None, False, False
    for index in range(start + 1, len(source.lines)):
        line = source.lines[index]
        if depth == 0 and (line.startswith("  ]") or line.startswith("  }")):
            return
        if depth == 0:
            child_start = index
        for character in line:
            if in_string:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == '"':
                    in_string = False
            elif character == '"':
                in_string = True
            elif character in "{[":
                depth += 1
            elif character in "}]":
                depth -= 1
        if depth == 0:
            text = "\n".join(source.lines[child_start:index + 1]).strip().rstrip(",")
            name = None
            named = re.match(r'^"((?:[^"\\]|\\.)*)":\s*(.*)$', text, re.S)
            if named:
                name, text = json.loads('"' + named.group(1) + '"'), named.group(2)
            yield child_start + 1, index + 1, json.loads(text), name
    raise SystemExit(f"{source.id}: unterminated {key!r} section")


def declaration(source: Source, symbol: str):
    """The lines that declare an SDK symbol, with what the source says about it.

    Returns (start_line, end_line, kind):
    "value": an enumerator or macro, one line carrying its own description, or
        two when the description is a one-line comment directly above it;
    "prototype": a function declared without a body;
    "definition": a function cited through the end of its body.
    A function is cited from its documentation comment when one ends on the
    line above. Only a comment block that closes on its own line counts, so a
    trailing remark on a preprocessor line is never mistaken for one."""
    name = re.escape(symbol)
    value = re.compile(rf"^\s*(?:#define\s+{name}\b|{name}\s*=)")
    function = re.compile(
        rf"^(?!(?:{STATEMENT_KEYWORDS})\b)(?:__STATIC_INLINE\s+)?[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*[\s\*]+{name}\s*\(")
    matches = [(index + 1, bool(function.match(line)))
               for index, line in enumerate(source.lines) if value.match(line) or function.match(line)]
    if len(matches) != 1:
        raise SystemExit(f"{source.id}: expected one declaration of {symbol}, found {len(matches)}")
    line, is_function = matches[0]
    if not is_function:
        above = source.lines[line - 2].strip() if line >= 2 else ""
        # A banner or separator comment is not a description of the next line.
        if "/*" not in source.lines[line - 1] and above.startswith(("/**", "/*!")) and above.endswith("*/"):
            return line - 1, line, "value"
        return line, line, "value"
    start = line
    if line >= 2 and source.lines[line - 2].strip() == "*/":
        start = line - 1
        while start > 1 and not source.lines[start - 1].lstrip().startswith("/*"):
            start -= 1
    end = line
    while end <= len(source.lines):
        text = re.sub(r"\s*(?:/\*.*?\*/|//.*)\s*$", "", source.lines[end - 1]).rstrip()
        if text.endswith("{"):
            # The body opens on its own line or at the end of the signature.
            kind = "definition"
            while end < len(source.lines) and source.lines[end - 1] != "}":
                end += 1
            if source.lines[end - 1] != "}":
                raise SystemExit(f"{source.id}: no end of {symbol}")
            break
        if text.endswith(";"):
            kind = "prototype"
            break
        end += 1
    else:
        raise SystemExit(f"{source.id}: no end of the declaration of {symbol}")
    if end - start >= 400:
        raise SystemExit(f"{source.id}: declaration of {symbol} is too long to cite")
    return start, end, kind


class Graph:
    def __init__(self, base, target, sources):
        self.value = base
        self.target = target
        self.sources = sources
        self.evidence_ids = {record["id"] for record in base["evidence"]}
        self.resource_ids = {record["id"] for record in base["resources"]}
        self.edge_ids = {record["id"] for record in base["edges"]}
        self.added = {"evidence": 0, "resources": 0, "edges": 0}

    def cite(self, identifier, source, start, end, statement):
        if identifier in self.evidence_ids:
            existing = next(record for record in self.value["evidence"] if record["id"] == identifier)
            if (existing["source"], existing["start_line"], existing["end_line"]) != (source.id, start, end):
                raise SystemExit(f"evidence {identifier} already cites another place")
            return identifier
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", identifier):
            raise SystemExit(f"invalid evidence id {identifier!r}")
        self.value["evidence"].append({
            "id": identifier,
            "source": source.id,
            "start_line": start,
            "end_line": end,
            "excerpt_sha256": sha256(source.excerpt(start, end).encode("utf-8")),
            "statement": statement,
            "targets": [self.target],
        })
        self.evidence_ids.add(identifier)
        self.added["evidence"] += 1
        return identifier

    def cite_symbol(self, prefix, source, symbol):
        start, end, kind = declaration(source, symbol)
        described = any("/*" in line for line in source.lines[start - 1:end])
        if kind == "definition":
            statement = (f"SDK source documents and defines {symbol} in these lines." if described
                         else f"SDK source defines {symbol} in these lines.")
        elif kind == "prototype":
            statement = f"SDK header declares the prototype of {symbol}; the excerpt does not contain its definition."
        else:
            statement = (f"SDK source declares {symbol} with its description." if described
                         else f"SDK source declares {symbol} on this line; the line carries no description.")
        return self.cite(f"{prefix}-{symbol}", source, start, end, statement)

    def resource(self, identifier, kind, attributes, conditions, evidence):
        if identifier in self.resource_ids:
            return False
        self.value["resources"].append({
            "id": identifier,
            "target": self.target,
            "kind": kind,
            "capacity": 1,
            "attributes": attributes,
            "conditions": dict(conditions),
            "evidence": evidence,
            "implementation": None,
        })
        self.resource_ids.add(identifier)
        self.added["resources"] += 1
        return True

    def edge(self, identifier, source, destination, relation, attributes, conditions, evidence):
        if identifier in self.edge_ids:
            raise SystemExit(f"route {identifier} already exists")
        record = {
            "id": identifier,
            "target": self.target,
            "from": source,
            "to": destination,
            "relation": relation,
            "conditions": dict(conditions),
            "evidence": evidence,
        }
        if attributes:
            record["attributes"] = attributes
        self.value["edges"].append(record)
        self.edge_ids.add(identifier)
        self.added["edges"] += 1


def complete(record, attributes, evidence):
    """Give an existing pin the descriptor attributes it lacks. A value it
    already has must agree: two sources disagreeing is not ours to settle."""
    for key, value in attributes.items():
        if record["attributes"].setdefault(key, value) != value:
            raise SystemExit(f"{record['id']}: reviewed {key} disagrees with the descriptor")
    for identifier in evidence:
        if identifier not in record["evidence"]:
            record["evidence"].append(identifier)
    if len(record["evidence"]) > 64:
        raise SystemExit(f"{record['id']} cites more than 64 sources")


def pins_and_routes(graph: Graph, config, sources):
    pinout = sources[config["pinout"]["source"]]
    prefix, conditions = config["resource_prefix"], config["conditions"]
    symbols = config["gpio_symbols"]
    positions = {}
    for start, end, value, _ in children(pinout, "bonds"):
        # A supply pad is bonded to several positions; a configurable pad to one.
        positions.setdefault(value["die_pad"], []).append((start, end, value["position"]))
    pad_names = {}
    pad_blocks = {}
    for start, end, value, name in children(pinout, "die_pads"):
        pad_blocks[name] = (start, end, value)
        if value["type"] == "gpio":
            pad_names[name] = config["pad_name"].format(port=value["port"], index=value["index"])
    if len(set(pad_names.values())) != len(pad_names):
        raise SystemExit("two pads share one port and index")
    signals = list(children(pinout, "signals"))
    gpio_signal = {value["die_pad"]: (start, end)
                   for start, end, value, _ in signals if value["function"]["type"] == "user_gpio"}

    def symbol(kind, pad):
        spec = symbols[kind]
        text = spec["symbol"].format(port=pad["port"], index=pad["index"])
        return text, graph.cite_symbol(f"inv-{kind}", sources[spec["source"]], text)

    existing = {record["id"]: record for record in graph.value["resources"]}
    for pad, name in sorted(pad_names.items(), key=lambda item: (pad_blocks[item[0]][2]["port"], pad_blocks[item[0]][2]["index"])):
        start, end, value = pad_blocks[pad]
        if len(positions.get(pad, [])) != 1 or pad not in gpio_signal:
            raise SystemExit(f"pad {pad} needs exactly one package position and a GPIO signal")
        bond = positions[pad][0]
        port_symbol, port_cite = symbol("port", value)
        pin_symbol, pin_cite = symbol("pin", value)
        clock_symbol, clock_cite = symbol("clock", value)
        # Electrical labels of the descriptor (I/O structure, supply) are left
        # out on purpose: they disagree with the datasheet for several pads and
        # must not be read as a voltage tolerance.
        attributes = {
            "pin": name,
            "port_symbol": port_symbol,
            "pin_symbol": pin_symbol,
            "clock_enable_api": clock_symbol,
            "package_position": bond[2],
        }
        if pad != name:
            attributes["descriptor_pad"] = pad
        evidence = [
            graph.cite(f"inv-pad-{name}", pinout, start, end,
                       f"Pinout descriptor declares die pad {pad} as a configurable I/O of port {value['port']}, index {value['index']}."),
            graph.cite(f"inv-bond-{name}", pinout, bond[0], bond[1],
                       f"Pinout descriptor bonds die pad {pad} to package position {bond[2]}."),
            graph.cite(f"inv-gpio-{name}", pinout, gpio_signal[pad][0], gpio_signal[pad][1],
                       f"Pinout descriptor declares a user GPIO signal on die pad {pad}."),
            port_cite, pin_cite, clock_cite,
        ]
        identifier = f"{prefix}.{name}"
        if identifier in existing:
            complete(existing[identifier], attributes, evidence)
        else:
            graph.resource(identifier, "pin", {**attributes, "ownership_policy": "exclusive"}, conditions, evidence)

    excluded = {(item["signal"], item["pad"]): item["reason"] for item in config.get("excluded_signals", [])}
    skipped = []
    for start, end, value, _ in signals:
        function = value["function"]
        if function["type"] == "user_gpio":
            continue
        pad, signal, instance = value["die_pad"], value["name"], value["instance"]
        if pad not in pad_names:
            raise SystemExit(f"signal {signal} is on a pad that is not a configurable I/O: {pad}")
        name = pad_names[pad]
        if (signal, name) in excluded:
            skipped.append({"signal": signal, "pad": name, "reason": excluded.pop((signal, name))})
            continue
        detail = f" as alternate function {function['id']}" if "id" in function else f" as an {function['type']} function"
        cite = graph.cite(
            f"inv-signal-{signal}-{name}", pinout, start, end,
            f"Pinout descriptor declares signal {signal} of instance {instance} on die pad {pad}{detail}.")
        # `descriptor_instance` says only that the descriptor names this
        # instance. A reviewed capability gives an instance its real kind.
        graph.resource(f"{prefix}.{instance}", "descriptor_instance", {"instance": instance}, conditions, [cite])
        attributes = {"signal": signal, "type": function["type"]}
        if "id" in function:
            attributes["alternate"] = function["id"]
        graph.edge(f"route.{signal}.{name}", f"{prefix}.{instance}", f"{prefix}.{name}",
                   "signal", attributes, conditions, [cite])
    if excluded:
        raise SystemExit(f"excluded signals not found in the descriptor: {sorted(excluded)}")
    return skipped


def interrupts(graph: Graph, config, sources):
    spec = config["interrupts"]
    header, startup = sources[spec["header"]], sources[spec["startup"]]
    prefix, conditions = config["resource_prefix"], config["conditions"]
    suffix = re.escape(spec["symbol_suffix"])
    end = header.find(r"^\}\s*" + re.escape(spec["enum_type"]) + r";")
    start = max(index + 1 for index, line in enumerate(header.lines[:end]) if re.match(r"^typedef enum\b", line))
    # A description may continue on following lines, so only the declaration is matched.
    entry = re.compile(r"^\s*(\w+)" + suffix + r"\s*=\s*(-?\d+)\s*,?\s*(?:/\*.*)?$")
    named = re.compile(r"^\s*\w+" + suffix + r"\b")
    declared_interrupts = []
    for line in range(start, end):
        text = header.lines[line - 1]
        match = entry.match(text)
        if match:
            declared_interrupts.append((match.group(1), int(match.group(2)), line))
        elif named.match(text):
            raise SystemExit(f"{header.id}: unreadable interrupt declaration on line {line}")
    # The vector table lists one handler per slot; external slot 0 follows the
    # last core exception entry named in the configuration.
    anchor = startup.find(r"^\s*" + re.escape(spec["last_core_vector"]) + r",\s*$")
    slots = []
    for line in range(anchor + 1, len(startup.lines) + 1):
        text = startup.lines[line - 1].strip()
        if text.startswith("}"):
            break
        match = re.fullmatch(r"(\w+),?", text)
        if not match:
            raise SystemExit(f"{startup.id}: unexpected vector table line {line}: {text!r}")
        slots.append((match.group(1), line))
    external = [item for item in declared_interrupts if item[1] >= 0]
    if len({number for _, number, _ in external}) != len(external):
        raise SystemExit("two interrupts share one number")
    handlers = sum(1 for handler, _ in slots if handler != "0")
    if handlers != len(external):
        raise SystemExit(f"{handlers} vector handlers for {len(external)} declared external interrupts")
    for name, number, line in external:
        if number >= len(slots):
            raise SystemExit(f"interrupt {name} has no vector slot {number}")
        handler, vector_line = slots[number]
        expected = name + spec["handler_suffix"]
        if handler != expected:
            raise SystemExit(f"vector slot {number} holds {handler}, not {expected}")
        symbol = name + spec["symbol_suffix"]
        evidence = [
            graph.cite(f"inv-irq-{name}", header, line, line,
                       f"Device header declares {symbol} with number {number}."),
            graph.cite(f"inv-vector-{name}", startup, vector_line, vector_line,
                       f"Captured startup vector table places {handler} in external slot {number}."),
        ]
        graph.resource(f"{prefix}.{name}.irq", "interrupt", {
            "irq_symbol": symbol,
            "irq_number": str(number),
            "handler_symbol": handler,
            "ownership_policy": "exclusive",
        }, conditions, evidence)
    return len(external), sum(1 for handler, _ in slots if handler == "0"), len(slots)


def build(config_path: pathlib.Path, source_root: pathlib.Path):
    """Run the inventory stage and return (graph, sources by id, summary).
    A later authoring stage may keep extending the returned graph."""
    config = json.loads(config_path.read_text(encoding="utf-8"))
    base_bytes = (config_path.parent / config["base"]["path"]).read_bytes()
    if sha256(base_bytes) != config["base"]["sha256"]:
        raise SystemExit("base graph differs from the configured hash")
    base = json.loads(base_bytes)
    base["schema"], base["id"] = 4, config["id"]
    wanted = {config["pinout"]["source"], config["interrupts"]["header"], config["interrupts"]["startup"],
              *(spec["source"] for spec in config["gpio_symbols"].values())}
    sources = {record["id"]: Source(record, source_root)
               for record in base["sources"] if record["id"] in wanted}
    if set(sources) != wanted:
        raise SystemExit(f"base graph does not pin: {sorted(wanted - set(sources))}")
    target = next(record for record in base["targets"] if record["id"] == config["target"])
    graph = Graph(base, config["target"], sources)
    skipped = pins_and_routes(graph, config, sources)
    external, holes, slots = interrupts(graph, config, sources)
    target["assumptions"].append(config["assumption"])
    return graph, sources, {"external_interrupts": external, "vector_slots": slots,
                            "empty_vector_slots": holes, "excluded_signals": skipped}


def write(graph: Graph, out: pathlib.Path, summary):
    if out.exists():
        raise SystemExit("refusing to overwrite an existing graph")
    value = graph.value
    out.write_bytes((json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
    counts = {key: len(value[key]) for key in ("sources", "evidence", "resources", "edges", "capabilities", "implementations")}
    print(json.dumps({"id": value["id"], "added": graph.added, "totals": counts, **summary,
                      "sha256": sha256(out.read_bytes())}, indent=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--config", type=pathlib.Path, required=True)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if arguments.out.exists():
        raise SystemExit("refusing to overwrite an existing graph")
    graph, _, summary = build(arguments.config, arguments.source_root)
    write(graph, arguments.out, summary)


if __name__ == "__main__":
    sys.exit(main())
