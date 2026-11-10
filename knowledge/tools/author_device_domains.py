# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Open reviewed capabilities to the whole device, on top of the inventory.

The inventory stage records which pads, routes and interrupts are declared.
This stage decides which of them a capability may use, from two kinds of input
that are both cited line by line:

* the board netlist, which says what else is connected to each device pad;
* an authored policy text, which says how that is turned into allocation rules.

It then widens existing capabilities to that domain. A capability that needed a
fixed pad now accepts any eligible pad its signal is routed to, and reads the
alternate function from the selected route. No component template changes.

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


class Domains:
    def __init__(self, graph, sources, inventory_config, config, source_root):
        self.graph, self.sources, self.config = graph, sources, config
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

    def policy(self, name):
        line = self.config["policy"]["lines"][name]
        text = self.policy_source.lines[line - 1]
        return self.graph.cite(f"policy3-{name}", self.policy_source, line, line,
                               "BKPT policy, version 3: " + text.split(":", 1)[0] + ". Authored engineering policy, not a manufacturer statement.")

    def resource(self, pin):
        return find(self.value["resources"], f"{self.prefix}.{pin}")

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
        for old, new in spec["guidance"]:
            if card["guidance"].count(old) != 1:
                raise SystemExit(f"{card['id']}: guidance anchor not found exactly once")
            card["guidance"] = card["guidance"].replace(old, new)
        extend(card, [rule])
        return len(pins)

    def gpio_interrupt(self):
        spec = self.config["gpio_interrupt"]
        capability = find(self.value["capabilities"], spec["capability"])
        rule = self.policy("gpio_interrupt")
        source = self.sources.get(spec["exti_source"]) or inventory.Source(
            find(self.value["sources"], spec["exti_source"]), self.source_root)
        self.sources[spec["exti_source"]] = source
        pins = self.allocatable(capability["id"])
        pads = {pin_name(pad): pad for pad in self.pads.values()}
        edges = {edge["id"] for edge in self.value["edges"]}

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
                if identifier in edges:
                    continue
                self.value["edges"].append({
                    "id": identifier, "target": self.graph.target, "from": start, "to": end,
                    "relation": relation, "conditions": dict(self.conditions), "evidence": evidence})
                edges.add(identifier)
                self.graph.added["edges"] += 1
        self.choices(capability, "pin", pins)
        capability["description"] = spec["description"]
        extend(capability, [rule, self.policy("general")])
        extend(find(self.value["implementations"], spec["card"]), [rule])
        return len(pins)

    def signal_pins(self, spec):
        """Let a capability take each named signal on any eligible pad it is
        routed to, binding the alternate function from the selected route."""
        rule = self.policy(spec["policy"])
        general = self.config["general_attribute"]
        symbol_spec = spec["alternate_symbol"]
        symbol_source = self.sources[symbol_spec["source"]]
        routes = {}
        for role, binding in spec["roles"].items():
            found = [edge for edge in self.value["edges"]
                     if edge["relation"] == "signal" and edge.get("attributes", {}).get("signal") == binding["signal"]
                     and edge["to"].split(".", 1)[1] in self.eligible]
            if not found:
                raise SystemExit(f"no eligible route carries {binding['signal']}")
            routes[role] = found
        mapping, cites = {}, []
        for alternate in sorted({edge["attributes"]["alternate"] for found in routes.values() for edge in found}):
            text = symbol_spec["symbol"].format(alternate=alternate.replace(symbol_spec["split"], symbol_spec["split"] + "_", 1))
            mapping[alternate] = text
            cites.append(self.graph.cite_symbol("inv-alternate", symbol_source, text))
        for identifier in spec["capabilities"]:
            capability = find(self.value["capabilities"], identifier)
            for role, binding in spec["roles"].items():
                record = find(capability["roles"], role)
                # The pad this role was reviewed with stays the first choice,
                # so widening the domain does not move an existing design.
                reviewed = [resource["id"] for resource in self.value["resources"]
                            if resource["kind"] == record["kind"] and record["attributes"]
                            and all(resource["attributes"].get(key) == value
                                    for key, value in record["attributes"].items())]
                if len(reviewed) == 1:
                    record["prefer"] = reviewed
                record["attributes"] = dict(general)
                links = [link for link in capability["links"] if link["from"] == binding["from"] and link["to"] == role]
                if len(links) != 1:
                    raise SystemExit(f"{identifier}: expected one link from {binding['from']} to {role}")
                links[0]["relation"] = "signal"
                links[0]["attributes"] = {"signal": binding["signal"]}
            extend(capability, [rule, self.policy("general")])
        for identifier in spec["cards"]:
            card = find(self.value["implementations"], identifier)
            for role, binding in spec["roles"].items():
                if binding["binding"] not in card["component"]["bindings"]:
                    raise SystemExit(f"{identifier}: no binding {binding['binding']}")
                card["component"]["bindings"][binding["binding"]] = {
                    "source": {"type": "route_attribute", "from": binding["from"], "to": role,
                               "relation": "signal", "attribute": "alternate"},
                    "map": dict(mapping),
                }
            extend(card, [rule, *cites])
        return {role: sorted(edge["to"].split(".", 1)[1] for edge in found) for role, found in routes.items()}


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
    domains.source_root = arguments.source_root
    domains.board()
    summary["general_allocation_pads"] = len(domains.eligible)
    summary["gpio_output_pads"] = domains.gpio_output()
    summary["gpio_interrupt_pads"] = domains.gpio_interrupt()
    summary["signal_routes"] = [domains.signal_pins(spec) for spec in config["signal_pins"]]
    summary["reservations"] = [record["resource"].split(".", 1)[1] for record in domains.target["reservations"]]
    inventory.write(graph, arguments.out, summary)


if __name__ == "__main__":
    sys.exit(main())
