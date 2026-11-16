# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Record the clock tree, the peripheral clocks and one clock profile.

This stage runs after the domains stage and adds records only. It reads:

* the manufacturer's clock descriptor: every element becomes a clock node and
  every connection between two elements a clock input;
* the SDK's RCC header: every name under which a clock enable function is
  compiled for the part becomes a peripheral clock record with the functions
  the header defines under that name, and every name with a kernel clock
  source setter a kernel clock record with the enumerators of its type;
* the captured clock configuration: the selections it makes become a profile,
  and the frequency of every node those selections determine is derived by
  arithmetic over the descriptor and assigned under that profile.

Whether a line of a header is compiled is decided from the header's own
conditional lines and the macros the part's device header defines. Two
manufacturer sources are paired only where the configuration says so and the
pairing can be checked: the enumerators of a kernel clock setter must be the
inputs of the descriptor selector it is paired with.

No role, link or parameter of a capability changes, so a request keeps its
plan. A capability gains citations of the clocks its rates rest on, existing
resources gain none of the new attributes, and the target gains one service
that states the profile.

`compose_target.py` runs this stage when a recipe names a clocks
configuration, so a graph composed from its layers holds the same records.
The command line runs the stage alone on a graph that does not hold them yet:

    python author_device_clocks.py --graph stm32c5/graph.json \
        --inventory stm32c5/imports/device-inventory.json \
        --config stm32c5/imports/device-clocks.json --source-root ROOT --out NEW.json
"""
import fractions
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_inventory as inventory  # noqa: E402
import descriptor  # noqa: E402

DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
DEFINED = re.compile(r"^(!?)\s*defined\s*\(?\s*([A-Za-z_]\w*)\s*\)?$")
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)")
FUNCTION = re.compile(r"^(?:__STATIC_INLINE\s+)?[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(")
ATTRIBUTE_BYTES = 256
# The firmware tool refuses a citation whose record and excerpt together are
# more than 18,000 bytes as JSON. An excerpt is kept well inside that.
EXCERPT_BYTES = 15000


def find(records, identifier):
    matches = [record for record in records if record["id"] == identifier]
    if len(matches) != 1:
        raise SystemExit(f"expected one record {identifier!r}, found {len(matches)}")
    return matches[0]


def joined(owner, key, values):
    """A list as one attribute value. An empty list is no attribute; a value
    the engine would refuse is refused here, with the record that holds it."""
    text = ",".join(str(value) for value in values)
    if len(text) > ATTRIBUTE_BYTES:
        raise SystemExit(f"{owner}: {key} is longer than an attribute may be")
    return text


def number(value):
    """A descriptor number as the exact text of its value."""
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise SystemExit(f"not a number: {value!r}")
    fraction = fractions.Fraction(str(value))
    return str(fraction.numerator) if fraction.denominator == 1 else str(float(fraction))


class Conditional:
    """Which lines of a C source the preprocessor keeps, for a set of macros.

    Only tests of the form `defined(NAME)`, `!defined(NAME)`, `#ifdef` and
    `#ifndef` are decided, and any other test whose exact text the caller has
    settled. A line under any other test, or under a macro the configuration
    lists as undetermined, has no verdict: it is neither kept nor dropped, and
    whoever asks about it is told so."""

    def __init__(self, source, defined, undetermined=(), settled=None):
        self.source = source
        self.settled = {" ".join(text.split()): verdict for text, verdict in (settled or {}).items()}
        self.state = []
        stack = []
        for index, line in enumerate(source.lines):
            directive = DIRECTIVE.match(line)
            if not directive:
                self.state.append(self.combine(stack))
                continue
            word, rest = directive.group(1), re.sub(r"/\*.*?\*/|//.*$", "", directive.group(2)).strip()
            if word not in ("endif", "else") and not rest:
                raise SystemExit(f"{source.id}: conditional without a test at line {index + 1}")
            if word in ("if", "ifdef", "ifndef"):
                stack.append({"taken": None, "now": None})
                stack[-1]["taken"] = stack[-1]["now"] = self.decide(word, rest, defined, undetermined)
            elif not stack:
                raise SystemExit(f"{source.id}: #{word} without #if at line {index + 1}")
            elif word == "elif":
                stack[-1]["now"] = self.branch(stack[-1], self.decide("if", rest, defined, undetermined))
            elif word == "else":
                stack[-1]["now"] = self.branch(stack[-1], True)
            else:
                stack.pop()
                self.state.append(self.combine(stack))
                continue
            # A directive line belongs to the conditional around it.
            self.state.append(self.combine(stack[:-1]))
        if stack:
            raise SystemExit(f"{source.id}: unterminated conditional")

    def decide(self, word, rest, defined, undetermined):
        """The verdict of one test: what the caller settled for its exact
        text, else what its one macro decides."""
        if word == "if":
            settled = self.settled.get(" ".join(rest.split()))
            if settled is not None:
                return settled
        return self.test(word, rest, defined, undetermined)

    @staticmethod
    def test(word, rest, defined, undetermined):
        text = rest if word == "if" else ("" if word == "ifdef" else "!") + f"defined({rest})"
        parsed = DEFINED.match(text)
        if not parsed or parsed.group(2) in undetermined:
            return None
        return (parsed.group(2) in defined) != bool(parsed.group(1))

    @staticmethod
    def branch(frame, verdict):
        """The verdict of a later branch of one conditional: dropped once an
        earlier branch is kept, undecided while an earlier one is undecided."""
        if frame["taken"] is True:
            return False
        if frame["taken"] is None:
            return None if verdict is not False else False
        frame["taken"] = verdict
        return verdict

    @staticmethod
    def combine(frames):
        if any(frame["now"] is False for frame in frames):
            return False
        if any(frame["now"] is None for frame in frames):
            return None
        return True

    def kept(self, line):
        return self.state[line - 1]


def macros(source):
    """Every macro a source defines, wherever it stands in it."""
    return {match.group(1) for match in map(DEFINE.match, source.lines) if match}


class Clocks:
    def __init__(self, graph, sources, inventory_config, config, source_root):
        self.graph, self.sources, self.config = graph, sources, config
        self.source_root = source_root
        self.value = graph.value
        self.prefix = inventory_config["resource_prefix"]
        self.conditions = dict(inventory_config["conditions"])
        self.target = find(self.value["targets"], inventory_config["target"])
        if self.value["schema"] < 5:
            raise SystemExit("the clock stage needs graph schema 5")
        for record in config["sources"]:
            data = (source_root / record["path"]).read_bytes()
            if record.get("sha256", inventory.sha256(data)) != inventory.sha256(data):
                raise SystemExit(f"source {record['id']} differs from the hash the configuration pins")
            if any(existing["id"] == record["id"] for existing in self.value["sources"]):
                raise SystemExit(f"source {record['id']} is already pinned")
            if record.get("scope") not in ("device", "board", "sdk", "policy", "implementation"):
                raise SystemExit(f"source {record['id']} declares no scope")
            pinned = {key: record[key] for key in ("id", "path", "revision", "authority")}
            pinned["sha256"] = inventory.sha256(data)
            pinned = {key: pinned[key] for key in ("id", "path", "sha256", "revision", "authority")}
            pinned["scope"] = record["scope"]
            self.value["sources"].append(pinned)
            sources[record["id"]] = inventory.Source(pinned, source_root)
        if len(self.value["sources"]) > inventory.LIMITS["sources"]:
            raise SystemExit(f"more than {inventory.LIMITS['sources']} sources")
        self.located = {}
        self.blanked = {}
        for record in self.value["evidence"]:
            self.located.setdefault((record["source"], record["start_line"], record["end_line"]), record["id"])
        self.summary = {}

    # Citations.

    def source(self, identifier):
        """A pinned source, read when it is first needed and checked against
        the hash the graph records."""
        if identifier not in self.sources:
            record = [source for source in self.value["sources"] if source["id"] == identifier]
            if len(record) != 1:
                raise SystemExit(f"the clock stage needs the source {identifier}, which the graph does not pin")
            self.sources[identifier] = inventory.Source(record[0], self.source_root)
        return self.sources[identifier]

    def cite(self, identifier, source, start, end, statement):
        """Cite lines once: a place an earlier stage already cites keeps the
        identifier and the statement it was reviewed with."""
        known = self.located.get((source.id, start, end))
        if known:
            return known
        self.located[(source.id, start, end)] = identifier
        return self.graph.cite(identifier, source, start, end, statement)

    @staticmethod
    def spans(source, start, end):
        """Consecutive spans of lines that cover start to end, each short
        enough for one citation in lines and in bytes."""
        found, opened, size = [], start, 0
        for line in range(start, end + 1):
            cost = len(json.dumps(source.lines[line - 1])) + 1
            if line > opened and (line - opened >= inventory.LIMITS["excerpt_lines"] - 1 or size + cost > EXCERPT_BYTES):
                found.append((opened, line - 1))
                opened, size = line, 0
            size += cost
        found.append((opened, end))
        return found

    def cite_symbol(self, source, symbol):
        start, end, _ = inventory.declaration(source, symbol)
        known = self.located.get((source.id, start, end))
        if known:
            return known
        identifier = self.graph.cite_symbol("rcc", source, symbol)
        self.located[(source.id, start, end)] = identifier
        return identifier

    def cite_line(self, prefix, source_id, pattern, statement, following=0):
        """Cite the one line a pattern finds, with the lines that follow it
        when the statement is about them too."""
        source = self.source(source_id)
        line = source.find(pattern)
        return self.cite(f"{prefix}-{source_id}-{line}", source, line, line + following, statement)

    def code_of(self, source):
        """The lines of a C source with its comments, the contents of its
        string and character literals and its blocks under a zero `#if`
        blanked, so that a search finds code only. A backslash at the end of
        a line joins the next line to it before anything else is read, so a
        comment, a literal or a directive may continue over lines. What is
        left is given back physical line by physical line, and a line that is
        joined to another is given back empty: the continuation of a
        directive or of a statement is never read as a line of its own. A
        comment that spans lines continues its line too, so a directive that
        goes on behind such a comment is given back empty as a whole. No
        other condition of the preprocessor is evaluated, no macro is expanded
        and no trigraph or digraph is read."""
        if source.id not in self.blanked:
            # Join: a backslash, blanks after it and the line end vanish.
            characters, owner, joined_lines = [], [], set()
            for number, line in enumerate(source.lines):
                joined_on = re.search(r"\\[ \t]*$", line)
                kept = line[:joined_on.start()] if joined_on else line + "\n"
                if joined_on:
                    joined_lines.update((number, number + 1))
                characters.extend(kept)
                owner.extend([number] * len(kept))
            text, out, state, escaped, index = "".join(characters), [], "code", False, 0
            while index < len(text):
                character, pair = text[index], text[index:index + 2]
                if state == "code" and pair in ("/*", "//"):
                    state = "block" if pair == "/*" else "line"
                    out.extend("  ")
                    index += 2
                    continue
                if state == "block" and pair == "*/":
                    state = "code"
                    out.extend("  ")
                    index += 2
                    continue
                if character == "\n":
                    # A line comment and an unterminated literal end with their
                    # joined line. A block comment does not: for the compiler
                    # it is one blank, and the line goes on behind it.
                    out.append(" " if state == "block" else character)
                    if state != "block":
                        state = "code"
                    escaped = False
                elif state == "code":
                    if character in "\"'":
                        state = character
                    out.append(character)
                elif state in ("block", "line"):
                    out.append(" ")
                elif escaped:
                    escaped = False
                    out.append(" ")
                elif character == "\\":
                    escaped = True
                    out.append(" ")
                elif character == state:
                    state = "code"
                    out.append(character)
                else:
                    out.append(" ")
                index += 1
            # A block under an #if or #elif whose condition is the constant zero, to the next #else, #elif or #endif of its conditional.
            depth, begin = 0, 0
            for finish in [position for position, character in enumerate(out) if character == "\n"]:
                directive = DIRECTIVE.match("".join(out[begin:finish]))
                word, rest = (directive.group(1), directive.group(2)) if directive else (None, "")
                zero = word in ("if", "elif") and bool(re.fullmatch(r"[\s(]*(?:0+|0[xX]0+|0[bB]0+)[uUlL]*[\s)]*", rest))
                dead = bool(depth)
                if depth:
                    if word in ("if", "ifdef", "ifndef"):
                        depth += 1
                    elif word == "endif":
                        depth -= 1
                    elif word in ("else", "elif") and depth == 1 and not zero:
                        depth = 0
                elif zero:
                    depth, dead = 1, True
                if dead or (owner[begin] != owner[finish] and re.match(r"^\s*#", "".join(out[begin:finish]))):
                    # Never compiled, or a directive of any kind that continues over lines.
                    out[begin:finish] = " " * (finish - begin)
                begin = finish + 1
            lines = [[] for _ in source.lines]
            for character, number in zip(out, owner):
                if character != "\n":
                    lines[number].append(character)
            self.blanked[source.id] = ["" if number in joined_lines else "".join(line) for number, line in enumerate(lines)]
        return self.blanked[source.id]

    @staticmethod
    def called(call, line):
        """Whether what is left of a line holds the call; a preprocessor
        directive defines text and calls nothing."""
        return not re.match(r"^\s*#", line) and bool(call.search(line))

    @staticmethod
    def held(instance, capabilities):
        """The case a component's selection is stated for."""
        return (f"{instance} is assigned to " + ("the capability " if len(capabilities) == 1 else "one of the capabilities ")
                + ", ".join(capabilities) + " and that capability's initialization of it has completed successfully")

    def policy(self, name):
        policy = self.config["policy"]
        source = self.source(policy["source"])
        line = policy["lines"][name]
        text = source.lines[line - 1]
        return self.graph.cite(f"{policy['evidence_prefix']}-{name}", source, line, line,
                               f"{policy['label']}: " + text.split(":", 1)[0] + ". Authored engineering policy, not a manufacturer statement.")

    def add(self, identifier, kind, attributes, evidence, conditions=None):
        if len(attributes) > 32:
            raise SystemExit(f"{identifier} has more attributes than a record may carry")
        for key, text in attributes.items():
            if not text or len(text) > ATTRIBUTE_BYTES:
                raise SystemExit(f"{identifier}: attribute {key} is empty or too long")
        if len(evidence) > inventory.LIMITS["citations"]:
            raise SystemExit(f"{identifier} cites more than {inventory.LIMITS['citations']} sources")
        if not self.graph.resource(identifier, kind, attributes, conditions or self.conditions, list(dict.fromkeys(evidence))):
            raise SystemExit(f"resource {identifier} already exists")

    def link(self, identifier, start, end, relation, evidence, attributes=None):
        self.graph.edge(identifier, start, end, relation, attributes or {}, self.conditions, list(dict.fromkeys(evidence)))

    def instances(self):
        """Resources that stand for one peripheral instance, by its name."""
        named = {}
        for record in self.value["resources"]:
            name = record["attributes"].get("instance")
            if name and record["kind"] not in self.config["own_kinds"]:
                named.setdefault(name, []).append(record)
        return named

    # The clock tree.

    def tree(self):
        spec = self.config["descriptor"]
        source = self.source(spec["source"])
        root = descriptor.parse(source)
        elements = {}
        for element in root["tree"]["elements"]:
            value = element.value
            if value["id"] in elements:
                raise SystemExit(f"clock descriptor repeats element {value['id']}")
            elements[value["id"]] = element
        self.elements = elements
        self.element_cited = {}
        policy = self.policy("clock_nodes")
        self.inputs = {name: {} for name in elements}  # element -> {upstream element: label or None}
        edges = {}
        absent, unlisted = [], []
        for name, element in elements.items():
            value = element.value
            if value["type"] == "multiplexor":
                for entry in element["possible_Input"]:
                    upstream, label = entry.value["from"], entry.value["label"]
                    if upstream not in elements:
                        raise SystemExit(f"selector {name} names the unknown input {upstream}")
                    if upstream in self.inputs[name] or label in self.inputs[name].values():
                        raise SystemExit(f"selector {name} repeats an input")
                    self.inputs[name][upstream] = label
                    cited = self.cite(f"clockin-{name}-{upstream}", source, entry.start, entry.end,
                                      f"Clock descriptor lists {upstream} as the input labelled {label} of selector {name}.")
                    edges[(upstream, name)] = [cited]
                    if name not in elements[upstream].value["outputTargets"]:
                        unlisted.append(f"{upstream}->{name}")
        for name, element in elements.items():
            value = element.value
            targets = element["outputTargets"]
            for downstream in value["outputTargets"]:
                if downstream not in elements:
                    raise SystemExit(f"element {name} names the unknown target {downstream}")
            if not value["outputTargets"]:
                continue
            cited = self.cite(f"clockout-{name}", source, targets.start, targets.end,
                              f"Clock descriptor element {name} lists as its output targets: {', '.join(value['outputTargets'])}.")
            for downstream in value["outputTargets"]:
                if elements[downstream].value["type"] == "multiplexor":
                    if name not in self.inputs[downstream]:
                        absent.append(f"{name}->{downstream}")
                        continue
                    edges[(name, downstream)].append(cited)
                else:
                    self.inputs[downstream][name] = None
                    edges[(name, downstream)] = [cited]
        drawn_only = []
        for transition in root["tree"]["transitions"]:
            upstream = self.endpoint(transition.value["sourceTaskId"])
            downstream = self.endpoint(transition.value["targetTaskId"])
            if upstream not in elements or downstream not in elements:
                raise SystemExit(f"clock descriptor connection {transition.value['id']} names an unknown element")
            cited = self.cite(f"clockline-{upstream}-{downstream}", source, transition.start, transition.end,
                              f"Clock descriptor draws a connection from {upstream} to {downstream}.")
            if (upstream, downstream) in edges:
                edges[(upstream, downstream)].append(cited)
            elif elements[downstream].value["type"] == "multiplexor":
                raise SystemExit(f"clock descriptor draws {upstream} into selector {downstream}, which does not list it as an input")
            else:
                self.inputs[downstream][upstream] = None
                edges[(upstream, downstream)] = [cited]
                drawn_only.append(f"{upstream}->{downstream}")
        nodes, repeated = {}, []
        for name, element in elements.items():
            value = element.value
            identifier = f"{self.prefix}.clock.{name}"
            attributes = {"element": name, "element_type": value["type"]}
            for key, field in (("role", "role"), ("block", "block"), ("output_clock", "outputClockId")):
                if value.get(field):
                    attributes[key] = value[field]
            if value["type"] in self.config["fixed_source_types"]:
                attributes["value_hz"] = number(value["value"])
            elif value["type"] in self.config["source_types"] and "default" in value and not isinstance(value["default"], str):
                attributes["descriptor_default_hz"] = number(value["default"])
            for key in ("min", "max"):
                if key in value and not isinstance(value[key], str) and value["type"] != "divider":
                    attributes[f"{key}_hz"] = number(value[key])
            if value["type"] == "multiplexor":
                attributes["inputs"] = joined(identifier, "inputs", self.inputs[name].values())
                attributes["input_elements"] = joined(identifier, "input_elements", self.inputs[name].keys())
                detail = f" with the inputs {', '.join(self.inputs[name].values())}"
            elif "oneOf" in value:
                listed = [number(choice["const"]) for choice in value["oneOf"]]
                choices = list(dict.fromkeys(listed))
                if len(choices) != len(listed):
                    repeated.append(name)
                steps = {fractions.Fraction(after) - fractions.Fraction(before) for before, after in zip(choices, choices[1:])}
                if len(",".join(choices)) <= ATTRIBUTE_BYTES:
                    attributes["choices"] = joined(identifier, "choices", choices)
                    detail = f" with the choices {', '.join(choices)}"
                elif len(steps) == 1 and next(iter(steps)) > 0:
                    # Too many for one attribute and evenly spaced: first, last and step say the same.
                    attributes["choice_first"], attributes["choice_last"] = choices[0], choices[-1]
                    attributes["choice_step"] = number(str(next(iter(steps))))
                    detail = f" with {len(choices)} distinct choices from {choices[0]} to {choices[-1]} in steps of {attributes['choice_step']}"
                else:
                    raise SystemExit(f"{identifier}: its choices fit neither one attribute nor a first, last and step")
            else:
                detail = ""
            if value.get("clocked_instance"):
                attributes["clocks"] = joined(identifier, "clocks", value["clocked_instance"])
                if len(value["clocked_instance"]) > 1 and value["type"] in self.config["shared_types"]:
                    attributes["shared_by"] = attributes["clocks"]
                detail = f" names {', '.join(value['clocked_instance'])} as clocked by it" + (" and" + detail if detail else "")
            if name in repeated:
                detail += f"; the descriptor lists {len(value['oneOf'])} entries and one value more than once"
            statement = f"Clock descriptor element {name} of type {value['type']}{detail}. What can be configured, not the captured configuration."
            spans = self.spans(source, element.start, element.end)
            if len(spans) == 1:
                evidence = [self.cite(f"clock-{name}", source, element.start, element.end, statement), policy]
            else:
                # Longer than one citation may be: the element is cited whole,
                # in consecutive spans.
                evidence = []
                for index, (first, last) in enumerate(spans):
                    part = (f"Part {index + 1} of {len(spans)} of clock descriptor element {name}, which is longer than one citation may be; "
                            "the parts follow one another without a gap.")
                    evidence.append(self.cite(f"clock-{name}" if index == 0 else f"clock-{name}-part{index + 1}", source, first, last,
                                              statement + " " + part if index == 0 else part))
                evidence.append(policy)
            self.element_cited[name] = evidence[0]
            if "shared_by" in attributes:
                evidence.append(self.policy("shared_clocks"))
            self.add(identifier, spec["kind"], attributes, evidence)
            nodes[name] = identifier
        self.nodes = nodes
        for (upstream, downstream), evidence in sorted(edges.items()):
            label = self.inputs[downstream][upstream]
            self.link(f"clock.{upstream}.{downstream}", nodes[upstream], nodes[downstream], spec["relation"],
                      [*evidence, policy], {"input": label} if label else None)
        named = self.instances()
        linked = 0
        instance_policy = self.policy("instance_links")
        for name, element in elements.items():
            for instance in element.value.get("clocked_instance", []):
                for record in named.get(instance, []):
                    self.link(f"clock.{name}.clocks.{record['id'].split('.', 1)[1]}", nodes[name], record["id"], spec["clocks_relation"],
                              [self.element_cited[name], instance_policy])
                    linked += 1
        self.summary["clock_tree"] = {
            "elements": len(elements),
            "by_type": {kind: sum(1 for element in elements.values() if element.value["type"] == kind)
                        for kind in sorted({element.value["type"] for element in elements.values()})},
            "inputs": len(edges),
            "instance_links": linked,
            "shared": sorted(name for name, element in elements.items()
                             if len(element.value.get("clocked_instance", [])) > 1 and element.value["type"] in self.config["shared_types"]),
            "connections_only_drawn": drawn_only,
            "elements_that_repeat_a_choice": repeated,
            "targets_a_selector_does_not_list": absent,
            "selector_inputs_the_source_does_not_list": unlisted,
        }

    @staticmethod
    def endpoint(task):
        """The element a drawn connection ends on. A selector's input pin is
        written SELECTOR__INPUT_input; every other end ELEMENT_input or
        ELEMENT_output."""
        if "__" in task:
            return task.split("__", 1)[0]
        return re.sub(r"_(?:input|output)$", "", task)

    # What the SDK declares.

    def headers(self):
        spec = self.config["sdk"]
        defined = macros(self.source(spec["device_header"]))
        self.defined_evidence = {}
        for entry in spec.get("defined", []):
            source = self.source(entry["source"])
            line = source.find(r"^\s*#\s*define\s+" + re.escape(entry["macro"]) + r"\b")
            defined.add(entry["macro"])
            self.defined_evidence[entry["macro"]] = self.cite(
                f"rcc-macro-{entry['macro']}", source, line, line,
                f"{entry['label']} defines {entry['macro']} on this line.")
        undetermined = set(spec.get("undetermined", []))
        if undetermined & defined:
            raise SystemExit("a macro is both defined and listed as undetermined")
        settled = {}
        for entry in spec.get("settled", []):
            # A test of more than one macro is settled by the configuration
            # from one cited definition, whose value is checked here.
            source = self.source(entry["source"])
            line = source.find(r"^\s*#\s*define\s+" + re.escape(entry["macro"]) + r"\b")
            stated = re.match(r"^\s*#\s*define\s+\w+\s+(\S+)", source.lines[line - 1])
            if not stated or stated.group(1) != entry["value"]:
                raise SystemExit(f"{entry['macro']} is not defined as {entry['value']}")
            settled[entry["test"]] = entry["verdict"]
            self.defined_evidence[entry["macro"]] = self.cite(
                f"rcc-macro-{entry['macro']}", source, line, line,
                f"{entry['label']} defines {entry['macro']} as {entry['value']} on this line.")
        self.defined = defined
        self.header = self.source(spec["header"])
        self.body = self.source(spec["definitions"])
        self.bus = self.source(spec["bus_header"])
        self.kept = {source.id: Conditional(source, defined, undetermined, settled) for source in (self.header, self.body, self.bus)}
        for identifier in self.defined_evidence.values():
            if identifier not in self.target["evidence"]:
                self.target["evidence"].append(identifier)
        self.functions = {}
        undecided = []
        for index, line in enumerate(self.header.lines):
            match = FUNCTION.match(line)
            if not match or not match.group(1).startswith(spec["function_prefix"]):
                continue
            verdict = self.kept[self.header.id].kept(index + 1)
            if verdict is None:
                undecided.append(match.group(1))
            elif verdict:
                if match.group(1) in self.functions:
                    raise SystemExit(f"{self.header.id} declares {match.group(1)} twice for this part")
                self.functions[match.group(1)] = index + 1
        self.summary["sdk"] = {"functions_compiled_for_the_part": len(self.functions),
                               "functions_under_an_undecided_condition": sorted(undecided)}

    def function(self, name):
        """Cite a function the header declares for this part: at its
        definition where the driver source holds one, else at the header."""
        if name not in self.functions:
            return None
        pattern = re.compile(rf"^(?!\s)[\w \t\*]*\b{re.escape(name)}\s*\(")
        lines = [index + 1 for index, line in enumerate(self.body.lines) if pattern.match(line)]
        if len(lines) == 1:
            if self.kept[self.body.id].kept(lines[0]) is not True:
                raise SystemExit(f"{name} is declared for this part and its definition is not compiled")
            return self.cite_symbol(self.body, name)
        if lines:
            raise SystemExit(f"{self.body.id} defines {name} more than once")
        return self.cite_symbol(self.header, name)

    def mapping(self):
        """Elements of the manufacturer's configuration mapping, by name. A
        name the mapping uses twice is left out: it does not say which."""
        spec = self.config.get("mapping")
        self.mapped = {}
        if not spec:
            return
        source = self.source(spec["source"])
        seen = {}
        for element in descriptor.parse(source)["configurable_elements"]:
            seen.setdefault(element.value["name"], []).append(element)
        self.mapping_source = source
        self.mapped = {name: found[0] for name, found in seen.items() if len(found) == 1}
        self.summary["mapping"] = {"elements": sum(len(found) for found in seen.values()),
                                   "names_used_more_than_once": sorted(name for name, found in seen.items() if len(found) > 1)}

    def gates(self):
        spec = self.config["gates"]
        enable = re.compile(re.escape(spec["functions"]["enable_api"]).replace(r"\{name\}", "(?P<name>[A-Z0-9]+)"))
        names = sorted(match.group("name") for match in map(enable.fullmatch, self.functions) if match)
        policy, instance_policy = self.policy("peripheral_clocks"), self.policy("instance_links")
        call = re.compile(r"\b(LL_([A-Z0-9]+)_GRP(\d+))_EnableClock\((LL_[A-Z0-9]+_GRP\d+_PERIPH_[A-Z0-9_]+)\)")
        named = self.instances()
        pointers = self.config.get("mapping", {}).get("withheld_chapters", {})
        counts = {key: 0 for key in spec["functions"]}
        without_bus, links, coupled, chapters = [], 0, {}, 0
        self.gate_ids = {}
        self.element_gates = {}
        by_api = {}
        for record in self.value["resources"]:
            api = record["attributes"].get(spec["resource_attribute"])
            if api:
                by_api.setdefault(api, []).append(record)
        for name in names:
            identifier = f"{self.prefix}.rcc.{name}"
            attributes, evidence = {"name": name}, [policy]
            for key, template in spec["functions"].items():
                function = template.format(name=name)
                cited = self.function(function)
                if cited:
                    attributes[key] = function
                    evidence.append(cited)
                    counts[key] += 1
                    if key == "enable_api":
                        enabled = cited
            enable_function = attributes["enable_api"]
            start, end, _ = inventory.declaration(self.header, enable_function)
            calls = call.findall("\n".join(self.header.lines[start - 1:end]))
            if len(calls) == 1:
                group, bus, _, symbol = calls[0]
                attributes["bus"] = bus
                attributes["bus_group"] = group
                line, _, _ = inventory.declaration(self.bus, symbol)
                if self.kept[self.bus.id].kept(line) is not True:
                    raise SystemExit(f"{symbol} is used for this part and its definition is not compiled")
                field = re.match(r"^\s*#\s*define\s+" + re.escape(symbol) + r"\s+(\w+)\s*(?:/\*.*)?$", self.bus.lines[line - 1])
                if not field:
                    raise SystemExit(f"{self.bus.id}: {symbol} is not defined as one field")
                attributes["enable_field"] = field.group(1)
                evidence.append(self.cite(f"rcc-{symbol}", self.bus, line, line,
                                          f"LL bus header defines {symbol} as {field.group(1)} on this line."))
            else:
                without_bus.append(name)
            element = self.mapped.get(name)
            members = []
            if element is not None:
                cited = self.cite(f"mapping-{name}", self.mapping_source, element.start, element.end,
                                  f"Configuration mapping element {name}: the manufacturer's list of configurable elements, with the reference manual chapter it names. A pointer and a grouping, not a statement about clocks.")
                chapter = element.value.get("docLink", {})
                if chapter.get("Chapter") and name not in pointers:
                    attributes["manual"] = f"{chapter.get('RM', '')} chapter {chapter['Chapter']}".strip()
                    evidence.extend([cited, self.policy("chapter_pointers")])
                    chapters += 1
                members = [member["name"] for member in element.value.get("configurableElements", [])
                           if member["name"] in self.defined]
                if members:
                    coupled[name] = members
                    if cited not in evidence:
                        evidence.append(cited)
            targets = {}
            for record in by_api.get(enable_function, []):
                targets[record["id"]] = [instance_policy]
            for record in named.get(name, []):
                if record["kind"] == spec["unpromoted_kind"]:
                    targets.setdefault(record["id"], [instance_policy])
            for member in members:
                for record in named.get(member, []):
                    targets.setdefault(record["id"], [instance_policy]).append(self.located[(self.mapping_source.id, element.start, element.end)])
            for entry in spec.get("stated_links", {}).get(name, []):
                # The manufacturer's description of the enable field names
                # the instances the gate serves.
                if "enable_field" not in attributes or attributes["enable_field"].rsplit("_", 1)[1] not in entry["pattern"]:
                    raise SystemExit(f"{name}: the stated link does not cite the record's enable field")
                cited = self.cite_line("rcc-field", entry["source"], entry["pattern"], entry["label"])
                evidence.append(cited)
                for member in entry["instances"]:
                    found = named.get(member, [])
                    if not found and not self.config.get("composed"):
                        raise SystemExit(f"{name}: no resource stands for the instance {member}")
                    for record in found:
                        targets.setdefault(record["id"], [instance_policy]).append(cited)
            gated = sorted({record["attributes"]["instance"] for record in self.value["resources"]
                            if record["id"] in targets and "instance" in record["attributes"]})
            if gated:
                attributes["instances"] = joined(identifier, "instances", gated)
            # The descriptor elements that name the record or an instance it
            # serves: where to read what clocks it and at which frequency.
            served = {name, *gated}
            fed = [element for element, located in self.elements.items()
                   if served & set(located.value.get("clocked_instance", []))]
            if fed:
                attributes["clock_elements"] = joined(identifier, "clock_elements", fed)
                evidence.extend(self.element_cited[element] for element in fed)
                if instance_policy not in evidence:
                    evidence.append(instance_policy)
            self.add(identifier, spec["kind"], attributes, evidence)
            self.gate_ids[name] = identifier
            for element in fed:
                self.element_gates.setdefault(element, []).append(name)
            for target, cited in sorted(targets.items()):
                self.link(f"rcc.{name}.gates.{target.split('.', 1)[1]}", identifier, target, spec["relation"],
                          [enabled, *cited])
                links += 1
        unknown = sorted(set(spec.get("stated_links", {})) - set(names))
        if unknown:
            raise SystemExit(f"stated links name peripheral clocks the SDK does not declare for this part: {unknown}")
        self.summary["peripheral_clocks"] = {"names": len(names), **counts, "with_bus_and_field": len(names) - len(without_bus),
                                             "without_one_bus_call": without_bus, "links": links,
                                             "coupled_by_the_mapping": coupled, "chapter_pointers": chapters,
                                             "chapter_pointers_withheld": sorted(set(pointers) & set(names))}

    def kernel_clocks(self):
        spec = self.config["kernel_clocks"]
        setter = re.compile(re.escape(spec["functions"]["set_source_api"]).replace(r"\{name\}", "(?P<name>[A-Z0-9]+)"))
        names = sorted(match.group("name") for match in map(setter.fullmatch, self.functions) if match)
        missing = [name for name in names if name not in spec["selectors"]]
        if missing or set(spec["selectors"]) - set(names):
            raise SystemExit(f"kernel clocks the configuration does not pair with a descriptor selector: {missing}; "
                             f"names it pairs that the SDK does not declare for this part: {sorted(set(spec['selectors']) - set(names))}")
        policy = self.policy("kernel_clocks")
        paired, unpaired, shared = {}, {}, {}
        self.kernel = {}
        for name in names:
            identifier = f"{self.prefix}.rcc.kernel.{name}"
            attributes, evidence = {"name": name}, [policy]
            for key, template in spec["functions"].items():
                function = template.format(name=name)
                cited = self.function(function)
                if cited:
                    attributes[key] = function
                    evidence.append(cited)
            line = self.functions[attributes["set_source_api"]]
            kind = re.search(r"\(\s*(\w+_t)\s+\w+\s*\)", self.header.lines[line - 1])
            if not kind:
                raise SystemExit(f"{attributes['set_source_api']}: no one typed argument")
            closing = self.header.find(r"^\}\s*" + re.escape(kind.group(1)) + r"\s*;")
            if self.kept[self.header.id].kept(closing) is not True:
                raise SystemExit(f"{kind.group(1)} is not compiled for this part")
            opening = closing
            while opening > 1 and not re.match(r"^\s*typedef\s+enum\b", self.header.lines[opening - 1]):
                opening -= 1
            start = opening
            if self.header.lines[opening - 2].strip() == "*/":
                start = opening - 1
                while start > 1 and not self.header.lines[start - 1].lstrip().startswith("/*"):
                    start -= 1
            enumerators = [match.group(1) for match in (re.match(r"^\s*([A-Za-z_]\w*)\s*=", text)
                                                        for text in self.header.lines[opening:closing - 1]) if match]
            if not enumerators:
                raise SystemExit(f"{kind.group(1)} has no enumerators")
            attributes["source_type"] = kind.group(1)
            attributes["sources"] = joined(identifier, "sources", enumerators)
            evidence.append(self.cite(f"rcc-{kind.group(1)}", self.header, start, closing,
                                      f"SDK header declares {kind.group(1)} with the enumerators {', '.join(enumerators)}."))
            pair = spec["selectors"][name]
            if pair.get("element") is None:
                unpaired[name] = pair["reason"]
            else:
                element = self.elements[pair["element"]]
                if element.value["type"] != "multiplexor":
                    raise SystemExit(f"{pair['element']} is not a selector")
                labels, none = {}, []
                for enumerator in enumerators:
                    if not enumerator.startswith(pair["prefix"]):
                        raise SystemExit(f"{enumerator} does not begin with {pair['prefix']}")
                    suffix = enumerator[len(pair["prefix"]):]
                    if suffix in pair.get("without_input", []):
                        # An enumerator the configuration says selects no input.
                        none.append(enumerator)
                        continue
                    labels[enumerator] = pair.get("labels", {}).get(suffix, suffix)
                if sorted(enumerator[len(pair["prefix"]):] for enumerator in none) != sorted(pair.get("without_input", [])):
                    raise SystemExit(f"kernel clock {name}: the configuration names an enumerator without input that the SDK does not declare")
                if none:
                    attributes["sources_without_input"] = joined(identifier, "sources_without_input", none)
                offered = list(self.inputs[pair["element"]].values())
                if sorted(labels.values()) != sorted(offered) or len(set(labels.values())) != len(labels):
                    raise SystemExit(f"kernel clock {name}: the SDK offers {sorted(labels.values())} and descriptor selector "
                                     f"{pair['element']} offers {sorted(offered)}")
                attributes["selector"] = pair["element"]
                attributes["inputs"] = joined(identifier, "inputs", [labels[enumerator] for enumerator in enumerators if enumerator in labels])
                clocked = element.value.get("clocked_instance", [])
                if clocked:
                    attributes["clocks"] = joined(identifier, "clocks", clocked)
                if len(clocked) > 1:
                    attributes["shared_by"] = attributes["clocks"]
                    evidence.append(self.policy("shared_clocks"))
                    shared[name] = clocked
                evidence.append(self.element_cited[pair["element"]])
                paired[name] = {"selector": pair["element"], "labels": labels}
            self.add(identifier, spec["kind"], attributes, evidence)
            if name in paired:
                self.link(f"rcc.kernel.{name}.selects.{pair['element']}", identifier, self.nodes[pair["element"]], spec["relation"],
                          [evidence[-1], policy])
            self.kernel[name] = paired.get(name)
        used = {entry["selector"] for entry in paired.values()}
        without = sorted(name for name, element in self.elements.items()
                         if element.value["type"] == "multiplexor" and name not in used)
        declared = self.config["kernel_clocks"].get("selectors_without_setter", {})
        if sorted(declared) != without:
            raise SystemExit(f"selectors no kernel clock setter is paired with: {without}; the configuration names {sorted(declared)}")
        self.summary["kernel_clocks"] = {"names": len(names), "paired_with_a_selector": len(paired), "shared": shared,
                                         "not_paired": unpaired, "selectors_without_setter": declared}

    # The profile.

    def makers(self, record, attribute, setter):
        """The lines that make the selection a resource's attribute names, and
        the capabilities whose unit makes it. A component makes it when its
        template calls the setter binding with the source binding, both read
        from a role this resource can fill. A captured initialization makes
        it when the line the configuration names calls the paired setter with
        the attribute's value; it does so for every capability that lists the
        card which calls that initialization."""
        evidence = {entry["id"]: entry for entry in self.value["evidence"]}
        cards = {card["id"]: card for card in self.value["implementations"]}
        api = self.config["profile"]["component_api_attribute"]
        lines, capabilities = [], []
        for capability in self.value["capabilities"]:
            roles = {role["id"]: role for role in capability["roles"]}
            for identifier in capability.get("implementations", []):
                component = cards[identifier].get("component")
                for name, binding in (component or {}).get("bindings", {}).items():
                    source = binding["source"]
                    role = roles.get(source.get("role"))
                    if source["type"] != "resource_attribute" or source["attribute"] != attribute or role is None:
                        continue
                    if role["kind"] != record["kind"] or any(record["attributes"].get(key) != text for key, text in role["attributes"].items()):
                        continue
                    callers = [other for other, bound in component["bindings"].items()
                               if bound["source"] == {**source, "attribute": api}]
                    call = re.compile("|".join(r"(?<![A-Za-z0-9_}])\{\{" + re.escape(caller) + r"\}\}\s*\(\s*\{\{" + re.escape(name) + r"\}\}\s*\)"
                                               for caller in callers)) if callers else None
                    found = []
                    for entry in component["files"]:
                        template = evidence[entry["template"]]
                        text = self.source(template["source"])
                        for line in range(template["start_line"], template["end_line"] + 1):
                            if call and self.called(call, self.code_of(text)[line - 1]):
                                found.append(self.cite(
                                    f"component-{template['source']}-{line}", text, line, line,
                                    f"Component template of card {identifier}: this line of the unit's initialization calls the "
                                    f"{api} of the selected resource with its {attribute}. Authored implementation, not a manufacturer statement."))
                    if not found:
                        raise SystemExit(f"{identifier} reads {attribute} and no template line of it calls the {api} with it")
                    lines.extend(found)
                    capabilities.append(capability["id"])
        for entry in self.config["profile"].get("captured_component_selections", []):
            if entry["value"] == record["attributes"][attribute]:
                using = [capability["id"] for capability in self.value["capabilities"]
                         if entry["card"] in capability.get("implementations", [])]
                if not using:
                    raise SystemExit(f"no capability lists the card {entry['card']}")
                if not entry.get("cite"):
                    raise SystemExit(f"the captured selection of {entry['value']} cites no line that makes it")
                call = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(setter) + r"\s*\(\s*" + re.escape(entry["value"]) + r"\s*\)")
                for source_id, pattern, label in entry["cite"]:
                    text = self.source(source_id)
                    if not self.called(call, self.code_of(text)[text.find(pattern) - 1]):
                        raise SystemExit(f"the captured line cited for {entry['value']} does not call {setter} with it")
                    lines.append(self.cite_line("profile", source_id, pattern, label))
                capabilities.extend(using)
        return list(dict.fromkeys(lines)), sorted(set(capabilities))

    def profile(self):
        spec = self.config["profile"]
        if self.target["context"].get("clock_profile") != spec["id"]:
            raise SystemExit("the configured profile is not the one the target's context names")
        policy = self.policy("profile")
        selected, cited, basis, symbol, holds = {}, {}, {}, {}, {}

        def select(entry, kind):
            name = entry["element"]
            if name in selected or name not in self.elements:
                raise SystemExit(f"profile selection of {name} is repeated or names no element")
            value = self.elements[name].value
            if value["type"] == "multiplexor":
                if entry["input"] not in self.inputs[name].values():
                    raise SystemExit(f"selector {name} has no input {entry['input']}")
                selected[name] = entry["input"]
            else:
                choices = [number(choice["const"]) for choice in value.get("oneOf", [])]
                text = number(entry["value"])
                if choices and text not in choices:
                    raise SystemExit(f"{name} has no choice {text}")
                for key, low in (("min", True), ("max", False)):
                    if key in value and not isinstance(value[key], str) and not choices:
                        limit = fractions.Fraction(str(value[key]))
                        if (fractions.Fraction(text) < limit) if low else (fractions.Fraction(text) > limit):
                            raise SystemExit(f"{name}: {text} is outside the descriptor's range")
                selected[name] = text
            cited[name] = []
            if "symbol" in entry:
                # The symbol has to say the selection: for a selector it is the
                # enumerator the kernel clock pairing gives the selected input,
                # or it ends in that input; for anything else it ends in the number.
                paired = [pair for pair in self.kernel.values() if pair and pair["selector"] == name]
                if value["type"] == "multiplexor":
                    says = paired[0]["labels"].get(entry["symbol"]) if paired else entry["symbol"].rsplit("_SRC_", 1)[-1]
                    agrees = says == selected[name]
                else:
                    ending = re.search(r"(\d+)(MHZ|KHZ)?$", entry["symbol"])
                    agrees = bool(ending) and fractions.Fraction(ending.group(1)) * {"MHZ": 1000000, "KHZ": 1000, None: 1}[ending.group(2)] \
                        == fractions.Fraction(selected[name])
                if not agrees:
                    raise SystemExit(f"the selection of {name} is {selected[name]} and its symbol {entry['symbol']} says something else")
            if not entry.get("cite"):
                raise SystemExit(f"the selection of {name} cites no line that makes it")
            if "field" not in entry:
                raise SystemExit(f"the selection of {name} cites lines and does not name the field they set")

            if kind == "component" and entry["field"] not in self.functions:
                raise SystemExit(f"the component selection of {name} names the field {entry['field']}, "
                                 "which is no function the SDK header declares for this part")

            def whole(token):
                """A number, with a C integer suffix, is not found inside another number or a name; a name not inside another name."""
                if re.fullmatch(r"[0-9.]+", token):
                    return r"(?<![A-Za-z0-9_.])" + re.escape(token) + r"(?![0-9.])(?:[uU](?:ll?|LL?)?|(?:ll?|LL?)[uU]?)?(?![A-Za-z0-9_])"
                return r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])"

            for source_id, pattern, label, *following in entry["cite"]:
                # A citing line makes the selection. What code_of leaves of
                # it has one of three forms, where the value is the
                # selection's symbol, or its number where it has no symbol: a
                # statement that fills the line and assigns the value alone,
                # without a cast, to the field; a call of the field with the
                # value as one whole argument, on a line that is no
                # preprocessor directive; a #define of the field as the value
                # alone. The check is textual: it guards against a citation
                # that says something else than the configuration.
                text = self.source(source_id)
                line = self.code_of(text)[text.find(pattern) - 1]
                wanted = entry["symbol"] if "symbol" in entry else selected[name]
                field, made = whole(entry["field"]), whole(wanted)
                if re.match(r"^\s*#", line):
                    forms = (r"^\s*#\s*define\s+" + field + r"\s+" + made + r"\s*$",)
                else:
                    # Every argument of the call is one plain name or number, so that the value is a whole argument.
                    suffix = r"(?:[uU](?:ll?|LL?)?|(?:ll?|LL?)[uU]?)?"
                    plain = r"(?:[A-Za-z_][A-Za-z0-9_]*|(?:0[xX][0-9a-fA-F]+|[0-9]+(?:\.[0-9]+)?)" + suffix + r")"
                    forms = (r"^\s*(?:[A-Za-z_][A-Za-z0-9_]*\s*(?:\.|->)\s*)*" + field + r"\s*=(?!=)\s*" + made + r"\s*;\s*$",
                             r"(?<![.>])" + field + r"\s*\(\s*(?:" + plain + r"\s*,\s*)*" + made + r"\s*(?:,\s*" + plain + r"\s*)*\)")
                # The field stands on the line once: a second use of it would be a second statement or call.
                if len(re.findall(field, line)) != 1 or not any(re.search(form, line) for form in forms):
                    raise SystemExit(f"the selection of {name} is not what its cited line states: "
                                     f"the line does not set {entry['field']} to {wanted}")
                cited[name].append(self.cite_line("profile", source_id, pattern, label, *following))
            for source_id, pattern, label in entry.get("support", []):
                cited[name].append(self.cite_line("profile", source_id, pattern, label))
            for identifier in entry.get("evidence", []):
                find(self.value["evidence"], identifier)
                cited[name].append(identifier)
            basis[name] = kind
            if "symbol" in entry:
                symbol[name] = entry["symbol"]
                cited[name].append(self.cite_symbol(self.header, entry["symbol"]))
            if kind == "component":
                # A selection the configuration states for a component names
                # the capability that makes it and the instance it is made for.
                if "capability" not in entry or "instance" not in entry:
                    raise SystemExit(f"the component selection of {name} names no capability or no instance")
                capability = find(self.value["capabilities"], entry["capability"])
                fills = [record for record in self.value["resources"]
                         if record["attributes"].get("instance") == entry["instance"]
                         and any(role["kind"] == record["kind"]
                                 and all(record["attributes"].get(key) == text for key, text in role["attributes"].items())
                                 for role in capability["roles"])]
                if not fills:
                    raise SystemExit(f"the component selection of {name}: no role of {entry['capability']} takes the instance {entry['instance']}")
                holds[name] = self.held(entry["instance"], [entry["capability"]])

        for entry in spec["selections"]:
            select(entry, entry.get("basis", "captured"))
        # A kernel clock selection a reviewed resource already carries.
        attribute = spec["component_attribute"]
        component_policy = self.policy("component_selections")
        for record in self.value["resources"]:
            enumerator = record["attributes"].get(attribute)
            if not enumerator:
                continue
            found = [(name, pair) for name, pair in self.kernel.items() if pair and enumerator in pair["labels"]]
            if len(found) != 1:
                raise SystemExit(f"{record['id']}: {enumerator} is not an enumerator of one paired kernel clock")
            name, pair = found[0]
            if pair["selector"] in selected:
                raise SystemExit(f"{pair['selector']} is selected twice")
            setter = find(self.value["resources"], f"{self.prefix}.rcc.kernel.{name}")["attributes"]["set_source_api"]
            lines, capabilities = self.makers(record, attribute, setter)
            if not lines:
                raise SystemExit(f"{record['id']} names {enumerator} and no component template or captured line makes that selection")
            selected[pair["selector"]] = pair["labels"][enumerator]
            cited[pair["selector"]] = [self.cite_symbol(self.header, enumerator), *lines, component_policy]
            basis[pair["selector"]] = "component"
            symbol[pair["selector"]] = enumerator
            holds[pair["selector"]] = self.held(record["attributes"].get("instance", record["id"]), capabilities)
        frequency, rule, through = {}, {}, {}

        def derive(name, trail=()):
            if name in frequency:
                return frequency[name]
            if name in trail:
                raise SystemExit(f"the clock descriptor loops through {name}")
            value = self.elements[name].value
            kind = value["type"]
            result = None
            if kind in self.config["source_types"]:
                reference = [derive(upstream, (*trail, name)) for upstream in self.inputs[name]]
                if any(above is None for above in reference):
                    pass
                elif name in selected:
                    result, rule[name] = fractions.Fraction(selected[name]), "the selected value"
                    if reference:
                        rule[name] += f", with element {next(iter(self.inputs[name]))} as its reference"
                        through[name] = next(iter(self.inputs[name]))
                elif kind in self.config["fixed_source_types"]:
                    result, rule[name] = fractions.Fraction(str(value["value"])), "the one value the descriptor states"
                    basis.setdefault(name, "fixed")
                elif len(value.get("oneOf", [])) == 1:
                    result, rule[name] = fractions.Fraction(number(value["oneOf"][0]["const"])), "the one choice the descriptor gives"
                    basis.setdefault(name, "fixed")
                    if reference:
                        through[name] = next(iter(self.inputs[name]))
            elif kind == "multiplexor":
                labels = list(self.inputs[name].values())
                choice = selected.get(name) or (labels[0] if len(labels) == 1 else None)
                if choice is not None:
                    upstream = next(key for key, label in self.inputs[name].items() if label == choice)
                    result = derive(upstream, (*trail, name))
                    rule[name] = (f"its selected input {choice}" if name in selected else f"its one input {choice}") + f", element {upstream}"
                    through[name] = upstream
                    basis.setdefault(name, "fixed")
            elif kind == "divider":
                upstream = list(self.inputs[name])
                if len(upstream) != 1:
                    raise SystemExit(f"divider {name} has {len(upstream)} inputs")
                choices = [number(choice["const"]) for choice in value.get("oneOf", [])]
                factor = selected.get(name) or (choices[0] if len(choices) == 1 else None)
                above = derive(upstream[0], (*trail, name))
                if factor is not None and above is not None:
                    result = above / fractions.Fraction(factor)
                    rule[name] = f"element {upstream[0]} divided by {factor}"
                    basis.setdefault(name, "fixed")
                    through[name] = upstream[0]
            else:
                upstream = list(self.inputs[name])
                if len(upstream) == 1:
                    result = derive(upstream[0], (*trail, name))
                    rule[name] = f"its one input, element {upstream[0]}"
                    through[name] = upstream[0]
                elif len(upstream) > 1:
                    raise SystemExit(f"element {name} of type {kind} has several inputs")
            if result is not None:
                for key in ("max",):
                    if key in value and not isinstance(value[key], str) and kind != "divider" and result > fractions.Fraction(str(value[key])):
                        raise SystemExit(f"{name}: {result} Hz is above the descriptor's limit {value[key]}")
                basis.setdefault(name, "derived")
            frequency[name] = result
            return result

        for name in self.elements:
            derive(name)

        def chain(name):
            """An element and every element its frequency is derived through."""
            found, cursor = [], name
            while cursor is not None:
                found.append(cursor)
                cursor = through.get(cursor)
            return found

        def condition(name):
            """The case an assignment is stated for: its own and that of
            every clock it is derived through."""
            found = []
            for element in chain(name):
                if element in holds and holds[element] not in found:
                    found.append(holds[element])
            return found

        system = spec["system_clock"]
        if frequency.get(system) is None:
            raise SystemExit(f"the profile does not determine the system clock {system}")
        feeding = set(chain(system))

        def running(name):
            """How far the captured configuration runs a clock. `captured`: it
            feeds the system clock, or it is derived from the system clock,
            rests on no component's selection and names no instance that has
            a peripheral clock record. `gated`: any other clock derived from
            the system clock; the peripheral clocks of the instances it names
            are listed, and no assignment says whether one is enabled.
            `not_stated`: nothing captured switches its source on."""
            if name in feeding:
                return "captured"
            if system not in chain(name):
                return "not_stated"
            return "gated" if condition(name) or self.element_gates.get(name) else "captured"
        self.selected = selected
        for name in selected:
            if frequency.get(name) is None:
                raise SystemExit(f"the selection of {name} determines no frequency: an input of it is not selected")
        for name, expected in spec.get("expect", {}).items():
            line = self.cite_line("profile", expected["source"], expected["pattern"], expected["label"])
            # The value the captured line states: the last number in the text the pattern matches.
            text = self.source(expected["source"])
            matched = re.search(expected["pattern"], text.lines[text.find(expected["pattern"]) - 1])
            stated = re.findall(r"\d+", matched.group(0))
            if frequency.get(name) is None or not stated or fractions.Fraction(stated[-1]) != frequency[name]:
                raise SystemExit(f"{name}: derived {frequency.get(name)} and the captured configuration states otherwise")
            cited.setdefault(name, []).append(line)
        source = self.source(self.config["descriptor"]["source"])
        conditions = {**self.conditions, "clock_profile": spec["id"]}
        self.assigned, self.assignment_evidence = {}, {}
        for name, element in self.elements.items():
            if frequency.get(name) is None:
                continue
            hertz = number(str(frequency[name]) if frequency[name].denominator == 1 else float(frequency[name]))
            identifier = f"{self.prefix}.clock-profile.{spec['id']}.{name}"
            attributes = {"profile": spec["id"], "element": name, "frequency_hz": hertz, "basis": basis[name]}
            if name in selected:
                attributes["selection"] = selected[name]
            if name in symbol:
                attributes["sdk_symbol"] = symbol[name]
            conditional = condition(name)
            if conditional:
                attributes["holds_while"] = joined(identifier, "holds_while", ["; ".join(conditional)])
                held = (f" It is stated for the case that {' and '.join(conditional)}: a component's initialization then makes the "
                        "selection it rests on. No assignment states it for another case; a selector's reset selection, which the "
                        "graph does not record, may be the same input.")
            else:
                held = ""
            attributes["running"] = running(name)
            if attributes["running"] == "not_stated":
                held += " Nothing captured switches this clock on: the value is the one it has when it runs."
            gating = None
            if attributes["running"] == "gated":
                held += " The captured configuration runs the system clock it is derived from."
                gates = self.element_gates.get(name, [])
                if gates:
                    attributes["peripheral_clocks"] = joined(identifier, "peripheral_clocks", gates)
                    held += (f" Instances it names have the peripheral clock records {', '.join(gates)}: each peripheral clock is "
                             "enabled by its own enable bit, and no assignment says whether one is enabled.")
                    gating = self.cite_line("profile", *spec["gating_cite"])
            nominal = [element for element in chain(name) if basis.get(element) == "nominal"]
            if nominal:
                attributes["nominal_source"] = nominal[-1]
                held += f" It rests on the nominal value of {nominal[-1]}."
            named = element["id"]
            statement = self.cite(
                f"clockprofile-{name}", source, named.start, named.end,
                f"Under profile {spec['id']} the clock stage assigns {name} the frequency {hertz} Hz: {rule[name]}. "
                f"Derived by arithmetic over the descriptor from the cited selections; nominal, not a measurement.{held} "
                f"The cited line names the element; the element is cited whole under its own identifier.")
            evidence = [statement, self.element_cited[name], policy, *cited.get(name, [])]
            if nominal:
                evidence.append(self.policy("nominal_sources"))
            if conditional and component_policy not in evidence:
                evidence.append(component_policy)
            if gating:
                evidence.append(gating)
            self.add(identifier, spec["kind"], attributes, evidence, conditions)
            self.link(f"clock-profile.{spec['id']}.{name}", identifier, self.nodes[name], spec["relation"], [statement, policy])
            self.assigned[name] = hertz
            self.assignment_evidence[name] = statement
        self.summary["profile"] = {"id": spec["id"], "selections": len(selected), "assigned": len(self.assigned),
                                   "by_basis": {kind: sum(1 for name in self.assigned if basis[name] == kind)
                                                for kind in sorted({basis[name] for name in self.assigned})},
                                   "conditional": sorted(name for name in self.assigned if condition(name)),
                                   "by_running": {state: sorted(name for name in self.assigned if running(name) == state)
                                                  for state in ("captured", "gated", "not_stated")},
                                   "not_determined": sorted(name for name in self.elements if name not in self.assigned),
                                   "frequencies": dict(sorted(self.assigned.items()))}

    def rate_basis(self):
        policy = self.policy("rate_basis")
        checked, absent = {}, []
        lenient = bool(self.config.get("composed"))
        present = {capability["id"] for capability in self.value["capabilities"]}
        for entry in self.config.get("rate_basis", []):
            elements = [name for name in entry["elements"] if name in self.assigned]
            if len(elements) != len(entry["elements"]) and not lenient:
                raise SystemExit(f"rate basis names a clock the profile does not assign: {sorted(set(entry['elements']) - set(elements))}")
            for capability_id in entry["capabilities"]:
                if capability_id not in present or not elements:
                    if not lenient:
                        raise SystemExit(f"rate basis names the unknown capability {capability_id}")
                    absent.append(capability_id)
                    continue
                capability = find(self.value["capabilities"], capability_id)
                added = [self.assignment_evidence[name] for name in elements]
                for identifier in (*added, policy, *[self.policy(name) for name in entry.get("policy", [])]):
                    if identifier not in capability["evidence"]:
                        capability["evidence"].append(identifier)
                if len(capability["evidence"]) > inventory.LIMITS["citations"]:
                    raise SystemExit(f"{capability_id} cites more than {inventory.LIMITS['citations']} sources")
                divides = entry.get("divides")
                if divides:
                    parameter = capability["parameters"][divides]
                    values = parameter["values"] if parameter["type"] == "choice" else [parameter["min"], parameter["max"]]
                    for name in elements:
                        for text in values:
                            if fractions.Fraction(self.assigned[name]) % fractions.Fraction(text) != 0:
                                raise SystemExit(f"{capability_id}: {divides} {text} does not divide {name} at {self.assigned[name]} Hz")
                    checked[capability_id] = f"{len(values)} values of {divides} divide {', '.join(elements)}"
        self.summary["rate_basis"] = {"capabilities": sorted({name for entry in self.config.get("rate_basis", [])
                                                              for name in entry["capabilities"] if name not in absent}),
                                      "exact_division_checked": checked}
        if absent:
            self.summary["rate_basis"]["not_on_this_target"] = sorted(set(absent))

    def statements(self):
        """The service that states the profile, the assumption that says what
        is and is not covered, and the citations of the timer clock rule."""
        spec = self.config["service"]
        properties = {"profile": self.config["profile"]["id"]}
        for key, name in spec["frequencies"].items():
            properties[key] = self.assigned[name]
        for key, text in spec["properties"].items():
            properties[key] = text.replace("{prefix}", self.prefix).replace("{profile}", self.config["profile"]["id"])
        if len(properties) > 16 or any(not text or len(text) > ATTRIBUTE_BYTES for text in properties.values()):
            raise SystemExit("the clock profile service has too many or too long properties")
        evidence = [self.policy("profile"), *[self.assignment_evidence[name] for name in spec["frequencies"].values()]]
        for source_id, pattern, label in spec.get("cite", []):
            evidence.append(self.cite_line("profile", source_id, pattern, label))
        for name in spec.get("symbols", []):
            cited = self.function(name)
            if not cited:
                raise SystemExit(f"the clock profile service names {name}, which is not compiled for this part")
            evidence.append(cited)
        for identifier in spec.get("evidence", []):
            find(self.value["evidence"], identifier)
            evidence.append(identifier)
        service = {"id": spec["id"], "kind": spec["kind"], "description": spec["description"],
                   "properties": properties, "conditions": {**self.conditions, "clock_profile": self.config["profile"]["id"]},
                   "evidence": list(dict.fromkeys(evidence))}
        services = self.target.setdefault("services", [])
        if any(existing["id"] == spec["id"] for existing in services):
            raise SystemExit(f"service {spec['id']} already exists")
        services.append(service)
        timer = self.config["timer_clock"]
        suffix, buses = timer["role_suffix"], set()
        timers = sorted(name for name, located in self.elements.items()
                        if suffix and located.value.get("role", "").endswith(suffix) and located.value["role"] != suffix)
        if sorted(timer["elements"]) != timers:
            raise SystemExit(f"the timer clock rule has to list the timer clock elements of the descriptor, {timers}; "
                             f"it lists {sorted(timer['elements'])}")
        for name in timer["elements"]:
            # A timer clock element carries the role of its bus prescaler with
            # a suffix; the rule has to name exactly those prescalers.
            role = self.elements[name].value.get("role", "")
            if not suffix or not role.endswith(suffix) or role == suffix:
                raise SystemExit(f"timer clock element {name} has no role that ends in {suffix}")
            dividers = [other for other, located in self.elements.items()
                        if located.value["type"] == "divider" and located.value.get("role") == role[:-len(suffix)]]
            if len(dividers) != 1:
                raise SystemExit(f"timer clock element {name}: {len(dividers)} dividers have the role {role[:-len(suffix)]}")
            buses.add(dividers[0])
        if set(timer.get("requires", {})) != buses:
            raise SystemExit(f"the timer clock rule has to name the prescalers of its timer clocks, {sorted(buses)}; "
                             f"it names {sorted(timer.get('requires', {}))}")
        for element, wanted in timer["requires"].items():
            # The rule is stated for these selections only; another profile
            # needs the question of the timer clock settled first.
            if self.selected.get(element) != wanted:
                raise SystemExit(f"the timer clock rule holds only while {element} is {wanted}; the profile selects {self.selected.get(element)}")
        cited = [self.policy("timer_clock")]
        for source_id, pattern, label in timer["cite"]:
            cited.append(self.cite_line("timerclock", source_id, pattern, label))
        for identifier in timer.get("evidence", []):
            find(self.value["evidence"], identifier)
            cited.append(identifier)
        for name in timer["elements"]:
            record = find(self.value["resources"], self.nodes[name])
            for identifier in cited:
                if identifier not in record["evidence"]:
                    record["evidence"].append(identifier)
        for identifier in cited:
            if identifier not in self.target["evidence"]:
                self.target["evidence"].append(identifier)
        for old, new in self.config.get("assumption_replacements", []):
            if self.target["assumptions"].count(old) != 1:
                raise SystemExit(f"assumption to replace not found exactly once: {old[:60]!r}")
            self.target["assumptions"][self.target["assumptions"].index(old)] = new
        for text in self.config.get("assumptions", []):
            self.target["assumptions"].append(text)
        if len(self.target["assumptions"]) > inventory.LIMITS["assumptions"]:
            raise SystemExit("more assumptions than a target may carry")


def run(graph, sources, summary, inventory_config, config, source_root):
    """The clock stage over a graph the domains stage has finished."""
    clocks = Clocks(graph, sources, inventory_config, config, source_root)
    clocks.tree()
    clocks.headers()
    clocks.mapping()
    clocks.gates()
    clocks.kernel_clocks()
    clocks.profile()
    clocks.rate_basis()
    clocks.statements()
    summary["clocks"] = clocks.summary
    return summary


def main():
    """Run the stage on a graph that does not hold its records yet."""
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--graph", type=pathlib.Path, required=True, help="the graph to extend, for example stm32c5/graph.json")
    parser.add_argument("--inventory", type=pathlib.Path, required=True, help="the family's inventory configuration")
    parser.add_argument("--config", type=pathlib.Path, required=True, help="the family's clocks configuration")
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if arguments.out.exists():
        raise SystemExit("refusing to overwrite an existing file; write the result beside the graph and move it")
    value = json.loads(arguments.graph.read_bytes())
    inventory_config = json.loads(arguments.inventory.read_text(encoding="utf-8"))
    inventory.use_schema(value, value["schema"])
    graph = inventory.Graph(value, inventory_config["target"], {})
    summary = run(graph, {}, {}, inventory_config, json.loads(arguments.config.read_text(encoding="utf-8")), arguments.source_root)
    inventory.write(graph, arguments.out, summary)


if __name__ == "__main__":
    sys.exit(main())
