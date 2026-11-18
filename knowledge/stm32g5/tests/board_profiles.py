# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Compose the source-pinned HAL2 cards and the MB2067 board restrictions."""
from collections import Counter


def add_profiles(graph, layer, source_bytes, root, inventory):
    """Append whole source records and update the existing physical pins."""
    for record in layer["sources"]:
        # Verify bytes again when regenerating. A changed vendor pack needs
        # new excerpt checks and new review bindings.
        inventory.Source(record, data=source_bytes(record, root))
        graph.value["sources"].append({k: v for k, v in record.items() if k != "package_file"})
    for section in ("evidence", "resources", "capabilities", "implementations"):
        records = graph.value[section]
        records.extend(layer[section])
        duplicates = [key for key, count in Counter(r["id"] for r in records).items() if count > 1]
        if duplicates:
            raise ValueError(f"duplicate {section} IDs: {duplicates}")
    pins = {r["id"]: r for r in graph.value["resources"] if r["kind"] == "pin"}
    for update in layer["pin_updates"]:
        pin = pins[update["id"]]
        for key in update["remove_attributes"]:
            del pin["attributes"][key]
        pin["attributes"].update(update["attributes"])
        pin["evidence"].extend(update["evidence"])
