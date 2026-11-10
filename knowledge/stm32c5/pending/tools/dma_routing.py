# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Build source-only DMA maps with exact-header presence filtering."""
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent
SDK = Path("D:/Projects/BKPT-Data/sdk/stm32c5-hal2-2.1.0")
SOURCE = Path("D:/Projects/BKPT-Data/firmware/sources/st/c5")
if not __debug__:
    raise RuntimeError("DMA source transcription requires Python without optimization.")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build():
    paths = {"header": SDK / "stm32c5xx_dfp/Include/stm32c542xx.h",
             "LL": SDK / "stm32c5xx_drivers/ll/stm32c5xx_ll_dma.h",
             "SVD": SOURCE / "pack-2.1.0/device.svd"}
    data = {"schema": "bkpt.external.dma-routing.v1", "publication": "unpublished",
            "target": "STM32C542RCT6 / LQFP64 / NUCLEO-C542RC", "sources": {}, "evidence": {},
            "execution_permission": "none", "implementation": "not_authored",
            "build": "not_tested", "hardware": "not_observed", "review": "See current external review receipts; source-only retention, no expanded composition admission."}
    lines = {}
    for key, path in paths.items():
        data["sources"][key] = {"path": str(path), "sha256": digest(path)}
        lines[key] = path.read_text(encoding="utf-8").splitlines()

    def excerpt(key, source, start, end):
        text = "\n".join(lines[source][start - 1:end])
        data["evidence"][key] = {"source": source, "start_line": start, "end_line": end,
                                 "text": text, "sha256": hashlib.sha256(text.encode()).hexdigest()}

    excerpt("LL.requests_and_triggers", "LL", 370, 760)
    excerpt("LL.trigger_modes", "LL", 296, 340)
    excerpt("header.channels", "header", 14041, 14063)
    excerpt("header.trigger_width", "header", 3963, 3966)
    symbols = {match[1] for line in lines["header"]
               if (match := re.match(r"#define\s+(\w+)\b", line))}
    channel_definition = next(line for line in lines["header"] if re.match(r"#define\s+DAC_NB_OF_CHANNEL\b", line))
    assert re.search(r"\b2U(?:L)?\b", channel_definition), channel_definition
    dac_line = lines["header"].index(channel_definition) + 1
    excerpt("header.DAC_channels", "header", dac_line, dac_line)
    relevant_guards = set()

    def guard_value(condition):
        match = re.fullmatch(r"defined\s*\(\s*(\w+)\s*\)", condition)
        if match:
            relevant_guards.add(match[1])
            return match[1] in symbols
        if condition == "defined(DAC_NB_OF_CHANNEL) && (DAC_NB_OF_CHANNEL == 2UL)":
            relevant_guards.add("DAC_NB_OF_CHANNEL")
            return True  # Exact literal declaration checked and bound above.
        raise ValueError(("unsupported presence guard", condition))

    stack, records = [], []
    for index in range(373, 760):
        line = lines["LL"][index]
        if line.startswith("#if "):
            condition = line[4:].strip()
            stack.append({"condition": condition, "present": guard_value(condition)})
        elif line.startswith("#endif"):
            assert stack, (index + 1, "unmatched endif")
            stack.pop()
        elif line.startswith(("#else", "#elif", "#ifdef", "#ifndef")):
            raise ValueError((index + 1, "unsupported conditional"))
        match = re.match(r"#define\s+(LL_LPDMA([12])_(REQUEST|TRIGGER)_\w+)\s+(\d+)U\b", line)
        if match:
            records.append({"name": match[1], "controller": "LPDMA" + match[2],
                            "kind": match[3].lower(), "selector": int(match[4]),
                            "line": index + 1, "presence_guards": [dict(g) for g in stack],
                            "exact_header_presence_candidate": all(g["present"] for g in stack),
                            "evidence": ["LL.requests_and_triggers"]})
    assert not stack
    assert len({r["name"] for r in records}) == len(records)
    data["LL_literal_definitions"] = records
    data["presence_evaluation"] = {"scope": "Only explicit guards in the pinned LL map slice against literal definitions in the exact header; not a full C preprocessor or operation admission.",
                                    "symbols": {s: s in symbols for s in sorted(relevant_guards)}}
    data["counts"] = {}
    for controller in ("LPDMA1", "LPDMA2"):
        requests = [r for r in records if r["controller"] == controller and r["kind"] == "request" and r["exact_header_presence_candidate"]]
        triggers = [r for r in records if r["controller"] == controller and r["kind"] == "trigger" and r["exact_header_presence_candidate"]]
        values = sorted({r["selector"] for r in requests})
        assert len(values) == 58 and len(triggers) == 29
        data["counts"][controller] = {"request_names": len(requests), "request_selector_values": values,
                                      "trigger_names": len(triggers), "trigger_selector_values": sorted({r["selector"] for r in triggers})}
    data["SVD_channel_fields"] = []
    for peripheral in ET.parse(paths["SVD"]).getroot().findall("./peripherals/peripheral"):
        name = peripheral.findtext("name")
        if name not in ("LPDMA1", "LPDMA2"):
            continue
        for register in peripheral.findall("./registers/register"):
            if re.fullmatch(r"LPDMA_C[0-3]TR2", register.findtext("name", "")):
                for field in register.findall("./fields/field"):
                    if field.findtext("name") in ("TRIGSEL", "PFREQ"):
                        data["SVD_channel_fields"].append({"controller": name, "register": register.findtext("name"),
                            "field": field.findtext("name"), "bit_offset": int(field.findtext("bitOffset")),
                            "bit_width": int(field.findtext("bitWidth")), "source": "SVD"})
    assert len(data["SVD_channel_fields"]) == 16
    # Bind complete TR2 register XML blocks with their enclosing controller identity.
    current_controller = None
    register_start = None
    register_name = None
    for index, line in enumerate(lines["SVD"]):
        match = re.fullmatch(r"\s{6}<name>([^<]+)</name>", line)
        if match:
            current_controller = match[1]
        if line == "        <register>":
            register_start, register_name = index + 1, None
        match = re.fullmatch(r"\s{10}<name>([^<]+)</name>", line)
        if match and register_start:
            register_name = match[1]
        if line == "        </register>" and current_controller in ("LPDMA1", "LPDMA2") and register_name and re.fullmatch(r"LPDMA_C[0-3]TR2", register_name):
            key = f"SVD.{current_controller}.{register_name}"
            excerpt(key, "SVD", register_start, index + 1)
            for row in data["SVD_channel_fields"]:
                if row["controller"] == current_controller and row["register"] == register_name:
                    row["evidence"] = [key]
    assert all("evidence" in row for row in data["SVD_channel_fields"])
    path = SOURCE / "documents/RM0522.pdf"
    extraction = path.with_suffix(".pages.json")
    data["sources"]["RM0522"] = {"path": str(path), "sha256": digest(path),
        "extraction_path": str(extraction), "extraction_sha256": digest(extraction)}
    for page in json.loads(extraction.read_text(encoding="utf-8"))["pages"]:
        if page["page"] in list(range(383, 390)) + [410, 414, 415, 431, 432, 433]:
            text = page["text"]
            data["evidence"][f"RM0522:p{page['page']}"] = {"source": "RM0522", "page": page["page"],
                "text": text, "sha256": hashlib.sha256(text.encode()).hexdigest()}
    data["channels"] = {"LPDMA1": [0, 1, 2, 3], "LPDMA2": [0, 1, 2, 3],
                         "evidence": ["header.channels", "RM0522:p383"]}
    data["RM_trigger_selector_intersection"] = list(range(24)) + list(range(28, 32))
    for controller in ("LPDMA1", "LPDMA2"):
        assert data["RM_trigger_selector_intersection"] == sorted(set(data["counts"][controller]["trigger_selector_values"]) - {36})
    data["conflicts"] = [
        {"id": "PFREQ_channel_domain", "detail": "RM Table92: CH0/CH7, intersecting exact inventory to CH0. Header: CH0/CH3 on both controllers. SVD exposes PFREQ on all four. CH0 is a common candidate, not full-domain resolution.", "evidence": ["RM0522:p387", "header.channels"]},
        {"id": "TRIGSEL_width", "detail": "RM/header specify six bits; all eight SVD channel TR2 fields specify five. Do not truncate a future selector based on SVD width.", "evidence": ["RM0522:p432", "header.trigger_width"]},
        {"id": "COMP2_trigger", "detail": "LL provides COMP2_OUT selector36 under exact COMP2 presence. RM Table94 ends at35; SVD five-bit field cannot encode36. SDK-only candidate, no admitted routing.", "evidence": ["LL.requests_and_triggers", "RM0522:p388", "RM0522:p389"]},
        {"id": "TRIGSEL_polarity_prose", "detail": "RM p432 TRIGSEL prose says active for TRIGPOL=00, contradicting its polarity table and pp388/414. Preserve this source defect.", "evidence": ["RM0522:p432", "RM0522:p388", "RM0522:p414"]}]
    data["contracts"] = [
        {"id": "distinct_roles", "detail": "Request source, transfer address and trigger source are distinct. A peripheral request can support data/control/status transfers, but arbitrary endpoint pairings are not qualified.", "evidence": ["RM0522:p410"]},
        {"id": "software_request", "detail": "SWREQ=1 ignores REQSEL, BREQ and PFREQ. GPIO IDR reads do not create a GPIO hardware-request selector.", "evidence": ["RM0522:p432", "RM0522:p433"]},
        {"id": "special_protocols", "detail": "LPTIM1_UE selector54 requires BREQ=1. I3C1_RX selector57 is listed for early termination; PFREQ channel conflict remains material.", "evidence": ["RM0522:p387", "RM0522:p388"]},
        {"id": "request_ownership", "detail": "Do not assign one physical hardware request to simultaneously active competing channels, including aliases/controller routes; no setting-error report is supplied. LPDMA2 SPI2S1_RX and SPI1_RX both encode4, not two endpoints.", "evidence": ["RM0522:p410", "RM0522:p433", "LL.requests_and_triggers"]},
        {"id": "trigger_fanout_and_loss", "detail": "Trigger fanout is permitted; one hit may pend, another can be lost with TOF without automatic channel disable. Fanout does not promise simultaneous sampling or completion order. Arbitration precedes the read, so GPIO state is not captured at the trigger edge.", "evidence": ["RM0522:p414", "RM0522:p415"]},
        {"id": "channel_lifecycle", "detail": "No FIFO: one internally registered source-transfer cell. TR2 is read-only while enabled; configure only with EN=0/completed channel. Block/link/single trigger modes have distinct gating semantics.", "evidence": ["RM0522:p383", "RM0522:p384", "RM0522:p431", "RM0522:p432"]}]
    svd_keys = [key for key in data["evidence"] if key.startswith("SVD.")]
    for conflict in data["conflicts"]:
        if conflict["id"] in ("PFREQ_channel_domain", "TRIGSEL_width", "COMP2_trigger"):
            conflict["evidence"].extend(svd_keys)
    additions = {"software_request": ["RM0522:p410"], "special_protocols": ["RM0522:p386"],
                 "trigger_fanout_and_loss": ["RM0522:p410"], "channel_lifecycle": ["LL.trigger_modes"],
                 "request_ownership": ["RM0522:p384", "RM0522:p385", "RM0522:p386"]}
    for contract in data["contracts"]:
        contract["evidence"].extend(additions.get(contract["id"], []))
    data["contracts"].append({"id": "DAC_channel_presence", "detail": "DAC1_CH2 selector74 survives exact two-channel declaration; package naming/electrical conflict remains separate.", "evidence": ["header.DAC_channels", "LL.requests_and_triggers", "RM0522:p386"]})
    data["remaining"] = ["EXTI trigger stage before/after edge detector remains unresolved.",
        "No hardware-request endpoint/address/width protocol admitted; all peripheral lifecycle and ownership gates remain.",
        "Do not broaden composition.json admission or mutate the active executable catalog.",
        "DAC1_CH2 selector74 survives exact two-channel guard; package DAC identity conflict remains separate.",
        "LL TIM*_TRGI versus RM tim*_trig_dma naming is selector correspondence, not signal-delivery qualification."]
    return data


if __name__ == "__main__":
    result = build()
    (ROOT / "dma-routing.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({controller: {k: len(v) if isinstance(v, list) else v for k, v in counts.items()}
                      for controller, counts in result["counts"].items()}))
