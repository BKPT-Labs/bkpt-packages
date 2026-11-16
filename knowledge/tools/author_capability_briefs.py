# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Give every capability its brief: one short authored line for the index.

An agent first lists a chip's capabilities as an index of rows, each the
capability's ID and its brief, and reads the whole input contract only of the
capabilities it uses. The brief says what the capability gives the
application, so that together with the ID it tells siblings apart.

The lines are data: the family's briefs configuration lists one per
capability, and this stage writes each into its capability, right after the
description. It raises the graph to the schema that admits a brief and
refuses:

* a brief for a capability the graph does not hold, unless the recipe dropped
  that capability for its package;
* a capability without a brief, so a capability is never added without its
  line;
* a brief the firmware tool would refuse: 1 to 96 bytes of printable ASCII
  (0x20 to 0x7E), without a leading or trailing space;
* a capability that already carries a brief, or the same capability twice.

Solving does not read a brief, so no role, link, parameter or record changes
and every request keeps its plan. `compose_target.py` runs this stage last,
for a recipe that names a briefs configuration.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import author_device_inventory as inventory  # noqa: E402

# The first graph schema in which a capability may carry a brief.
BRIEF_SCHEMA = 6
BRIEF_BYTES = 96


def check(capability, brief):
    """Refuse a brief the firmware tool would refuse, with the reason."""
    if not isinstance(brief, str) or not brief:
        raise SystemExit(f"{capability}: a brief is a non-empty string")
    if any(not 0x20 <= ord(character) <= 0x7E for character in brief):
        raise SystemExit(f"{capability}: a brief is printable ASCII only")
    if len(brief) > BRIEF_BYTES:
        raise SystemExit(f"{capability}: a brief is at most {BRIEF_BYTES} bytes, this one {len(brief)}")
    if brief != brief.strip(" "):
        raise SystemExit(f"{capability}: a brief has no leading or trailing space")


def run(graph, summary, config, dropped=()):
    """Write the briefs into the graph's capabilities. `dropped` names the
    capabilities the recipe removed for its package."""
    if set(config) - {"schema", "graph_schema", "briefs"} or config.get("schema") != 1:
        raise SystemExit("a briefs configuration has schema 1 and only graph_schema and briefs")
    # Without graph_schema the graph keeps its schema and its bounds, as in the clock stage.
    inventory.use_schema(graph.value, config.get("graph_schema", graph.value["schema"]))
    if graph.value["schema"] < BRIEF_SCHEMA:
        raise SystemExit(f"a brief needs graph schema {BRIEF_SCHEMA}; this graph is schema {graph.value['schema']}")
    briefs = {}
    for entry in config["briefs"]:
        if set(entry) != {"capability", "brief"} or not isinstance(entry["capability"], str):
            raise SystemExit("a brief entry names its capability and its brief, nothing else")
        name = entry["capability"]
        if name in briefs:
            raise SystemExit(f"{name}: more than one brief")
        check(name, entry["brief"])
        briefs[name] = entry["brief"]
    held = [capability["id"] for capability in graph.value["capabilities"]]
    unknown = sorted(set(briefs) - set(held) - set(dropped))
    if unknown:
        raise SystemExit(f"briefs for capabilities the graph does not hold: {unknown}")
    missing = sorted(set(held) - set(briefs))
    if missing:
        raise SystemExit(f"capabilities without a brief: {missing}")
    written = []
    for capability in graph.value["capabilities"]:
        if "brief" in capability:
            raise SystemExit(f"{capability['id']}: already carries a brief; this stage writes every brief")
        # The brief stands right after the description, where a reader of the
        # graph looks for it. The record is changed in place, not replaced.
        ordered = {}
        for key, value in capability.items():
            ordered[key] = value
            if key == "description":
                ordered["brief"] = briefs[capability["id"]]
        ordered.setdefault("brief", briefs[capability["id"]])
        capability.clear()
        capability.update(ordered)
        written.append(len(capability["brief"]))
    summary["briefs"] = {"capabilities": len(written), "longest_bytes": max(written, default=0),
                         "not_held": sorted(set(briefs) - set(held))}
    return summary
