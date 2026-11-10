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
        self.unbonded = set()

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
        if spec.get("kind", "netlist") == "declared":
            self.declared_board(spec)
        else:
            self.netlist_board(spec)
        for old, new in self.config.get("assumption_replacements", []):
            # A reviewed assumption that names the old, narrower domain is restated.
            if self.target["assumptions"].count(old) != 1:
                raise SystemExit("assumption to restate not found exactly once")
            self.target["assumptions"][self.target["assumptions"].index(old)] = new
        self.target["assumptions"].append(self.config["assumption"])
        if len(self.target["assumptions"]) > 16:
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
        rule_line = description.find(r'^\s*"eligible"\s*:')
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
            if all(passive.fullmatch(reference) for reference in others):
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
            if missing and self.config.get("unbonded_signals") != "report":
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

    def request_facts(self, instance, direction):
        """What the sources say about one DMA request line: its table row and
        selector, and per controller the SDK names that carry that selector.
        Refuses unless the table and every controller's definitions agree."""
        spec = self.config["dma_requests"]
        table = self.source(spec["table"]["source"])
        row = re.escape(spec["table"]["row"].format(selector="@", name=instance.lower(), direction=direction))
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

    def request(self, instance, direction):
        """One DMA request line, recorded only when the manual's table and the
        SDK definitions for every controller agree on its selector. A line the
        base already records is held to the same facts instead of being
        trusted: a base that changed underneath is refused, not preserved."""
        spec = self.config["dma_requests"]
        identifier = f"{self.prefix}.{instance}.{direction}.request"
        table, line, selector, names = self.request_facts(instance, direction)
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
        self.graph.resource(identifier, spec["kind"], attributes, self.conditions, [*evidence, rule, *spec["evidence"]])
        self.graph.edge(owner_edge, owner, identifier, f"{direction}_request", None, self.conditions, [evidence[0], rule])
        selection = spec["channel_selection"]
        select = self.graph.cite(selection["id"], self.source(selection["source"]), selection["start"], selection["end"],
                                 selection["statement"])
        for edge, channel in routes.items():
            self.graph.edge(edge, identifier, channel, "routes_to", None, self.conditions,
                            [evidence[0], select, *spec["channel_evidence"]])
        return identifier

    def interrupt(self, instance, name, block, source, record, rule):
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
                            [cite, f"inv-irq-{name}", rule])

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
            for attribute, (source_id, symbol, *names) in spec["symbols"].items():
                attributes[attribute] = symbol
                cited = self.source(source_id)
                evidence.append(self.graph.cite_symbol("inv-instance", cited, symbol))
                # A symbol that exists but belongs to another instance would
                # pass an existence check, so the excerpt must also name what
                # the configuration says it refers to.
                start, end, _ = inventory.declaration(cited, symbol)
                excerpt = cited.excerpt(start, end)
                for template in names:
                    name = template.format(instance=instance, lower=instance.lower())
                    if not re.search(r"(?<![A-Za-z0-9])" + re.escape(name) + r"(?![A-Za-z0-9_])", excerpt):
                        raise SystemExit(f"{instance}: {symbol} does not refer to {name}")
            record = self.resource(instance)
            if record["kind"] == description["unpromoted_kind"]:
                record["kind"] = spec["kind"]
                promoted.append(instance)
            elif record["kind"] != spec["kind"]:
                raise SystemExit(f"{instance} is already a {record['kind']}")
            inventory.complete(record, attributes, evidence)
            if "capacity" in spec:
                # One physical unit that several allocations may name at once.
                record["capacity"] = spec["capacity"]
            if "interrupt" in spec:
                self.interrupt(instance, spec["interrupt"], block, source, record, rule)
            for direction in spec.get("requests", []):
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
                first = find(self.value["resources"], reviewed[0])
                if any(first["attributes"].get(key) != value for key, value in record["attributes"].items()):
                    raise SystemExit(f"{capability['id']}.{role}: the reviewed {self.name(reviewed[0])} is outside the new domain")
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
        symbols = [self.graph.cite_symbol("inv-api", self.source(source), symbol) for source, symbol in spec.get("symbols", [])]
        quoted = [self.graph.cite(entry["id"], self.source(entry["source"]), entry["start"], entry["end"], entry["statement"])
                  for entry in spec.get("cites", [])]
        evidence = [rule, general, *symbols, *quoted, *spec.get("evidence", [])]
        for records, identifier in ((self.value["capabilities"], spec["capability"]), (self.value["implementations"], spec["card"])):
            if any(record["id"] == identifier for record in records):
                raise SystemExit(f"{identifier} already exists")
        self.value["capabilities"].append({
            "id": spec["capability"], "target": self.graph.target, "description": spec["description"], "parameters": {},
            "roles": [
                {"id": spec["instance_role"], "kind": kind, "attributes": {}, "parameter_attributes": {}, "count": 1},
                {"id": "pad", "kind": "pin", "attributes": dict(marker), "parameter_attributes": {}, "count": 1},
            ],
            "links": [{"from": spec["instance_role"], "to": "pad", "relation": "signal", "attributes": {"function": function}}],
            "implementations": [spec["card"]], "parameter_constraints": [], "domain_complete": False,
            "closure_evidence": [], "evidence": list(evidence),
        })
        self.value["implementations"].append({
            "id": spec["card"], "target": self.graph.target, "guidance": spec["guidance"],
            "evidence": [*evidence, *cites], "edits": [],
        })
        return sorted({f"{self.name(edge['from'])}:{self.name(edge['to'])}" for edge in found})


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
    run(graph, sources, summary, inventory_config, config, arguments.source_root)
    inventory.write(graph, arguments.out, summary)


def run(graph, sources, summary, inventory_config, config, source_root):
    """The domains stage over a graph the inventory stage has filled."""
    domains = Domains(graph, sources, inventory_config, config, source_root)
    domains.board()
    summary["general_allocation_pads"] = len(domains.eligible)
    summary["gpio_output_pads"] = domains.gpio_output()
    summary["gpio_interrupt_pads"] = domains.gpio_interrupt()
    domains.templates()
    summary["route_marks"] = domains.route_marks()
    if domains.unbonded:
        summary["unbonded_signals"] = sorted(domains.unbonded)
    summary["promoted_instances"] = domains.instances()
    summary["profiles"] = [domains.profile(spec) for spec in config["profiles"]]
    summary["allocations"] = {spec["capability"]: domains.allocation(spec) for spec in config.get("allocations", [])}
    summary["reservations"] = [domains.name(record["resource"]) for record in domains.target["reservations"]]
    return summary


if __name__ == "__main__":
    sys.exit(main())
