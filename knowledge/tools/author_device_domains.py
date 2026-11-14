# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Open reviewed capabilities to the whole device, on top of the inventory.

The inventory stage records which pads, routes and interrupts are declared.
This stage decides which of them a capability may use, from inputs that are
all cited line by line:

* the board, from its own file: a netlist, which says what else is connected
  to each device pad, or a description declared by the board's owner;
* the device description, which says which instances share a register layout
  and which interrupt each one raises;
* the DMA request table and the SDK definitions that must agree with it;
* an authored policy text, which says how those are turned into rules.

It then widens existing capabilities to that domain. A capability that needed a
fixed pad or instance now accepts any covered one, keeps the reviewed one as
its first choice, and reads the alternate function from the selected route.

The family configuration states no board. `compose_target.py` applies the
board file a recipe names and then runs this stage, so there is no command
line here.
"""
import copy
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_inventory as inventory  # noqa: E402
import descriptor  # noqa: E402
import json  # noqa: E402


def find(records, identifier):
    matches = [record for record in records if record["id"] == identifier]
    if len(matches) != 1:
        raise SystemExit(f"expected one record {identifier!r}, found {len(matches)}")
    return matches[0]


def extend(record, evidence):
    for identifier in evidence:
        if identifier not in record["evidence"]:
            record["evidence"].append(identifier)
    if len(record["evidence"]) > inventory.LIMITS["citations"]:
        raise SystemExit(f"{record['id']} cites more than {inventory.LIMITS['citations']} sources")


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
        self.pad_template = inventory_config["pad_name"]
        self.conditions = inventory_config["conditions"]
        self.target = find(self.value["targets"], inventory_config["target"])
        for record in config["sources"]:
            data = (source_root / record["path"]).read_bytes()
            if record.get("sha256", inventory.sha256(data)) != inventory.sha256(data):
                raise SystemExit(f"source {record['id']} differs from the hash the configuration pins")
            pinned = {**record, "sha256": inventory.sha256(data)}
            pinned = {key: pinned[key] for key in ("id", "path", "sha256", "revision", "authority")}
            if any(existing["id"] == record["id"] for existing in self.value["sources"]):
                raise SystemExit(f"source {record['id']} is already pinned")
            self.value["sources"].append(pinned)
            sources[record["id"]] = inventory.Source(pinned, source_root)
        if len(self.value["sources"]) > inventory.LIMITS["sources"]:
            raise SystemExit(f"more than {inventory.LIMITS['sources']} sources; a later graph schema allows more")
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
        self.unbonded = set()
        self.composed = bool(config.get("composed"))
        self.narrowed = []

    def pad_name(self, pad):
        return self.pad_template.format(port=pad["port"], index=pad["index"])

    def source(self, identifier):
        """A source the graph already pins, opened on first use."""
        if identifier not in self.sources:
            self.sources[identifier] = inventory.Source(find(self.value["sources"], identifier), self.source_root)
        return self.sources[identifier]

    def policy_entry(self, name):
        """A rule about the board comes from the board's own policy text when
        the board brings one; every other rule from the chip family's."""
        board = self.config.get("board_policy")
        if board and name in board["lines"]:
            return board, self.source(board["source"])
        if name not in self.config["policy"]["lines"]:
            raise SystemExit(f"no policy text states the rule {name}")
        return self.config["policy"], self.policy_source

    def policy_text(self, name):
        policy, source = self.policy_entry(name)
        return source.lines[policy["lines"][name] - 1]

    def policy(self, name):
        policy, source = self.policy_entry(name)
        line = policy["lines"][name]
        text = source.lines[line - 1]
        return self.graph.cite(f"{policy['evidence_prefix']}-{name}", source, line, line,
                               f"{policy['label']}: " + text.split(":", 1)[0] + ". Authored engineering policy, not a manufacturer statement.")

    def resource(self, name):
        return find(self.value["resources"], f"{self.prefix}.{name}")

    def name(self, identifier):
        return identifier.split(".", 1)[1]

    def board(self):
        """Decide which pads the board leaves free, from its netlist or from a
        description its owner declares, then restate what that changes."""
        spec = self.config["board"]
        if spec["kind"] == "declared":
            self.declared_board(spec)
        else:
            self.netlist_board(spec)
        for old, new in self.config.get("assumption_replacements", []):
            # A reviewed assumption that names the old, narrower domain is restated.
            if self.target["assumptions"].count(old) != 1:
                raise SystemExit("assumption to restate not found exactly once")
            self.target["assumptions"][self.target["assumptions"].index(old)] = new
        self.target["assumptions"].append(self.config["assumption"])
        if len(self.target["assumptions"]) > inventory.LIMITS["assumptions"]:
            raise SystemExit("too many target assumptions")

    def reserve(self, pin, record, evidence):
        """A pad the base occupies and no capability may adopt."""
        extend(record, evidence)
        extend(self.target, evidence)
        held = [entry for entry in self.target["reservations"] if entry["resource"] == record["id"]]
        if any(entry["adoptable_by"] for entry in held):
            # The policy forbids adoption; a base that allows it is not ours to keep.
            raise SystemExit(f"{pin} is reserved by policy but the base lets a capability adopt it")
        if not held:
            self.target["reservations"].append({"resource": record["id"], "count": 1, "adoptable_by": []})

    def withheld(self, pin, record):
        """A pad the chip family keeps out of general allocation on every
        board, with the manufacturer statement that is the reason."""
        spec = self.config.get("withheld_pads")
        if not spec or pin not in spec["pads"]:
            return False
        cite = spec["cite"]
        extend(record, [self.policy(spec["policy"]),
                        self.graph.cite(cite["id"], self.source(cite["source"]), cite["start"], cite["end"], cite["statement"])])
        return True

    def foundation_pads(self):
        """Pads the project foundation uses must be reserved by whatever board
        is applied. A board may add restrictions; it cannot omit these."""
        spec = self.config.get("foundation_signals")
        if not spec:
            return
        rule = self.policy(spec["policy"])
        reserved = {entry["resource"] for entry in self.target["reservations"] if not entry["adoptable_by"]}
        for signal in spec["signals"]:
            pads = sorted(edge["to"] for edge in self.value["edges"]
                          if edge["relation"] == "signal" and edge["attributes"]["signal"] == signal)
            if not pads:
                raise SystemExit(f"the foundation needs {signal}, which this package does not route")
            missing = [self.name(pad) for pad in pads if pad not in reserved]
            if missing:
                raise SystemExit(f"the foundation needs {signal} on {', '.join(missing)}; the board does not reserve it")
        extend(self.target, [rule])

    def declared_board(self, spec):
        """A board described by its owner: the pads it occupies, the pads it
        keeps out, and its statement that every other pad is free. Nothing in
        it is checked against a schematic, and each citation says so."""
        description = self.source(spec["source"])
        stated = json.loads("\n".join(description.lines))
        names = {self.pad_name(pad) for pad in self.pads.values()}
        reserved, excluded = stated["reserved"], stated["excluded"]
        if set(reserved) & set(excluded):
            raise SystemExit("the board description both reserves and excludes a pad")
        unknown = sorted((set(reserved) | set(excluded)) - names)
        if unknown:
            raise SystemExit(f"the board description names pads this package does not bond: {unknown}")
        if stated.get("unlisted_pads") != "free" or set(stated) - {
                "board", "description", "part", "package", "clock", "unlisted_pads", "reserved", "excluded"}:
            # Only one statement about unnamed pads is understood. Anything
            # else is refused, never read as permission.
            raise SystemExit('a board description must say "unlisted_pads": "free" and nothing this tool does not understand')
        # A description is written for one board carrying one exact part.
        for key in ("board", "part", "package"):
            if stated.get(key) != self.target[key]:
                raise SystemExit(f"the board description is for {key} {stated.get(key)}, not {self.target[key]}")
        rule_line = description.find(r'^\s*"unlisted_pads"\s*:')
        self.eligible, self.net_cite = [], {}
        for pin in sorted(names):
            record = self.resource(pin)
            if pin in reserved or pin in excluded:
                line = description.find(rf'^\s*"{pin}"\s*:')
                cite = self.graph.cite(
                    f"board-{pin}", description, line, line,
                    f"The board description, written by the board's owner, states the use of pad {pin}. Not checked against a schematic or a populated board.")
                self.net_cite[pin] = cite
                rule = self.policy(reserved.get(pin) or excluded[pin])
                if pin in reserved:
                    self.reserve(pin, record, [cite, rule])
                else:
                    extend(record, [cite, rule])
                continue
            cite = self.graph.cite(
                "board-eligible", description, rule_line, rule_line,
                "The board description states that every pad it does not name is free. The owner's statement, not a verified fact.")
            self.net_cite[pin] = cite
            if self.withheld(pin, record):
                extend(record, [cite])
                continue
            record["attributes"].update(self.config["general_attribute"])
            extend(record, [cite, self.policy("general")])
            self.eligible.append(pin)

    def netlist_board(self, spec):
        """Classify every configurable pad by what its board net carries."""
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
                pin = self.pad_name(self.pads[pad])
                if pin in nets:
                    raise SystemExit(f"pad {pin} is on two board nets")
                others = sorted({m["reference"] for m in value["net"] if m["reference"] != spec["device_reference"]})
                nets[pin] = (start, end, value["netLabel"], others)
        if set(nets) != {self.pad_name(pad) for pad in self.pads.values()}:
            raise SystemExit("a configurable pad has no board net")
        unknown = sorted((set(spec["reserved_nets"]) | set(spec["excluded_nets"])) - {label for _, _, label, _ in nets.values()})
        if unknown:
            # A misspelt net would silently leave its pad open for allocation.
            raise SystemExit(f"configured nets {unknown} join no configurable pad")
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
                self.reserve(pin, record, [cite, self.policy(spec["reserved_nets"][label])])
                continue
            if label in spec["excluded_nets"]:
                extend(record, [cite, self.policy(spec["excluded_nets"][label])])
                continue
            if self.withheld(pin, record):
                extend(record, [cite])
            elif all(passive.fullmatch(reference) for reference in others):
                record["attributes"].update(self.config["general_attribute"])
                extend(record, [cite, self.policy("general")])
                self.eligible.append(pin)
            else:
                extend(record, [cite, self.policy("general")])

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
        if missing and not self.composed:
            raise SystemExit(f"{capability['id']}: reviewed choices left the domain: {missing}")
        if missing:
            # Another package or board may withhold a reviewed choice; the
            # rest of the domain still stands, and the loss is reported.
            values[:] = [pin for pin in values if pin in pins]
            self.narrowed.append(f"{capability['id']}.{parameter} lost {', '.join(missing)}")
        values.extend(pin for pin in pins if pin not in values)
        if not values:
            raise SystemExit(f"{capability['id']}: no choice is left for {parameter}")
        if len(values) > inventory.LIMITS["choices"]:
            raise SystemExit(f"choice domain exceeds {inventory.LIMITS['choices']} values")

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
        pads = {self.pad_name(pad): pad for pad in self.pads.values()}

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

    def route_marks(self):
        """Give listed signal routes an authored attribute, such as the role a
        signal plays in a profile or the SDK symbol of its channel.

        The policy line is the authority: for every value its sentence must
        name exactly the signals configured here. A value that is an SDK
        symbol is also cited where the SDK declares it."""
        marked = {}
        for spec in self.config["route_marks"]:
            attribute = spec["attribute"]
            rule, text = self.policy(spec["policy"]), self.policy_text(spec["policy"])
            values, cites = {}, {}
            for value, signals in spec["values"].items():
                sentences = [sentence.rstrip(".") for sentence in text.split(". ")
                             if sentence.rstrip(".").endswith((f" are {value}", f" is {value}"))]
                if len(sentences) != 1:
                    raise SystemExit(f"policy does not define {attribute} {value} exactly once")
                subject = sentences[0].rsplit(" ", 2)[0]
                stated = set(re.findall(r"\b[A-Z][A-Z0-9]*_[A-Z0-9_]+\b", subject))
                if stated != set(signals):
                    raise SystemExit(f"{attribute} {value}: policy names {sorted(stated)}, configuration {sorted(signals)}")
                for signal in signals:
                    if values.setdefault(signal, value) != value:
                        raise SystemExit(f"signal {signal} has two values of {attribute}")
                if "symbol_source" in spec:
                    cites[value] = self.graph.cite_symbol("inv-mark", self.source(spec["symbol_source"]), value)
            for sentence in text.split(". "):
                defined = re.search(r"^(.*) (?:are|is) ([A-Za-z_][A-Za-z0-9_]*)$", sentence.rstrip("."))
                if defined and defined.group(2) not in spec["values"] \
                        and re.search(r"\b[A-Z][A-Z0-9]*_[A-Z0-9_]+\b", defined.group(1)):
                    raise SystemExit(f"policy defines {attribute} {defined.group(2)} but the configuration does not")
            count, seen = {}, set()
            for edge in self.value["edges"]:
                signal = edge.get("attributes", {}).get("signal") if edge["relation"] == "signal" else None
                value = values.get(signal)
                if value is None:
                    continue
                if edge["attributes"].setdefault(attribute, value) != value:
                    raise SystemExit(f"{edge['id']} already has another {attribute}")
                extend(edge, [rule, *([cites[value]] if value in cites else [])])
                count[value] = count.get(value, 0) + 1
                seen.add(signal)
            missing = sorted(set(values) - seen)
            if missing and not self.composed:
                raise SystemExit(f"configured signals {missing} have no route to mark with {attribute}")
            # A signal belongs to the die; a smaller package of the same die
            # bonds fewer of them. A composed target reports those instead.
            self.unbonded.update(missing)
            for value, routes in count.items():
                marked[f"{attribute}={value}"] = routes
        return marked

    def described(self, instance):
        if self.blocks is None:
            self.blocks = description_blocks(self.source(self.config["device_description"]["source"]))
        if instance not in self.blocks:
            raise SystemExit(f"device description has no peripheral {instance}")
        return self.blocks[instance]

    def request_facts(self, instance, direction, request):
        """What the sources say about one DMA request line: its table row and
        selector, and per controller the SDK names that carry that selector.
        Refuses unless the table and every controller's definitions agree."""
        spec = self.config["dma_requests"]
        table = self.source(spec["table"]["source"])
        row = re.escape(spec["table"]["row"].format(selector="@", request=request))
        pattern = re.compile("^" + row.replace("@", r"(\d+)") + r"\s*$")
        rows = [(number, match.group(1)) for number, line in enumerate(table.lines, 1)
                if (match := pattern.match(line))]
        if len(rows) != 1:
            raise SystemExit(f"{table.id}: expected one request row for {instance} {direction}, found {len(rows)}")
        line, selector = rows[0]
        names = {}
        for controller, symbols in spec["controllers"].items():
            found = {key: template.format(NAME=instance, DIRECTION=direction.upper()) for key, (_, template) in symbols.items()}
            low = self.source(symbols["ll"][0])
            _, declared, _ = inventory.declaration(low, found["ll"])
            value = re.fullmatch(rf"#define\s+{found['ll']}\s+(\d+)U\b.*", low.lines[declared - 1].strip())
            if not value or value.group(1) != selector:
                raise SystemExit(f"{found['ll']} does not carry selector {selector}")
            high = self.source(symbols["hal"][0])
            _, declared, _ = inventory.declaration(high, found["hal"])
            if not re.match(rf"\s*{found['hal']}\s*=\s*{found['ll']}\s*,", high.lines[declared - 1]):
                raise SystemExit(f"{found['hal']} is not defined as {found['ll']}")
            names[controller] = (low, found["ll"], high, found["hal"])
        return table, line, selector, names

    def request(self, instance, direction, request, cited=()):
        """One DMA request line, recorded only when the manual's table and the
        SDK definitions for every controller agree on its selector. A line the
        base already records is held to the same facts instead of being
        trusted: a base that changed underneath is refused, not preserved."""
        spec = self.config["dma_requests"]
        identifier = f"{self.prefix}.{instance}.{direction}.request"
        table, line, selector, names = self.request_facts(instance, direction, request)
        attributes = {"peripheral": instance, "direction": direction, "selector": selector}
        for controller, (_, _, _, high_name) in names.items():
            attributes[f"hal_request_{controller.lower()}"] = high_name
        clash = sorted(set(spec["attributes"]) & set(attributes))
        if clash:
            raise SystemExit(f"configured request attributes {clash} would replace values read from the sources")
        attributes.update(spec["attributes"])
        owner = f"{self.prefix}.{instance}"
        owner_edge = f"{instance}.{direction}_request.{instance}.{direction}.request"
        channels = sorted((record for record in self.value["resources"] if record["kind"] == spec["channel_kind"]),
                          key=lambda record: record["id"])
        unverified = sorted({record["attributes"]["controller"] for record in channels} - set(names))
        if unverified:
            # A route may only reach a channel whose controller's definitions were checked.
            raise SystemExit(f"request definitions are not configured for controllers {unverified}")
        routes = {f"{instance}.{direction}.request.routes_to.{self.name(record['id'])}": record["id"] for record in channels}
        if identifier in self.graph.resource_ids:
            existing = find(self.value["resources"], identifier)
            if existing["attributes"] != attributes or existing["kind"] != spec["kind"] or existing["capacity"] != 1:
                raise SystemExit(f"{identifier}: the recorded request disagrees with the table and SDK definitions")
            edges = {edge["id"]: edge for edge in self.value["edges"]}
            expected = {owner_edge: (owner, identifier, f"{direction}_request"),
                        **{edge: (identifier, channel, "routes_to") for edge, channel in routes.items()}}
            for edge, ends in expected.items():
                if edge not in edges or (edges[edge]["from"], edges[edge]["to"], edges[edge]["relation"]) != ends:
                    raise SystemExit(f"{identifier}: recorded route {edge} is missing or different")
            return identifier
        rule = self.policy(spec["policy"])
        evidence = [self.graph.cite(
            f"request-row-{instance}-{direction}", table, line, line,
            f"Reference manual request table row: selector {selector} is the {instance} {direction} request. A text extraction of the table; the row is quoted, the table layout is not visually validated.")]
        for low, low_name, high, high_name in names.values():
            evidence += [self.graph.cite_symbol("inv-request", low, low_name),
                         self.graph.cite_symbol("inv-request", high, high_name)]
        self.graph.resource(identifier, spec["kind"], attributes, self.conditions,
                            [*cited, *evidence, rule, *spec["evidence"]])
        self.graph.edge(owner_edge, owner, identifier, f"{direction}_request", None, self.conditions, [evidence[0], rule])
        selection = spec["channel_selection"]
        select = self.graph.cite(selection["id"], self.source(selection["source"]), selection["start"], selection["end"],
                                 selection["statement"])
        for edge, channel in routes.items():
            self.graph.edge(edge, identifier, channel, "routes_to", None, self.conditions,
                            [evidence[0], select, *spec["channel_evidence"]])
        return identifier

    def interrupt(self, instance, name, block, source, record, rule, cited=()):
        """Join an instance to the interrupt its description declares."""
        irq = self.resource(f"{name}.irq")
        declared = [entry for entry in block["interrupts"] if entry["value"] == irq["attributes"]["irq_number"]]
        if len(declared) != 1:
            raise SystemExit(f"{instance}: device description does not declare interrupt {irq['attributes']['irq_number']} exactly once")
        identifier = f"{instance}.raises_interrupt.{name}.irq"
        if identifier not in self.graph.edge_ids:
            cite = self.graph.cite(
                f"described-irq-{instance}", source, declared[0]["start"], declared[0]["end"],
                f"Device description declares interrupt {declared[0]['value']} inside peripheral {instance}.")
            self.graph.edge(identifier, record["id"], irq["id"], "raises_interrupt", None, self.conditions,
                            [*cited, cite, f"inv-irq-{name}", rule])

    def symbol(self, prefix, source_id, symbol, owner, agree=None):
        """Cite an SDK symbol where it is declared. A symbol that exists but
        belongs to something else would pass an existence check, so the cited
        lines must also name what the configuration says it refers to."""
        cited = self.source(source_id)
        cite = self.graph.cite_symbol(prefix, cited, symbol)
        if agree:
            start, end, _ = inventory.declaration(cited, symbol)
            if not re.search(r"(?<![A-Za-z0-9])" + re.escape(agree) + r"(?![A-Za-z0-9_])", cited.excerpt(start, end)):
                raise SystemExit(f"{owner}: {symbol} does not refer to {agree}")
        return cite

    def mark(self, signal, attributes, evidence):
        """Write authored attributes on every route of one signal. A value
        another rule already wrote is never replaced."""
        routes = [edge for edge in self.value["edges"]
                  if edge["relation"] == "signal" and edge.get("attributes", {}).get("signal") == signal]
        for edge in routes:
            for key, value in attributes.items():
                if edge["attributes"].setdefault(key, value) != value:
                    raise SystemExit(f"{edge['id']} already has another {key}")
            extend(edge, evidence)
        if not routes:
            # A signal belongs to the die; this package bonds none of its pads.
            self.unbonded.add(signal)
        return len(routes)

    def removed(self, spec):
        """Peripherals the manufacturer removes for exactly this part. A part
        the pack description does not list is refused, so a misspelt part
        cannot pass as a part without removals."""
        part = self.target["part"]
        located = descriptor.parse(self.source(spec["limitations"]))["limitations"]
        entry = located.get(part)
        if entry is None:
            # The descriptor lists unrestricted parts with empty removals, so
            # an absent part is unknown, not unrestricted.
            raise SystemExit(f"the limitations descriptor does not list {part}")
        cite = self.graph.cite(f"limited-{part}", self.source(spec["limitations"]), entry.start, entry.end,
                               f"Limitations descriptor lists what is removed for {part}.")
        return {name: cite for name in entry.value["removed_peripherals"]}

    def pack(self, spec):
        """The pack description is what ties a part to its package and to the
        descriptors of its die. Every input in use must be the one the pack
        names for this part; a part merely present in the pack is not enough."""
        pack, part = self.source(spec["pack_description"]), self.target["part"]
        variant = pack.find(re.escape(f'<variant Dvariant="{part}">'))
        close = next(n for n in range(variant, len(pack.lines) + 1) if pack.lines[n - 1].strip() == "</variant>")
        # What a variant does not state itself, its device entry states for
        # all its variants; the other variants' own lines are not this part's.
        first = max(n for n in range(1, variant) if "<device " in pack.lines[n - 1])
        last = next(n for n in range(variant, len(pack.lines) + 1) if pack.lines[n - 1].strip() == "</device>")
        shared, inside = [], False
        for line in pack.lines[first - 1:last]:
            inside = inside or "<variant " in line
            if not inside:
                shared.append(line)
            inside = inside and "</variant>" not in line
        block = "\n".join([*pack.lines[variant - 1:close], *shared])
        package = re.search(r'<feature type="[A-Za-z]+" name="([A-Za-z]+) (\d+)', block)
        pinout = descriptor.parse(self.source(spec["pinout"]))["characteristics"].value
        if not package or package.group(1) + package.group(2) != self.target["package"] \
                or pinout["package_name"] != self.target["package"]:
            raise SystemExit(f"the pack and the pinout descriptor do not both give {part} the package {self.target['package']}")
        named = re.search(r'schemaType="pinout" path="[^"]*/([^"/]+)"', block)
        if "pinout_file" in spec and (not named or named.group(1) != spec["pinout_file"]):
            raise SystemExit(f"the pack does not give {part} the pinout descriptor {spec['pinout_file']}")
        opened = max(n for n in range(1, variant) if "<subFamily " in pack.lines[n - 1])
        closed = next(n for n in range(variant, len(pack.lines) + 1) if "</subFamily>" in pack.lines[n - 1])
        family = "\n".join(pack.lines[opened - 1:closed])
        for kind, key in (("peripherals", "source"), ("limitations", "limitations")):
            name = pathlib.PurePosixPath(find(self.value["sources"], spec[key])["path"]).name
            if not re.search(rf'schemaType="{kind}" path="[^"]*/{re.escape(name)}"', family):
                raise SystemExit(f"the pack does not give {part} the {kind} descriptor {name}")
        clock = self.config.get("clock_descriptor")
        if clock:
            name = pathlib.PurePosixPath(find(self.value["sources"], clock["source"])["path"]).name
            if not re.search(rf'schemaType="clock" path="[^"]*/{re.escape(name)}"', block):
                raise SystemExit(f"the pack does not give {part} the clock descriptor {name}")
        if spec["die_in_path"].format(die=pinout["die_name"]) not in find(self.value["sources"], spec["source"])["path"]:
            raise SystemExit(f"this package is die {pinout['die_name']}; the peripherals descriptor is for another die")
        self.graph.cite(f"pack-variant-{part}", pack, variant, close,
                        f"Pack description declares variant {part} with its package and pinout descriptor.")
        extend(self.target, [f"pack-variant-{part}"])

    def clocked(self, name):
        """What the manufacturer's clock descriptor says feeds an instance:
        every clock element that names it, cited with its inputs and limit.
        The descriptor states what can be configured, never what is."""
        spec = self.config.get("clock_descriptor")
        if not spec:
            return []
        if not hasattr(self, "clock_elements"):
            source = self.source(spec["source"])
            self.clock_elements = (source, list(descriptor.parse(source)["tree"]["elements"]))
        source, elements = self.clock_elements
        cites = []
        for element in elements:
            value = element.value
            if name not in value.get("clocked_instance", []):
                continue
            if element.end - element.start >= inventory.LIMITS["excerpt_lines"]:
                raise SystemExit(f"clock element {value['id']} is longer than one citation may be")
            inputs = [entry["label"] for entry in value.get("possible_Input", [])]
            detail = f" with the inputs {', '.join(inputs)}" if inputs else ""
            if "max" in value:
                detail += f" and a highest value of {value['max']}"
            cites.append(self.graph.cite(
                f"clock-{value['id']}", source, element.start, element.end,
                f"Clock descriptor element {value['id']} of type {value['type']} names {', '.join(value['clocked_instance'])} as clocked by it{detail}. What can be configured, not the captured configuration."))
        return [*cites, self.policy(spec["policy"])] if cites else []

    def vectors(self):
        """An interrupt cites the enumerator and the vector the inventory
        transcribed for it. A reviewed record carried its symbols on other
        evidence; the lines that show them are added, never replaced."""
        added = 0
        for record in self.value["resources"]:
            if record["kind"] != "interrupt" or not record["id"].endswith(".irq"):
                continue
            name = self.name(record["id"])[:-len(".irq")]
            held = [identifier for identifier in (f"inv-irq-{name}", f"inv-vector-{name}")
                    if identifier in self.graph.evidence_ids and identifier not in record["evidence"]]
            if held:
                extend(record, held)
                added += 1
        return added

    def service_evidence(self):
        """Evidence a target service should cite and does not: a record the
        graph already holds, or a rule of the policy. Nothing is restated."""
        for spec in self.config.get("service_evidence", []):
            service = find(self.target["services"], spec["service"])
            missing = sorted(set(spec.get("evidence", [])) - self.graph.evidence_ids)
            if missing:
                raise SystemExit(f"{service['id']}: no evidence {missing}")
            added = [*spec.get("evidence", []), *(self.policy(name) for name in spec.get("policy", []))]
            if not added or all(identifier in service["evidence"] for identifier in added):
                raise SystemExit(f"{service['id']}: nothing to add")
            extend(service, added)

    def supersede(self):
        """Older policy texts limit themselves to one part, package, board and
        a few pads. Records that still cite them for what they say about
        ownership also cite the rule that replaces those limits."""
        spec = self.config.get("supersedes")
        if not spec:
            return
        rule = self.policy(spec["policy"])
        for section in ("targets", "resources", "edges", "capabilities", "implementations"):
            for record in self.value[section]:
                for holder in [record, *record.get("services", [])]:
                    if set(spec["evidence"]) & set(holder["evidence"]):
                        extend(holder, [rule])

    def restatements(self):
        """Card wording a widening made untrue, restated by exact replacement."""
        for spec in self.config.get("restatements", []):
            card = find(self.value["implementations"], spec["implementation"])
            card["guidance"] = restate(card["id"], card["guidance"], spec["guidance"])
            extend(card, [self.policy(spec["policy"])])

    def service_restatements(self):
        """Target service wording a widening made untrue, restated by exact
        replacement. The service then cites the rule that widened the domain
        and the lines that show what is true now."""
        for spec in self.config.get("service_restatements", []):
            service = find(self.target["services"], spec["service"])
            for name, replacements in spec["properties"].items():
                if name not in service["properties"]:
                    raise SystemExit(f"{service['id']} has no property {name!r} to restate")
                text = restate(f"{service['id']}.{name}", service["properties"][name], replacements)
                if len(text.encode("utf-8")) > 256:
                    raise SystemExit(f"{service['id']}.{name}: restated to more than the 256 bytes a property may hold")
                service["properties"][name] = text
            shown = [self.graph.cite(cite["id"], self.source(cite["source"]), cite["start"], cite["end"], cite["statement"])
                     for cite in spec["cite"]]
            if not shown:
                raise SystemExit(f"{service['id']}: a restated service must cite what shows the new wording")
            extend(service, [self.policy(spec["policy"]), *shown])

    def classes(self):
        """Promote every instance of a covered peripheral class.

        A class is a peripheral type and IP version as the manufacturer's
        peripherals descriptor names them, not a list of instance names. The
        descriptor also gives each instance its bus, its interrupt, its DMA
        requests and the role of each signal; SDK symbols are formed from the
        class's templates and checked against their declarations. An instance
        the part removes, or whose signals this package does not bond, is
        reported and left alone."""
        spec = self.config["peripherals"]
        source = self.source(spec["source"])
        self.pack(spec)
        entries = {entry.value["name"]: entry for entry in descriptor.parse(source)["peripherals"]}
        removed = self.removed(spec)
        # What the limitations entry of this part says, cited by every instance
        # a class covers "unless the part's limitations remove it".
        self.limited = f"limited-{self.target['part']}"
        promoted, skipped = [], {}
        for cls in self.config["instance_classes"]:
            ip = cls["ip"]
            for name, entry in entries.items():
                value = entry.value
                if value.get("peripheralType") not in ip["types"] or value.get("digitalName") != ip["digitalName"]:
                    continue
                identifiers = {signal["id"] for signal in value.get("pinout_signals", [])}
                internal = bool(cls.get("internal"))
                if not internal and not identifiers & set(cls.get("needs_signals", identifiers or {None})):
                    continue
                if value.get("peripheralVersionNum") not in ip["versions"]:
                    skipped[name] = f"IP version {value.get('peripheralVersionNum')} is not covered"
                elif name in removed:
                    skipped[name] = "removed for this part"
                elif f"{self.prefix}.{name}" not in self.graph.resource_ids and not internal:
                    skipped[name] = "no signal of it is bonded in this package"
                else:
                    if f"{self.prefix}.{name}" not in self.graph.resource_ids:
                        # A class used without any pad exists on every package of
                        # the die, so the descriptor's declaration is its record.
                        self.graph.resource(
                            f"{self.prefix}.{name}", self.config["device_description"]["unpromoted_kind"], {"instance": name},
                            self.conditions, [self.graph.cite(
                                f"declared-{name}", source, entry["name"].start, entry["peripheralVersionNum"].end,
                                f"Peripherals descriptor declares {name} with type {value['peripheralType']} and IP version {value['peripheralVersionNum']}.")])
                    self.promote(name, cls, entry, source)
                    promoted.append(name)
        return promoted, skipped

    def promote(self, name, cls, entry, source):
        value, rule = entry.value, self.policy(cls["policy"])
        description = self.source(self.config["device_description"]["source"])
        number = re.search(r"(\d+)$", name)
        fields = {"instance": name, "lower": name.lower(), "number": number.group(1) if number else ""}
        evidence = [
            self.graph.cite(f"declared-{name}", source, entry["name"].start, entry["peripheralVersionNum"].end,
                            f"Peripherals descriptor declares {name} with type {value['peripheralType']} and IP version {value['peripheralVersionNum']}."),
            self.graph.cite(f"declared-ip-{name}", source, entry["digitalName"].start, entry["digitalName"].end,
                            f"Peripherals descriptor names the IP of {name} as {value['digitalName']}."),
        ]
        if entry["peripheralVersionNum"].end - entry["name"].start > 3:
            raise SystemExit(f"{name}: the descriptor does not state name, type and version together")
        if "bus_clock" in cls:
            buses = [bus for mapping in value["busMapping"] for bus in mapping["connections"]]
            if len(buses) != 1 or buses[0] not in cls["bus_clock"]:
                raise SystemExit(f"{name}: the descriptor places it on {buses}, which the class does not cover")
            fields["bus_clock"] = cls["bus_clock"][buses[0]]
            evidence.append(self.graph.cite(
                f"declared-bus-{name}", source, entry["busMapping"].start, entry["busMapping"].end,
                f"Peripherals descriptor places {name} on bus {buses[0]}."))
        block = self.described(name)
        layout = f" with the register layout of {block['derived']}" if block["derived"] else " with its own register layout"
        evidence += [self.graph.cite(
            f"described-{name}", description, block["start"], block["name_line"],
            f"Device description declares peripheral {name}{layout}. A layout statement, not proof of identical behaviour, clocks or errata."),
            rule, *cls.get("evidence", []), *self.clocked(name), *([self.limited] if hasattr(self, "limited") else [])]
        attributes = dict(cls.get("attributes", {}))
        for attribute, (source_id, symbol, *agree) in cls["symbols"].items():
            attributes[attribute] = symbol.format(**fields)
            evidence.append(self.symbol("inv-instance", source_id, attributes[attribute], name,
                                        agree[0].format(**fields) if agree else None))
        record = self.resource(name)
        if record["kind"] == self.config["device_description"]["unpromoted_kind"]:
            record["kind"] = cls["kind"]
        elif record["kind"] != cls["kind"]:
            raise SystemExit(f"{name} is already a {record['kind']}")
        inventory.complete(record, attributes, evidence)

        def link(kind, signal):
            found = [item for item in entry["interconnect"] if item.value["type"] == kind and item.value["ip_signal"] == signal]
            if len(found) != 1:
                raise SystemExit(f"{name}: expected one {kind} {signal} in the descriptor, found {len(found)}")
            cite = self.graph.cite(f"declared-{kind.replace(' ', '-')}-{name}-{signal}", source, found[0].start, found[0].end,
                                   f"Peripherals descriptor connects the {signal} {kind} of {name} to {found[0].value['source']}.")
            return found[0].value["source"], cite

        if "interrupt" in cls:
            # A class may name its interrupt by several descriptor signals, in
            # order: one instance has a vector of its own for an event that
            # another instance of the same IP raises on its only vector.
            wanted = cls["interrupt"] if isinstance(cls["interrupt"], list) else [cls["interrupt"]]
            declared = {item.value["ip_signal"] for item in entry["interconnect"] if item.value["type"] == "interrupt"}
            present = [signal for signal in wanted if signal in declared]
            if not present:
                raise SystemExit(f"{name}: the descriptor declares none of the interrupts {wanted}")
            interrupt, cite = link("interrupt", present[0])
            self.interrupt(name, interrupt, block, description, record,
                           self.policy(cls["interrupt_policy"]) if "interrupt_policy" in cls else rule, [cite])
        for direction, signal in cls.get("requests", {}).items():
            request, cite = link("dma request", signal)
            self.request(name, direction, request, [cite])
        if "roles" in cls:
            roles = cls["roles"]
            role_rule, text = self.policy(roles["policy"]), self.policy_text(roles["policy"])
            protocols = [item for item in entry["protocols"] if item.value["type"] == roles["protocol"]]
            if len(protocols) != 1:
                raise SystemExit(f"{name}: expected one {roles['protocol']} protocol in the descriptor, found {len(protocols)}")
            for kind, function in roles["functions"].items():
                if not re.search(r"(?<![A-Za-z0-9_])" + re.escape(f"{kind} is {function}") + r"(?![A-Za-z0-9_])", text):
                    raise SystemExit(f"the policy does not say that {kind} is {function}")
            for signal in protocols[0]["signals"]:
                function = roles["functions"].get(signal.value["type"])
                if function:
                    cite = self.graph.cite(
                        f"declared-role-{signal.value['signal']}", source, signal.start, signal.end,
                        f"Peripherals descriptor types signal {signal.value['signal']} as {signal.value['type']} in the {roles['protocol']} protocol of {name}.")
                    self.mark(signal.value["signal"], {"function": function}, [cite, role_rule])
        for spec in cls.get("signal_marks", []):
            mark_rule = self.policy(spec["policy"])
            for signal in entry["pinout_signals"]:
                match = re.fullmatch(spec["id"], signal.value["id"])
                if not match:
                    continue
                marks = {key: template.format(*match.groups(), instance=name) for key, template in spec["attributes"].items()}
                cites = [self.graph.cite(
                    f"declared-signal-{signal.value['name']}", source, signal.start, signal.end,
                    f"Peripherals descriptor declares signal {signal.value['name']} of {name} with identifier {signal.value['id']}."), mark_rule]
                for key, (source_id, agree) in spec.get("symbols", {}).items():
                    cites.append(self.symbol("inv-mark", source_id, marks[key], signal.value["name"],
                                             agree.format(*match.groups(), instance=name)))
                self.mark(signal.value["name"], marks, cites)

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
            if "pad_requires" in spec:
                found = [edge for edge in found if self.structured(self.name(edge["to"]), spec["pad_requires"], rule)]
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
                joined = [link["relation"] for link in capability["links"]
                          if role in spec["roles"] and link["from"] == spec["roles"][role]["from"] and link["to"] == role]
                if len(reviewed) > 1 and len(joined) == 1:
                    # Roles that select by the same pad attribute are told apart
                    # by the reviewed route that reaches each pad.
                    reached = {edge["to"] for edge in self.value["edges"]
                               if edge["relation"] == joined[0] and edge["from"] in covered}
                    reviewed = [name for name in reviewed if name in reached]
                record["attributes"] = {} if role == instance["role"] else dict(marker)
                kept = [name for name in reviewed
                        if all(find(self.value["resources"], name)["attributes"].get(key) == value
                               for key, value in record["attributes"].items())]
                if reviewed and len(kept) == len(reviewed):
                    record["prefer"] = kept
                elif self.composed and kept:
                    # Some reviewed choices are not available on this package or board.
                    record["prefer"] = kept
                    lost = ", ".join(self.name(name) for name in reviewed if name not in kept)
                    self.narrowed.append(f"{capability['id']}.{role} lost the reviewed choices {lost}")
                elif self.composed and len(reviewed) <= 1:
                    # The reviewed first choice is not available on this package or board.
                    self.narrowed.append(f"{capability['id']}.{role} has no reviewed first choice here")
                else:
                    raise SystemExit(f"{capability['id']}.{role}: no single reviewed resource in the new domain to keep as first choice")
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
            for pin in sorted(extra):
                # A pad outside general allocation that this profile may take:
                # the capability cites what the board says of it and the rule.
                if any(self.name(edge["to"]) == pin for found in routes.values() for edge in found):
                    extend(capability, [self.net_cite[pin], self.policy(extra[pin])])
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


    def structured(self, pin, required, rule):
        """Whether the pinout descriptor gives a pad the I/O structure option
        a profile requires. A pad that has it cites the lines that say so."""
        if not hasattr(self, "pad_lines"):
            pinout = self.source(self.config["peripherals"]["pinout"])
            self.pad_lines = {}
            for start, end, value, _ in inventory.children(pinout, "die_pads"):
                if value["type"] == "gpio":
                    self.pad_lines[self.pad_name(value)] = (pinout, start, end, value)
        pinout, start, end, value = self.pad_lines[pin]
        structure = value.get("io_structure", {})
        if required["option"] not in structure.get("options", []):
            return False
        meaning = [(start, end, text) for start, end, text, key in inventory.children(pinout, "io_structure_options_description")
                   if key == required["option"]]
        if len(meaning) != 1:
            raise SystemExit(f"the pinout descriptor does not describe the I/O structure option {required['option']}")
        described = self.graph.cite(
            f"pad-structure-option-{required['option']}", pinout, meaning[0][0], meaning[0][1],
            f"Pinout descriptor describes the I/O structure option {required['option']} as: {meaning[0][2]}.")
        cite = self.graph.cite(
            f"pad-structure-{pin}", pinout, start, end,
            f"Pinout descriptor gives pad {pin} the I/O structure {structure.get('type')} with options {', '.join(structure['options'])}. A declared structure, not a measured drive strength.")
        extend(self.resource(pin), [cite, described, rule])
        return True

    def allocation(self, spec):
        """A capability that allocates an instance and a routed pad without
        generating code. The application writes the driver calls from the
        symbols the allocation returns, so the card says what is and is not
        decided and cites where each symbol is declared."""
        rule, general = self.policy(spec["policy"]), self.policy("general")
        function, kind = spec["function"], spec["instance_kind"]
        covered = {record["id"] for record in self.value["resources"] if record["kind"] == kind}
        if not covered:
            raise SystemExit(f"{spec['capability']}: no {kind} instance")
        found = [edge for edge in self.value["edges"]
                 if edge["relation"] == "signal" and edge.get("attributes", {}).get("function") == function]
        stray = sorted({self.name(edge["from"]) for edge in found if edge["from"] not in covered})
        if stray:
            raise SystemExit(f"function {function} is routed from uncovered instances {stray}")
        excluded = spec.get("excluded_pads", [])
        text = self.policy_text(spec["policy"])
        for pin in excluded:
            # The policy line must name every pad it keeps out of the allocation.
            if not re.search(rf"(?<![A-Za-z0-9_]){re.escape(pin)}(?![A-Za-z0-9_])", text):
                raise SystemExit(f"{spec['capability']}: the policy does not exclude {pin}")
        marker = spec["pad_attribute"]
        found = [edge for edge in found if self.name(edge["to"]) in self.eligible and self.name(edge["to"]) not in excluded]
        if not found:
            raise SystemExit(f"no eligible route carries {function}")
        for pin in sorted({self.name(edge["to"]) for edge in found}):
            record = self.resource(pin)
            record["attributes"].update(marker)
            extend(record, [rule])
        cites = []
        symbol_spec = spec.get("alternate_symbol")
        for edge in found:
            for attribute in spec["route_attributes"]:
                if attribute not in edge["attributes"]:
                    raise SystemExit(f"{edge['id']} lacks {attribute}")
            if symbol_spec:
                # The SDK name of the selector, stated on the route it belongs to.
                alternate = edge["attributes"]["alternate"]
                text = symbol_spec["symbol"].format(alternate=alternate.replace(symbol_spec["split"], symbol_spec["split"] + "_", 1))
                cite = self.graph.cite_symbol("inv-alternate", self.source(symbol_spec["source"]), text)
                if edge["attributes"].setdefault("alternate_symbol", text) != text:
                    raise SystemExit(f"{edge['id']} already has another alternate_symbol")
                extend(edge, [cite])
                if cite not in cites:
                    cites.append(cite)
        if spec.get("share_instance"):
            for instance in sorted({edge["from"] for edge in found}):
                find(self.value["resources"], instance)["capacity"] = len({edge["to"] for edge in found if edge["from"] == instance})
        symbols = [self.graph.cite_symbol("inv-api", self.source(source), symbol) for source, symbol in spec.get("symbols", [])]
        quoted = [self.graph.cite(entry["id"], self.source(entry["source"]), entry["start"], entry["end"], entry["statement"])
                  for entry in spec.get("cites", [])]
        evidence = [rule, general, *symbols, *quoted, *spec.get("evidence", [])]
        for records, identifier in ((self.value["capabilities"], spec["capability"]), (self.value["implementations"], spec["card"])):
            if any(record["id"] == identifier for record in records):
                raise SystemExit(f"{identifier} already exists")
        card = {"id": spec["card"], "target": self.graph.target, "guidance": spec["guidance"],
                "evidence": [*evidence, *cites], "edits": []}
        if "component" in spec:
            card.update(self.component(spec, {spec["instance_role"]: {edge["from"] for edge in found},
                                              "pad": {edge["to"] for edge in found}}))
        self.value["capabilities"].append({
            "id": spec["capability"], "target": self.graph.target, "description": spec["description"],
            "parameters": copy.deepcopy(spec.get("parameters", {})),
            "roles": [
                {"id": spec["instance_role"], "kind": kind, "attributes": {}, "parameter_attributes": {}, "count": 1},
                {"id": "pad", "kind": "pin", "attributes": dict(marker), "parameter_attributes": {}, "count": 1},
            ],
            "links": [{"from": spec["instance_role"], "to": "pad", "relation": "signal", "attributes": {"function": function}}],
            "implementations": [spec["card"]], "parameter_constraints": [], "domain_complete": False,
            "closure_evidence": [], "evidence": list(evidence),
        })
        self.value["implementations"].append(card)
        return sorted({f"{self.name(edge['from'])}:{self.name(edge['to'])}" for edge in found})

    def instance_capability(self, spec):
        """A capability that takes one whole instance of a class and no pad:
        a unit that runs inside the chip, such as a time base, a watchdog or a
        checksum unit. With an interrupt role the instance must raise exactly
        one interrupt the graph records, and the capability takes that vector
        with it. Everything else is as for an allocation with a component."""
        rule = self.policy(spec["policy"])
        kind, role = spec["instance_kind"], spec["instance_role"]
        instances = sorted(record["id"] for record in self.value["resources"] if record["kind"] == kind)
        if not instances:
            raise SystemExit(f"{spec['capability']}: no {kind} instance")
        ends = {role: set(instances)}
        roles = [{"id": role, "kind": kind, "attributes": {}, "parameter_attributes": {}, "count": 1}]
        links = []
        if "interrupt_role" in spec:
            raised = {}
            for instance in instances:
                found = [edge["to"] for edge in self.value["edges"]
                         if edge["from"] == instance and edge["relation"] == "raises_interrupt"]
                if len(found) != 1:
                    raise SystemExit(f"{spec['capability']}: {self.name(instance)} raises {len(found)} recorded interrupts, not one")
                raised[instance] = found[0]
            ends[spec["interrupt_role"]] = set(raised.values())
            roles.append({"id": spec["interrupt_role"], "kind": "interrupt", "attributes": {}, "parameter_attributes": {}, "count": 1})
            links.append({"from": role, "to": spec["interrupt_role"], "relation": "raises_interrupt"})
        for instance in instances:
            extend(find(self.value["resources"], instance), [rule])
        symbols = [self.graph.cite_symbol("inv-api", self.source(source), symbol) for source, symbol in spec.get("symbols", [])]
        quoted = [self.graph.cite(entry["id"], self.source(entry["source"]), entry["start"], entry["end"], entry["statement"])
                  for entry in spec.get("cites", [])]
        evidence = [rule, *symbols, *quoted, *spec.get("evidence", [])]
        for records, identifier in ((self.value["capabilities"], spec["capability"]), (self.value["implementations"], spec["card"])):
            if any(record["id"] == identifier for record in records):
                raise SystemExit(f"{identifier} already exists")
        card = {"id": spec["card"], "target": self.graph.target, "guidance": spec["guidance"],
                "evidence": list(evidence), "edits": []}
        if "route_attributes" in spec:
            raise SystemExit(f"{spec['card']}: a capability without a pad has no route to read")
        parameters = copy.deepcopy(spec.get("parameters", {}))
        if "like" in spec:
            # The same reviewed component on another class of instance: its
            # parameters, unit and integration edits are taken whole, and
            # every value its templates read must exist on the new instances.
            if "component" in spec or "parameters" in spec:
                raise SystemExit(f"{spec['card']}: a card like a reviewed one takes its component and parameters from it")
            like = find(self.value["implementations"], spec["like"]["card"])
            parameters = copy.deepcopy(find(self.value["capabilities"], spec["like"]["capability"])["parameters"])
            card["component"], card["edits"] = copy.deepcopy(like["component"]), copy.deepcopy(like["edits"])
            self.bindings({**spec, "parameters": parameters}, card["component"]["bindings"], ends)
        else:
            card.update(self.component(spec, ends))
        self.value["capabilities"].append({
            "id": spec["capability"], "target": self.graph.target, "description": spec["description"],
            "parameters": parameters, "roles": roles, "links": links,
            "implementations": [spec["card"]], "parameter_constraints": [], "domain_complete": False,
            "closure_evidence": [], "evidence": list(evidence),
        })
        self.value["implementations"].append(card)
        return [self.name(instance) for instance in instances]

    def pad_capability(self, spec):
        """A capability that takes one pad eligible for general allocation and
        generates nothing: the card states how the application uses the pad
        through the SDK, as the reviewed GPIO output card does."""
        rule, general = self.policy(spec["policy"]), self.policy("general")
        pins = list(self.eligible)
        if not pins:
            raise SystemExit(f"{spec['capability']}: no pad is eligible")
        for pin in pins:
            record = self.resource(pin)
            record["attributes"].update(spec["pad_attribute"])
            extend(record, [rule])
        symbols = [self.graph.cite_symbol("inv-api", self.source(source), symbol) for source, symbol in spec.get("symbols", [])]
        evidence = [rule, general, *symbols, *spec.get("evidence", [])]
        for records, identifier in ((self.value["capabilities"], spec["capability"]), (self.value["implementations"], spec["card"])):
            if any(record["id"] == identifier for record in records):
                raise SystemExit(f"{identifier} already exists")
        parameters = copy.deepcopy(spec.get("parameters", {}))
        if "pin" in parameters:
            raise SystemExit(f"{spec['capability']}: the pad choice is added here")
        parameters = {"pin": {"type": "choice", "values": pins}, **parameters}
        if len(pins) > inventory.LIMITS["choices"]:
            raise SystemExit(f"choice domain exceeds {inventory.LIMITS['choices']} values")
        self.value["capabilities"].append({
            "id": spec["capability"], "target": self.graph.target, "description": spec["description"],
            "parameters": parameters,
            "roles": [{"id": "pin", "kind": "pin", "attributes": dict(spec["pad_attribute"]),
                       "parameter_attributes": {"pin": "pin"}, "count": 1}],
            "links": [], "implementations": [spec["card"]], "parameter_constraints": [], "domain_complete": False,
            "closure_evidence": [], "evidence": list(evidence),
        })
        self.value["implementations"].append({"id": spec["card"], "target": self.graph.target, "guidance": spec["guidance"],
                                              "evidence": list(evidence), "edits": []})
        return len(pins)

    def static_edits(self, spec):
        """Integration edits a component needs that no reviewed card writes
        yet, such as switching on an SDK module nothing used before. Each one
        names the pinned foundation file it changes: that file must be the
        bytes the reviewed edits of the same path are pinned to, and hold the
        text to replace exactly once."""
        edits = []
        for edit in spec.get("static_edits", []):
            pinned = {existing["expected_sha256"] for card in self.value["implementations"]
                      for existing in [*card["edits"], *card.get("component", {}).get("edits", [])]
                      if existing["path"] == edit["path"]}
            if len(pinned) != 1:
                raise SystemExit(f"{spec['card']}: no single reviewed hash for {edit['path']}")
            record = find(self.value["sources"], edit["source"])
            if record["sha256"] not in pinned:
                raise SystemExit(f"{spec['card']}: source {edit['source']} is not the file the reviewed edits of {edit['path']} are pinned to")
            source = self.source(edit["source"])
            if source.excerpt(1, len(source.lines)).count(edit["old"]) != 1:
                raise SystemExit(f"{spec['card']}: {edit['path']} does not hold the text to replace exactly once: {edit['old'][:60]!r}")
            if edit["mode"] not in ("replace", "insert_before"):
                raise SystemExit(f"{spec['card']}: unknown edit mode {edit['mode']}")
            edits.append({"path": edit["path"], "expected_sha256": record["sha256"], "old": edit["old"],
                          "new": edit["new"], "mode": edit["mode"]})
        return edits

    def component(self, spec, ends):
        """A generated unit for an allocation. Its integration edits are not
        written a second time: they are the edits of a reviewed card, taken
        unchanged, so two cards that need the same module agree byte for byte
        and stay pinned to the same foundation files. Templates are named by
        the unit's files and interface, never as evidence of the card: a
        template is what is generated, not a source for a claim. Everything
        the engine would refuse later, and everything the shared edits take
        for granted about the unit, is refused here with the reason."""
        card, shared = spec["card"], spec["edits_from"]
        like = find(self.value["implementations"], shared["card"])
        static = []
        for text in shared.get("static", []):
            edits = [edit for edit in like["edits"] if text in edit["new"]]
            if len(edits) != 1:
                raise SystemExit(f"{card}: expected one edit of {shared['card']} that writes {text!r}, found {len(edits)}")
            static.append(copy.deepcopy(edits[0]))
        static += self.static_edits(spec)
        component = copy.deepcopy(spec["component"])
        if "edits" in component:
            raise SystemExit(f"{card}: a component takes its integration edits from a reviewed card")
        component["edits"] = copy.deepcopy(like["component"]["edits"])
        templates = {record["id"]: record for record in self.value["evidence"] if record.get("role") == "template"}
        cited = [file["template"] for file in component["files"]]
        cited += [identifier for group in component.get("interface_sources", {}).values() for identifier in group]
        missing = sorted(set(cited) - set(templates))
        if missing:
            raise SystemExit(f"{card}: no template evidence {missing}")
        self.bindings(spec, component["bindings"], ends)
        # The shared edits compile a file, include a header and call a
        # function of the unit: this unit must generate and define them.
        paths = {file["path"] for file in component["files"]}
        generated = "\n".join(
            self.source(templates[file["template"]]["source"]).excerpt(
                templates[file["template"]]["start_line"], templates[file["template"]]["end_line"])
            for file in component["files"])
        for edit in component["edits"]:
            unbound = sorted(set(re.findall(r"\{\{([A-Za-z0-9_]+)\}\}", edit["new"])) - set(component["bindings"]))
            if unbound:
                raise SystemExit(f"{card}: a shared edit uses {unbound}, which this component does not bind")
            for path in re.findall(r"[A-Za-z0-9_/.-]*\{\{[A-Za-z0-9_]+\}\}[A-Za-z0-9_.-]*\.[ch](?![A-Za-z0-9_])", edit["new"]):
                if path not in paths:
                    raise SystemExit(f"{card}: a shared edit names {path}, which this component does not generate")
            for call in re.findall(r"\{\{[A-Za-z0-9_]+\}\}_[A-Za-z0-9_]+(?=\()", edit["new"]):
                if call + "(" not in generated:
                    raise SystemExit(f"{card}: a shared edit calls {call}, which this component does not define")
        return {"component": component, "edits": static}

    def bindings(self, spec, bindings, ends):
        """Each value a template takes is one the allocation can always
        supply: `ends` gives, for every role, the resources it may be given."""
        card, parameters = spec["card"], spec.get("parameters", {})
        for name, binding in bindings.items():
            source, table = binding["source"], binding.get("map")
            kind = source["type"]
            if kind == "instance":
                if table is not None:
                    raise SystemExit(f"{card}: binding {name} names the unit and cannot be mapped")
            elif kind == "parameter":
                if source["name"] not in parameters:
                    raise SystemExit(f"{card}: binding {name} reads a parameter the capability does not have")
                if table is not None and set(table) != set(parameters[source["name"]]["values"]):
                    raise SystemExit(f"{card}: binding {name} does not map exactly the choices of {source['name']}")
            elif kind == "resource_attribute":
                if source["role"] not in ends:
                    raise SystemExit(f"{card}: binding {name} reads a role the allocation does not have")
                lacking = sorted(self.name(identifier) for identifier in ends[source["role"]]
                                 if source["attribute"] not in find(self.value["resources"], identifier)["attributes"])
                if lacking:
                    raise SystemExit(f"{card}: binding {name} needs {source['attribute']}, which {lacking} do not have")
            elif kind == "route_attribute":
                if "pad" not in ends or (source["from"], source["to"], source["relation"]) != (spec["instance_role"], "pad", "signal"):
                    raise SystemExit(f"{card}: binding {name} reads a route the allocation does not have")
                if source["attribute"] not in spec["route_attributes"]:
                    raise SystemExit(f"{card}: binding {name} reads {source['attribute']}, which the allocation does not require of every route")
            else:
                raise SystemExit(f"{card}: binding {name} has an unknown source {kind}")
            for value in (table or {}).values():
                if not 0 < len(value) <= 256 or any(not 32 <= ord(letter) <= 126 for letter in value) or "{{" in value or "}}" in value:
                    raise SystemExit(f"{card}: binding {name} maps to a value the engine does not accept")


def run(graph, sources, summary, inventory_config, config, source_root):
    """The domains stage over a graph the inventory stage has filled."""
    inventory.use_schema(graph.value, config.get("graph_schema"))
    domains = Domains(graph, sources, inventory_config, config, source_root)
    domains.board()
    domains.foundation_pads()
    summary["general_allocation_pads"] = len(domains.eligible)
    summary["gpio_output_pads"] = domains.gpio_output()
    summary["gpio_interrupt_pads"] = domains.gpio_interrupt()
    domains.templates()
    if config.get("route_marks"):
        summary["route_marks"] = domains.route_marks()
    summary["promoted_instances"], skipped = domains.classes()
    if skipped:
        summary["instances_not_promoted"] = skipped
    summary["profiles"] = [domains.profile(spec) for spec in config["profiles"]]
    summary["allocations"] = {spec["capability"]: domains.allocation(spec) for spec in config.get("allocations", [])}
    if config.get("instance_capabilities"):
        summary["instance_capabilities"] = {spec["capability"]: domains.instance_capability(spec)
                                            for spec in config["instance_capabilities"]}
    if config.get("pad_capabilities"):
        summary["pad_capabilities"] = {spec["capability"]: domains.pad_capability(spec) for spec in config["pad_capabilities"]}
    summary["reservations"] = [domains.name(record["resource"]) for record in domains.target["reservations"]]
    summary["interrupts_given_their_vector_lines"] = domains.vectors()
    domains.restatements()
    domains.service_restatements()
    domains.service_evidence()
    domains.supersede()
    if domains.unbonded:
        summary["unbonded_signals"] = sorted(domains.unbonded)
    if domains.narrowed:
        summary["narrowed"] = domains.narrowed
    return summary
