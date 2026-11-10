# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Transcribe exact-device declarations; never infer executable capabilities."""
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent
SDK = Path("D:/Projects/BKPT-Data/sdk/stm32c5-hal2-2.1.0")
SOURCES = Path("D:/Projects/BKPT-Data/firmware/sources/st/c5")
if not __debug__:
    raise RuntimeError("Source inventory generation requires Python without -O/-OO.")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build():
    header = SDK / "stm32c5xx_dfp/Include/stm32c542xx.h"
    startup = SDK / "stm32c5xx_dfp/Source/startup_stm32c542xx.c"
    svd = SOURCES / "pack-2.1.0/device.svd"
    data = {"schema": "bkpt.external.device-inventory.v1", "publication": "unpublished",
            "target": "STM32C542RCT6 / LQFP64 / NUCLEO-C542RC",
            "sources": {}, "evidence": {}, "execution_permission": "none",
            "implementation": "not_authored", "build": "not_tested", "hardware": "not_observed",
            "review": "See current external review receipts; source-only retention, no executable admission."}
    for key, path in (("header", header), ("startup", startup), ("SVD", svd)):
        data["sources"][key] = {"path": str(path), "sha256": sha(path)}

    def excerpt(key, source, lines, start, end):
        text = "\n".join(lines[start - 1:end])
        data["evidence"][key] = {"source": source, "start_line": start, "end_line": end,
                                 "text": text, "sha256": hashlib.sha256(text.encode()).hexdigest()}

    lines = header.read_text(encoding="utf-8").splitlines()
    declarations = []
    for index, line in enumerate(lines):
        match = re.match(r"\s*(\w+)_IRQn\s*=\s*(-?\d+)\s*,", line)
        if match:
            declarations.append({"name": match[1], "number": int(match[2]), "line": index + 1})
    excerpt("header.IRQ", "header", lines, min(r["line"] for r in declarations), max(r["line"] for r in declarations))
    external = [row for row in declarations if row["number"] >= 0]
    assert len({row["number"] for row in external}) == len(external)
    startup_lines = startup.read_text(encoding="utf-8").splitlines()
    beginning = startup_lines.index("const VECTOR_TABLE_Type __VECTOR_TABLE[] __VECTOR_TABLE_ATTRIBUTE =") + 2
    end = next(i for i in range(beginning, len(startup_lines)) if startup_lines[i].strip() == "};")
    vector = []
    for index in range(beginning, end):
        entry = startup_lines[index].split("/*", 1)[0].strip().rstrip(",").strip()
        if entry:
            vector.append({"slot": len(vector), "entry": entry, "line": index + 1})
    excerpt("startup.vector", "startup", startup_lines, beginning + 1, end)
    positions = [{**row, "irq_number": row["slot"] - 16} for row in vector[16:]]
    by_number = {row["number"]: row["name"] for row in external}
    for row in positions:
        number, entry = row["irq_number"], row["entry"]
        if entry == "0":
            assert number not in by_number
        else:
            assert entry == by_number[number] + "_IRQHandler", row
    assert len([row for row in positions if row["entry"] != "0"]) == len(external)
    svd_irqs = [{"peripheral": p.findtext("name"), "name": irq.findtext("name"),
                 "number": int(irq.findtext("value"), 0)}
                for p in ET.parse(svd).getroot().findall("./peripherals/peripheral")
                for irq in p.findall("interrupt")]
    for irq in svd_irqs:
        assert by_number[irq["number"]] == irq["name"], irq
    data["interrupt_inventory"] = {
        "header_declarations": declarations, "startup_positions": positions,
        "svd_entries": svd_irqs, "counts": {"header_declarations": len(declarations),
            "negative_core_declarations": len(declarations) - len(external),
            "named_external_IRQs": len(external), "startup_total_slots": len(vector),
            "startup_external_positions": len(positions), "svd_entries": len(svd_irqs)},
        "explicit_zero_positions": [r["irq_number"] for r in positions if r["entry"] == "0"],
        "header_IRQs_missing_from_SVD": [r for r in external if r["number"] not in {x["number"] for x in svd_irqs}],
        "evidence": ["header.IRQ", "startup.vector"]}

    predicates = {}
    guard_stack = []
    for index, line in enumerate(lines):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind = directive[1]
            if kind in ("if", "ifdef", "ifndef"):
                guard_stack.append(line.strip())
            elif kind in ("else", "elif"):
                guard_stack[-1] += " -> " + line.strip()
            elif kind == "endif":
                guard_stack.pop()
        match = re.match(r"\s*#\s*define\s+(IS_\w*INSTANCE)\(INSTANCE\)\s+(.*)", line)
        if not match:
            continue
        finish = index
        while lines[finish].rstrip().endswith("\\"):
            finish += 1
        text = " ".join(x.rstrip().removesuffix("\\") for x in lines[index:finish + 1])
        body = text.split("(INSTANCE)", 1)[1].strip()
        assert not re.search(r"\bX\b", body), (match[1], "reserved parser placeholder in original body")
        targets = re.findall(r"\(INSTANCE\)\s*==\s*(\w+)", body)
        aliases = re.findall(r"(IS_\w*INSTANCE)\(INSTANCE\)", body)
        residual = re.sub(r"\(INSTANCE\)\s*==\s*\w+", "X", body)
        residual = re.sub(r"IS_\w*INSTANCE\(INSTANCE\)", "X", residual)
        assert re.fullmatch(r"[X()|\s]+", residual), (match[1], body)
        # This extractor supports positive OR expressions only, never arbitrary C.
        assert "|" not in residual.replace("||", ""), (match[1], body)
        tokens = re.findall(r"\|\||[()X]", residual)
        cursor = 0

        def term():
            nonlocal cursor
            assert cursor < len(tokens), (match[1], "missing term")
            token = tokens[cursor]
            cursor += 1
            if token == "(":
                expression()
                assert cursor < len(tokens) and tokens[cursor] == ")", (match[1], "unbalanced")
                cursor += 1
            else:
                assert token == "X", (match[1], "invalid term")

        def expression():
            nonlocal cursor
            term()
            while cursor < len(tokens) and tokens[cursor] == "||":
                cursor += 1
                term()

        expression()
        assert cursor == len(tokens), (match[1], "trailing tokens")
        assert match[1] not in predicates, (match[1], "duplicate conditional definition unsupported")
        evidence = "predicate." + match[1]
        excerpt(evidence, "header", lines, index + 1, finish + 1)
        predicates[match[1]] = {"direct_symbols": targets, "alias_predicates": aliases,
                               "preprocessor_context": list(guard_stack), "evidence": [evidence]}

    def resolve(name, seen=()):
        assert name not in seen, ("predicate cycle", name)
        row = predicates[name]
        values = set(row["direct_symbols"])
        for alias in row["alias_predicates"]:
            assert row["preprocessor_context"] == predicates[alias]["preprocessor_context"], (name, alias, "different conditional context")
            values.update(resolve(alias, seen + (name,)))
        return values

    for name, row in predicates.items():
        row["derived_symbol_union"] = sorted(resolve(name))
        row["qualification"] = "Literal header predicate only; not peripheral, mode, pin, errata or runtime admission."
    data["instance_predicates"] = predicates
    data["predicate_count"] = len(predicates)
    adc_start = next(i + 1 for i, line in enumerate(lines) if re.match(r"#define\s+IS_ADC_ALL_INSTANCE\b", line))
    adc_end = next(i + 1 for i, line in enumerate(lines) if re.match(r"#define\s+ADC_MULTI_INSTANCE_MASTER\b", line))
    while lines[adc_end - 1].rstrip().endswith("\\"):
        adc_end += 1
    excerpt("header.adc_residue", "header", lines, adc_start, adc_end)
    data["excluded_macro_residue"] = {
        "detail": "ADC_MULTI_INSTANCE_MASTER(adc_instance) references ADC2 outside the selected IS_*INSTANCE signature. ADC1-only predicate and ADC12_COMMON do not establish ADC2 availability.",
        "evidence": ["header.adc_residue"]}
    for document, page_numbers in (("RM0522", range(440, 445)), ("DS15135", [14])):
        path = SOURCES / "documents" / (document + ".pdf")
        extraction_path = path.with_suffix(".pages.json")
        data["sources"][document] = {"path": str(path), "sha256": sha(path),
            "extraction_path": str(extraction_path), "extraction_sha256": sha(extraction_path)}
        for page in json.loads(extraction_path.read_text(encoding="utf-8"))["pages"]:
            if page["page"] in page_numbers:
                text = page["text"]
                data["evidence"][f"{document}:p{page['page']}"] = {"source": document,
                    "page": page["page"], "text": text, "sha256": hashlib.sha256(text.encode()).hexdigest()}
    data["published_count_conflicts"] = {
        "DS_maskable_channel_claim": 88, "RM_maskable_channel_claim": 98,
        "RM_table_external_position_range_inclusive": [0, 98],
        "qualification": "Prose counts match highest positions, but an editorial off-by-one explanation is only an inference. Neither equals named exact-device IRQ count.",
        "evidence": ["DS15135:p14"] + [f"RM0522:p{p}" for p in range(440, 445)]}
    data["limits"] = [
        "RM directs checking actual peripheral availability; exact startup zeros are not fabricated peripheral declarations.",
        "RM positions 89..98 are beyond the exact startup table, not additional zeros authored by it.",
        "Weak Default_IRQHandler aliases are fallback linkage, not merged physical IRQ identities.",
        "Header predicates describe SDK checks and may contain family residue. Do not treat symbolic unions as executable capability admission.",
        "Preprocessor context is a textual guard stack, not evaluated preprocessing; no modeling of undef, other-header redefinitions, command-line definitions, include order or directives inside comments/continuations.",
        "An SVD interrupt omission is not evidence of physical presence or absence; COMP12 exists in the SVD without interrupt entries.",
        "Preserve exact physical EXTI line identity across ports and consumers; no active catalog changes.",
        "Core vector routing, priority policy, handler implementation and hardware behavior remain unqualified."]
    data["IRQ_naming_cautions"] = {
        "detail": "Grouped TIM break/error and trigger/commutation/direction/index sources share vectors. LPUART Rx/Tx share IRQ56. PVD/PWR_PVD, ER/ERR and USB_FS/USB_DRD_FS are source naming correspondences, not additional IRQs; USB OTG wording does not establish OTG capability.",
        "evidence": ["header.IRQ", "startup.vector"] + [f"RM0522:p{p}" for p in range(440, 445)]}
    return data


if __name__ == "__main__":
    result = build()
    (ROOT / "device-inventory.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"predicates": result["predicate_count"], "IRQ_counts": result["interrupt_inventory"]["counts"]}))
