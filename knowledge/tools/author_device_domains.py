# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Open reviewed capabilities to the whole device, on top of the inventory.

The inventory stage records which pads, routes and interrupts are declared.
This stage decides which of them a capability may use, from inputs that are
all cited line by line:

* the board netlist, which says what else is connected to each device pad;
* the device description, which says which instances share a register layout
  and which interrupt each one raises;
* the DMA request table and the SDK definitions that must agree with it;
* an authored policy text, which says how those are turned into rules.

It then widens existing capabilities to that domain. A capability that needed a
fixed pad or instance now accepts any covered one, keeps the reviewed one as
its first choice, and reads the alternate function from the selected route.

Usage:
    python author_device_domains.py --inventory INVENTORY.json --domains DOMAINS.json \
        --source-root ROOT --out NEW.json
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_inventory as inventory  # noqa: E402
import json  # noqa: E402


def pin_name(pad):
    return f'P{pad["port"]}{pad["index"]}'


def find(records, identifier):
    matches = [record for record in records if record["id"] == identifier]
    if len(matches) != 1:
        raise SystemExit(f"expected one record {identifier!r}, found {len(matches)}")
    return matches[0]


def extend(record, evidence):
    for identifier in evidence:
        if identifier not in record["evidence"]:
            record["evidence"].append(identifier)
    if len(record["evidence"]) > 64:
        raise SystemExit(f"{record['id']} cites more than 64 sources")


def restate(owner, text, replacements):
    """Replace reviewed wording that named the old, narrower domain. Every
    anchor must be found exactly once, so a changed base cannot be missed."""
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f"{owner}: text to restate not found exactly once: {old[:60]!r}")
        text = text.replace(old, new)
    return text


def description_blocks(source):
    """Peripheral blocks of a device description: where each starts, which
    layout it derives from and which interrupts it declares."""
    blocks, current, opened = {}, None, None
    for number, line in enumerate(source.lines, 1):
        text = line.strip()
        if re.fullmatch(r"<peripheral(\s[^>]*)?>", text):
            derived = re.search(r'derivedFrom="([^"]+)"', text)
            current = {"start": number, "derived": derived.group(1) if derived else None,
                       "name": None, "interrupts": []}
        elif current is None:
            continue
        elif current["name"] is None:
            name = re.fullmatch(r"<name>([^<]+)</name>", text)
            if not name:
                raise SystemExit(f"{source.id}: peripheral at line {current['start']} does not start with its name")
            current["name"], current["name_line"] = name.group(1), number
        elif text == "<interrupt>":
            opened = number
        elif text == "</interrupt>":
            body = "".join(source.lines[opened - 1:number])
            name, value = re.search(r"<name>([^<]+)</name>", body), re.search(r"<value>(\d+)</value>", body)
            if not name or not value:
                raise SystemExit(f"{source.id}: unreadable interrupt at line {opened}")
            current["interrupts"].append({"start": opened, "end": number, "name": name.group(1), "value": value.group(1)})
        elif text == "</peripheral>":
            if current["name"] in blocks:
                raise SystemExit(f"{source.id}: peripheral {current['name']} is described twice")
            blocks[current["name"]] = current
            current = None
    return blocks


class Domains:
    def __init__(self, graph, sources, inventory_config, config, source_root):
        self.graph, self.sources, self.config = graph, sources, config
        self.source_root = source_root
        self.value = graph.value
        self.prefix = inventory_config["resource_prefix"]
        self.conditions = inventory_config["conditions"]
        self.target = find(self.value["targets"], inventory_config["target"])
        for record in config["sources"]:
            data = (source_root / record["path"]).read_bytes()
            pinned = {**record, "sha256": inventory.sha256(data)}
            pinned = {key: pinned[key] for key in ("id", "path", "sha256", "revision", "authority")}
            if any(existing["id"] == record["id"] for existing in self.value["sources"]):
                raise SystemExit(f"source {record['id']} is already pinned")
            self.value["sources"].append(pinned)
            sources[record["id"]] = inventory.Source(pinned, source_root)
        if len(self.value["sources"]) > 128:
            raise SystemExit("more than 128 sources")
        self.policy_source = sources[config["policy"]["source"]]
        pinout = sources[inventory_config["pinout"]["source"]]
        self.pads = {}
        for _, _, value, name in inventory.children(pinout, "die_pads"):
            if value["type"] == "gpio":
                self.pads[name] = value
        self.positions = {}
        for _, _, value, _ in inventory.children(pinout, "bonds"):
            self.positions.setdefault(value["position"], value["die_pad"])
        self.signals = [value for _, _, value, _ in inventory.children(pinout, "signals")]
        self.blocks = None

    def source(self, identifier):
        """A source the graph already pins, opened on first use."""
        if identifier not in self.sources:
            self.sources[identifier] = inventory.Source(find(self.value["sources"], identifier), self.source_root)
        return self.sources[identifier]

    def policy_text(self, name):
        return self.policy_source.lines[self.config["policy"]["lines"][name] - 1]

    def policy(self, name):
        line = self.config["policy"]["lines"][name]
        text = self.policy_source.lines[line - 1]
        return self.graph.cite(f"policy3-{name}", self.policy_source, line, line,
                               "BKPT policy, version 3: " + text.split(":", 1)[0] + ". Authored engineering policy, not a manufacturer statement.")

    def resource(self, name):
        return find(self.value["resources"], f"{self.prefix}.{name}")

    def name(self, identifier):
        return identifier.split(".", 1)[1]

    def board(self):
        """Classify every configurable pad by what its board net carries."""
        spec = self.config["board"]
        netlist = self.sources[spec["source"]]
        passive = re.compile(spec["passive_members"])
        nets = {}
        for start, end, value, _ in inventory.children(netlist, "nets"):
            for member in value["net"]:
                if member["reference"] != spec["device_reference"]:
                    continue
                pad = self.positions.get(member["pin"])
                if pad not in self.pads:
                    continue
                pin = pin_name(self.pads[pad])
                if pin in nets:
                    raise SystemExit(f"pad {pin} is on two board nets")
                others = sorted({m["reference"] for m in value["net"] if m["reference"] != spec["device_reference"]})
                nets[pin] = (start, end, value["netLabel"], others)
        if set(nets) != {pin_name(pad) for pad in self.pads.values()}:
            raise SystemExit("a configurable pad has no board net")
        self.eligible, self.net_cite = [], {}
        reservations = {record["resource"] for record in self.target["reservations"]}
        for pin, (start, end, label, others) in sorted(nets.items()):
            listed = ", ".join(others) if others else "nothing else"
            cite = self.graph.cite(
                f"net-{pin}", netlist, start, end,
                f"Board netlist places device pad {pin} on net {label} together with {listed}. Net membership only: conduction through bridges and component population are not stated.")
            self.net_cite[pin] = cite
            record = self.resource(pin)
            if label in spec["reserved_nets"]:
                rule = self.policy(spec["reserved_nets"][label])
                extend(record, [cite, rule])
                extend(self.target, [cite, rule])
                if record["id"] not in reservations:
                    self.target["reservations"].append({"resource": record["id"], "count": 1, "adoptable_by": []})
                continue
            if label in spec["excluded_nets"]:
                extend(record, [cite, self.policy(spec["excluded_nets"][label])])
                continue
            if all(passive.fullmatch(reference) for reference in others):
                record["attributes"].update(self.config["general_attribute"])
                extend(record, [cite, self.policy("general")])
                self.eligible.append(pin)
            else:
                extend(record, [cite, self.policy("general")])
        for old, new in self.config.get("assumption_replacements", []):
            # A reviewed assumption that names the old, narrower domain is restated.
            if self.target["assumptions"].count(old) != 1:
                raise SystemExit("assumption to restate not found exactly once")
            self.target["assumptions"][self.target["assumptions"].index(old)] = new
        self.target["assumptions"].append(self.config["assumption"])
        if len(self.target["assumptions"]) > 16:
            raise SystemExit("too many target assumptions")

    def net_member(self, label, reference, pin):
        """Cite one member of a board net, from the net's label to that member."""
        netlist = self.sources[self.config["board"]["source"]]
        found = [(start, end) for start, end, value, _ in inventory.children(netlist, "nets") if value["netLabel"] == label]
        if len(found) != 1:
            raise SystemExit(f"expected one board net {label}, found {len(found)}")
        start, end = found[0]
        member = re.compile(rf'"reference"\s*:\s*"{re.escape(reference)}"\s*,\s*"pin"\s*:\s*"{re.escape(pin)}"')
        lines = [number for number in range(start, end + 1) if member.search(netlist.lines[number - 1])]
        if len(lines) != 1:
            raise SystemExit(f"net {label} does not list {reference} pin {pin} exactly once")
        return self.graph.cite(f"net-{label}-{reference}.{pin}", netlist, start, lines[0],
                               f"Board netlist places {reference} pin {pin} on net {label}. Net membership only.")

    def allocatable(self, capability):
        """Eligible pads this capability could actually be given: a pad the
        base occupies counts only when the capability may adopt it."""
        blocked = {record["resource"] for record in self.target["reservations"]
                   if capability not in record["adoptable_by"]}
        return [pin for pin in self.eligible if f"{self.prefix}.{pin}" not in blocked]

    def choices(self, capability, parameter, pins):
        """Keep the reviewed order of existing choices, then add the rest."""
        values = capability["parameters"][parameter]["values"]
        missing = [pin for pin in values if pin not in pins]
        if missing:
            raise SystemExit(f"{capability['id']}: reviewed choices left the domain: {missing}")
        values.extend(pin for pin in pins if pin not in values)
        if len(values) > 128:
            raise SystemExit("choice domain exceeds 128 values")

    def gpio_output(self):
        spec = self.config["gpio_output"]
        capability = find(self.value["capabilities"], spec["capability"])
        rule = self.policy("gpio_output")
        pins = self.allocatable(capability["id"])
        for pin in pins:
            record = self.resource(pin)
            record["attributes"]["supports_gpio_output"] = "true"
            extend(record, [rule])
        self.choices(capability, "pin", pins)
        capability["description"] = spec["description"]
        extend(capability, [rule, self.policy("general")])
        card = find(self.value["implementations"], spec["card"])
        card["guidance"] = restate(card["id"], card["guidance"], spec["guidance"])
        extend(card, [rule])
        return len(pins)

    def gpio_interrupt(self):
        spec = self.config["gpio_interrupt"]
        capability = find(self.value["capabilities"], spec["capability"])
        rule = self.policy("gpio_interrupt")
        source = self.source(spec["exti_source"])
        features = spec.get("feature_pads", {})
        pins = [*self.allocatable(capability["id"]), *features]
        pads = {pin_name(pad): pad for pad in self.pads.values()}

        def symbol(template, pad):
            text = template.format(port=pad["port"], index=pad["index"])
            return text, self.graph.cite_symbol("inv-exti", source, text)

        for pin in pins:
            pad = pads[pin]
            record = self.resource(pin)
            port_symbol, port_cite = symbol(spec["port_symbol"], pad)
            record["attributes"]["supports_gpio_input_interrupt"] = "true"
            record["attributes"]["exti_port_symbol"] = port_symbol
            extend(record, [rule, port_cite])
            if pin in features:
                # A pad with a board circuit on it, admitted for this use only.
                feature = features[pin]
                record["attributes"].update(feature["attributes"])
                extend(record, [self.policy(feature["policy"]),
                                *(self.net_member(entry["net"], *entry["member"]) for entry in feature["circuit"])])
            line_symbol, line_cite = symbol(spec["line_symbol"], pad)
            interrupt = spec["interrupt"].format(index=pad["index"])
            line_id = f"{self.prefix}.{interrupt}"
            self.graph.resource(line_id, "exti_line", {"line_symbol": line_symbol, "ownership_policy": "exclusive"},
                                self.conditions, [line_cite, *spec["line_evidence"], rule])
            irq = find(self.value["resources"], f"{line_id}.irq")
            for key, value in spec["interrupt_attributes"].items():
                if irq["attributes"].setdefault(key, value) != value:
                    raise SystemExit(f"{irq['id']} already has another {key}")
            extend(irq, [rule])
            for identifier, start, end, relation, evidence in (
                (f"{pin}.exti-route", record["id"], line_id, "routes_to", [*spec["line_evidence"], line_cite]),
                (f"{interrupt}.irq-route", line_id, irq["id"], "raises_interrupt", [f"inv-irq-{interrupt}"]),
            ):
                if identifier not in self.graph.edge_ids:
                    self.graph.edge(identifier, start, end, relation, None, self.conditions, evidence)
        self.choices(capability, "pin", pins)
        capability["description"] = spec["description"]
        extend(capability, [rule, self.policy("general")])
        extend(find(self.value["implementations"], spec["card"]), [rule])
        return len(pins)

    def templates(self):
        """Pin authored templates as template-role excerpts."""
        for spec in self.config.get("templates", []):
            source = self.source(spec["source"])
            start, end = spec.get("start", 1), spec.get("end", len(source.lines))
            if spec["id"] in self.graph.evidence_ids:
                raise SystemExit(f"template {spec['id']} already exists")
            self.graph.cite(spec["id"], source, start, end, spec["statement"])
            find(self.value["evidence"], spec["id"])["role"] = "template"

    def route_functions(self):
        """Mark each listed signal route with the role it plays in a profile.

        The policy line is the authority: for every function its sentence must
        name exactly the signals configured here."""
        spec = self.config["route_functions"]
        rule, text = self.policy(spec["policy"]), self.policy_text(spec["policy"])
        functions, marked = {}, {}
        for function, signals in spec["functions"].items():
            sentences = [sentence for sentence in text.split(". ")
                         if sentence.endswith((f" are {function}", f" is {function}"))]
            if len(sentences) != 1:
                raise SystemExit(f"policy does not define function {function} exactly once")
            stated = set(re.findall(r"\b[A-Z][A-Z0-9]*_[A-Z0-9_]+\b", sentences[0]))
            if stated != set(signals):
                raise SystemExit(f"function {function}: policy names {sorted(stated)}, configuration {sorted(signals)}")
            for signal in signals:
                if functions.setdefault(signal, function) != function:
                    raise SystemExit(f"signal {signal} has two functions")
        for edge in self.value["edges"]:
            function = functions.get(edge.get("attributes", {}).get("signal")) if edge["relation"] == "signal" else None
            if function:
                edge["attributes"]["function"] = function
                extend(edge, [rule])
                marked[function] = marked.get(function, 0) + 1
        if set(marked) != set(spec["functions"]):
            raise SystemExit("a configured function marks no route")
        return marked

    def described(self, instance):
        if self.blocks is None:
            self.blocks = description_blocks(self.source(self.config["device_description"]["source"]))
        if instance not in self.blocks:
            raise SystemExit(f"device description has no peripheral {instance}")
        return self.blocks[instance]

    def request(self, instance, direction):
        """One DMA request line, recorded only when the manual's table and the
        SDK definitions for every controller agree on its selector."""
        spec = self.config["dma_requests"]
        identifier = f"{self.prefix}.{instance}.{direction}.request"
        if identifier in self.graph.resource_ids:
            return identifier
        rule = self.policy(spec["policy"])
        table = self.source(spec["table"]["source"])
        row = re.escape(spec["table"]["row"].format(selector="@", name=instance.lower(), direction=direction))
        pattern = re.compile("^" + row.replace("@", r"(\d+)") + r"\s*$")
        rows = [(number, match.group(1)) for number, line in enumerate(table.lines, 1)
                if (match := pattern.match(line))]
        if len(rows) != 1:
            raise SystemExit(f"{table.id}: expected one request row for {instance} {direction}, found {len(rows)}")
        line, selector = rows[0]
        evidence = [self.graph.cite(
            f"request-row-{instance}-{direction}", table, line, line,
            f"Reference manual request table row: selector {selector} is the {instance} {direction} request. A text extraction of the table; the row is quoted, the table layout is not visually validated.")]
        attributes = {"peripheral": instance, "direction": direction, "selector": selector}
        for controller, symbols in spec["controllers"].items():
            names = {key: template.format(NAME=instance, DIRECTION=direction.upper()) for key, (_, template) in symbols.items()}
            low = self.source(symbols["ll"][0])
            start, _, _ = inventory.declaration(low, names["ll"])
            value = re.fullmatch(rf"#define\s+{names['ll']}\s+(\d+)U\b.*", low.lines[start - 1].strip())
            if not value or value.group(1) != selector:
                raise SystemExit(f"{names['ll']} does not carry selector {selector}")
            high = self.source(symbols["hal"][0])
            start, _, _ = inventory.declaration(high, names["hal"])
            if not re.match(rf"\s*{names['hal']}\s*=\s*{names['ll']}\s*,", high.lines[start - 1]):
                raise SystemExit(f"{names['hal']} is not defined as {names['ll']}")
            evidence += [self.graph.cite_symbol("inv-request", low, names["ll"]),
                         self.graph.cite_symbol("inv-request", high, names["hal"])]
            attributes[f"hal_request_{controller.lower()}"] = names["hal"]
        attributes.update(spec["attributes"])
        self.graph.resource(identifier, spec["kind"], attributes, self.conditions, [*evidence, rule, *spec["evidence"]])
        owner = f"{self.prefix}.{instance}"
        self.graph.edge(f"{instance}.{direction}_request.{instance}.{direction}.request", owner, identifier,
                        f"{direction}_request", None, self.conditions, [evidence[0], rule])
        channels = sorted(record["id"] for record in self.value["resources"] if record["kind"] == spec["channel_kind"])
        for channel in channels:
            self.graph.edge(f"{instance}.{direction}.request.routes_to.{self.name(channel)}", identifier, channel,
                            "routes_to", None, self.conditions, [evidence[0], *spec["channel_evidence"]])
        return identifier

    def instances(self):
        """Promote described instances to a kind a profile can allocate."""
        description = self.config["device_description"]
        source = self.source(description["source"])
        promoted = []
        for spec in self.config["instances"]:
            instance, rule = spec["instance"], self.policy(spec["policy"])
            block = self.described(instance)
            if block["derived"] != spec.get("layout_of"):
                raise SystemExit(f"{instance}: device description derives its layout from {block['derived']}, "
                                 f"configuration says {spec.get('layout_of')}")
            layout = f" with the register layout of {block['derived']}" if block["derived"] else " with its own register layout"
            evidence = [self.graph.cite(
                f"described-{instance}", source, block["start"], block["name_line"],
                f"Device description declares peripheral {instance}{layout}. A layout statement, not proof of identical behaviour, clocks or errata."),
                rule, *spec.get("evidence", [])]
            attributes = dict(spec.get("attributes", {}))
            for attribute, (source_id, symbol) in spec["symbols"].items():
                attributes[attribute] = symbol
                evidence.append(self.graph.cite_symbol("inv-instance", self.source(source_id), symbol))
            record = self.resource(instance)
            if record["kind"] == description["unpromoted_kind"]:
                record["kind"] = spec["kind"]
                promoted.append(instance)
            elif record["kind"] != spec["kind"]:
                raise SystemExit(f"{instance} is already a {record['kind']}")
            inventory.complete(record, attributes, evidence)
            irq = self.resource(f"{spec['interrupt']}.irq")
            declared = [entry for entry in block["interrupts"] if entry["value"] == irq["attributes"]["irq_number"]]
            if len(declared) != 1:
                raise SystemExit(f"{instance}: device description does not declare interrupt {irq['attributes']['irq_number']} exactly once")
            identifier = f"{instance}.raises_interrupt.{spec['interrupt']}.irq"
            if identifier not in self.graph.edge_ids:
                cite = self.graph.cite(
                    f"described-irq-{instance}", source, declared[0]["start"], declared[0]["end"],
                    f"Device description declares interrupt {declared[0]['value']} inside peripheral {instance}.")
                self.graph.edge(identifier, record["id"], irq["id"], "raises_interrupt", None, self.conditions,
                                [cite, f"inv-irq-{spec['interrupt']}", rule])
            for direction in spec["requests"]:
                self.request(instance, direction)
        return promoted

    def profile(self, spec):
        """Let a capability take a covered instance and each of its signals on
        any allowed pad the signal is routed to. The reviewed instance and
        pads stay the first choice; the alternate function is bound from the
        selected route."""
        rule, general = self.policy(spec["policy"]), self.policy("general")
        instance = spec["instance"]
        instance_rule = self.policy(instance["policy"])
        extra = spec.get("extra_pads", {})
        allowed = [*self.eligible, *extra]
        capabilities = [find(self.value["capabilities"], identifier) for identifier in spec["capabilities"]]
        kinds = {find(capability["roles"], instance["role"])["kind"] for capability in capabilities}
        if len(kinds) != 1:
            raise SystemExit("profile capabilities disagree on the instance kind")
        covered = {record["id"] for record in self.value["resources"] if record["kind"] in kinds}
        routes = {}
        for role, binding in spec["roles"].items():
            found = [edge for edge in self.value["edges"]
                     if edge["relation"] == "signal" and edge.get("attributes", {}).get("function") == binding["function"]]
            stray = sorted({self.name(edge["from"]) for edge in found if edge["from"] not in covered})
            if stray:
                raise SystemExit(f"function {binding['function']} is routed from uncovered instances {stray}")
            found = [edge for edge in found if self.name(edge["to"]) in allowed]
            if not found:
                raise SystemExit(f"no allowed route carries {binding['function']}")
            routes[role] = found
        marker = spec.get("pad_attribute")
        if marker:
            # A profile with pads outside general allocation marks every pad it may take.
            for pin in sorted({self.name(edge["to"]) for found in routes.values() for edge in found}):
                record = self.resource(pin)
                record["attributes"].update(marker)
                extend(record, [rule, *([self.policy(extra[pin])] if pin in extra else [])])
        else:
            marker = self.config["general_attribute"]
        symbol_spec = spec["alternate_symbol"]
        symbol_source = self.source(symbol_spec["source"])
        mapping, cites = {}, []
        for alternate in sorted({edge["attributes"]["alternate"] for found in routes.values() for edge in found}):
            text = symbol_spec["symbol"].format(alternate=alternate.replace(symbol_spec["split"], symbol_spec["split"] + "_", 1))
            mapping[alternate] = text
            cites.append(self.graph.cite_symbol("inv-alternate", symbol_source, text))
        for capability in capabilities:
            for role in [instance["role"], *spec["roles"]]:
                record = find(capability["roles"], role)
                # What this role was reviewed with stays the first choice, so
                # widening the domain does not move an existing design.
                reviewed = [resource["id"] for resource in self.value["resources"]
                            if resource["kind"] == record["kind"] and record["attributes"]
                            and all(resource["attributes"].get(key) == value
                                    for key, value in record["attributes"].items())]
                if len(reviewed) != 1:
                    raise SystemExit(f"{capability['id']}.{role}: no single reviewed resource to keep as first choice")
                record["prefer"] = reviewed
                record["attributes"] = {} if role == instance["role"] else dict(marker)
            for role, binding in spec["roles"].items():
                links = [link for link in capability["links"] if link["from"] == binding["from"] and link["to"] == role]
                if len(links) != 1:
                    raise SystemExit(f"{capability['id']}: expected one link from {binding['from']} to {role}")
                links[0]["relation"] = "signal"
                links[0]["attributes"] = {"function": binding["function"]}
            patch = spec.get("capability", {})
            capability["description"] = restate(capability["id"], capability["description"], patch.get("description", []))
            for card in patch.get("implementations_remove", []):
                capability["implementations"].remove(card)
            extend(capability, [rule, general, instance_rule])
        for patch in spec["cards"]:
            card = find(self.value["implementations"], patch["card"])
            component = card["component"]
            for name in patch.get("bindings_remove", []):
                del component["bindings"][name]
            for name, source in patch.get("bindings", {}).items():
                if name in component["bindings"]:
                    raise SystemExit(f"{card['id']}: binding {name} already exists")
                component["bindings"][name] = {"source": source}
            for role, binding in spec["roles"].items():
                component["bindings"][binding["binding"]] = {
                    "source": {"type": "route_attribute", "from": binding["from"], "to": role,
                               "relation": "signal", "attribute": "alternate"},
                    "map": dict(mapping),
                }
            replaced = set()
            for file in component["files"]:
                if file["template"] in patch.get("templates", {}):
                    replaced.add(file["template"])
                    file["template"] = patch["templates"][file["template"]]
            if replaced != set(patch.get("templates", {})):
                raise SystemExit(f"{card['id']}: a template to replace is not used")
            card["guidance"] = restate(card["id"], card["guidance"], patch.get("guidance", []))
            for field, replacements in patch.get("interface", {}).items():
                component["interface"][field] = restate(card["id"], component["interface"][field], replacements)
            component.setdefault("interface_sources", {}).update(patch.get("interface_sources", {}))
            for identifier in patch.get("evidence_remove", []):
                card["evidence"].remove(identifier)
            symbols = [self.graph.cite_symbol("inv-api", self.source(source), symbol)
                       for source, symbol in patch.get("symbols", [])]
            extend(card, [rule, instance_rule, *cites, *symbols, *patch.get("evidence", [])])
        return {role: sorted({f"{self.name(edge['from'])}:{self.name(edge['to'])}" for edge in found})
                for role, found in routes.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--inventory", type=pathlib.Path, required=True)
    parser.add_argument("--domains", type=pathlib.Path, required=True)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if arguments.out.exists():
        raise SystemExit("refusing to overwrite an existing graph")
    graph, sources, summary = inventory.build(arguments.inventory, arguments.source_root)
    inventory_config = json.loads(arguments.inventory.read_text(encoding="utf-8"))
    config = json.loads(arguments.domains.read_text(encoding="utf-8"))
    domains = Domains(graph, sources, inventory_config, config, arguments.source_root)
    domains.board()
    summary["general_allocation_pads"] = len(domains.eligible)
    summary["gpio_output_pads"] = domains.gpio_output()
    summary["gpio_interrupt_pads"] = domains.gpio_interrupt()
    domains.templates()
    summary["function_routes"] = domains.route_functions()
    summary["promoted_instances"] = domains.instances()
    summary["profiles"] = [domains.profile(spec) for spec in config["profiles"]]
    summary["reservations"] = [domains.name(record["resource"]) for record in domains.target["reservations"]]
    inventory.write(graph, arguments.out, summary)


if __name__ == "__main__":
    sys.exit(main())
