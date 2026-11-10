# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Read-only integrity checks for external source surveys; not semantic qualification."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
if not __debug__:
    raise RuntimeError("Integrity verification requires Python without -O/-OO.")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_json(path):
    def unique_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError((str(path), "duplicate JSON key", key))
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique_keys)


def verify(name):
    artifact = ROOT / name
    data = read_json(artifact)
    assert data["execution_permission"] == "none", (name, "unexpected execution authority")
    assert data["build"] == "not_tested" and data["hardware"] == "not_observed", (name, "unexpected qualification state")
    pages = {}
    for key, source in data["sources"].items():
        assert digest(source["path"]) == source["sha256"], (name, key, "source")
        if "extraction_path" in source:
            path = Path(source["extraction_path"])
            assert digest(path) == source["extraction_sha256"], (name, key, "extraction")
            extraction = read_json(path)
            pages[key] = {page["page"]: page for page in extraction["pages"]}
    for key, evidence in data["evidence"].items():
        text = evidence["text"]
        hashes = [evidence[field] for field in ("sha256", "text_sha256") if field in evidence]
        assert hashes and all(hashlib.sha256(text.encode()).hexdigest() == value for value in hashes), (name, key, "text hash")
        source = evidence["source"]
        if "start_line" in evidence:
            lines = Path(data["sources"][source]["path"]).read_text(encoding="utf-8").splitlines()
            assert 1 <= evidence["start_line"] <= evidence["end_line"] <= len(lines), (name, key, "line range")
            actual = "\n".join(lines[evidence["start_line"] - 1:evidence["end_line"]])
            assert actual == text, (name, key, "source lines")
        elif "page" in evidence:
            assert source in pages, (name, key, "missing extraction")
            page = pages[source][evidence["page"]]
            assert text in (page["text"], page.get("layout_text")), (name, key, "source page")
        else:
            raise AssertionError((name, key, "unsupported evidence locator"))
    for visual in data.get("visual_checks", []):
        assert digest(visual["path"]) == visual["sha256"], (name, "visual")

    def check_references(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if (key == "evidence" or key.endswith("_evidence")) and isinstance(child, list):
                    for reference in child:
                        assert reference in data["evidence"], (name, "unknown evidence", reference)
                if key == "visual_check" and isinstance(child, dict):
                    assert digest(child["path"]) == child["sha256"], (name, "nested visual")
                if key == "inventory_binding" and isinstance(child, dict):
                    inventory_path = (ROOT / child["file"]).resolve()
                    assert inventory_path.parent == ROOT and type(child["index"]) is int and child["index"] >= 0, (name, "inventory location/index")
                    inventory = read_json(inventory_path)
                    entry = inventory["entries"][child["index"]]
                    assert entry["section"] == child["section"] and entry["revision_status"] == child["revision_status"], (name, "inventory binding")
                check_references(child)
        elif isinstance(value, list):
            ids = [child["id"] for child in value if isinstance(child, dict) and "id" in child]
            assert len(ids) == len(set(ids)), (name, "duplicate record id")
            for child in value:
                check_references(child)

    check_references(data)
    for field, linked in [("base_candidate_sha256", "candidate.json"),
                          ("register_supplement_sha256", "cordic-registers.json"),
                          ("errata_sha256", "errata.json"),
                          ("package_routes_sha256", "package-routes.json"),
                          ("sdk_lifecycle_sha256", "sdk-lifecycle.json"),
                          ("dma_routing_sha256", "dma-routing.json"),
                          ("composition_sha256", "composition.json")]:
        if field in data:
            assert data[field] == digest(ROOT / linked), (name, field)
    return {"file": name, "sha256": digest(artifact), "sources": len(data["sources"]),
            "excerpts": len(data["evidence"]), "qualification": "integrity_only"}


if __name__ == "__main__":
    print(json.dumps([verify(name) for name in
                      ("cordic-registers.json", "cordic-functions.json", "sdk-lifecycle.json",
                       "crc-contracts.json", "device-inventory.json", "dma-routing.json",
                       "adc-contracts.json", "watchdog-contracts.json", "dac-contracts.json",
                       "icache-contracts.json", "analog-comparators-contracts.json",
                       "ramcfg-contracts.json", "crs-contracts.json",
                       "dbg-contracts.json", "gpio-contracts.json", "basic-timer-contracts.json",
                       "sbs-contracts.json", "rng-contracts.json", "exti-contracts.json",
                       "hash-contracts.json", "nvic-systick-contracts.json", "pwr-contracts.json",
                       "electronic-signature-contracts.json", "rcc-contracts.json", "aes-contracts.json", "flash-contracts.json",
                       "lptim-contracts.json", "dma-controller-contracts.json",
                       "i2c-contracts.json", "rtc-contracts.json", "tamp-contracts.json",
                       "spi-contracts.json", "system-boot-contracts.json", "general-timer-contracts.json",
                       "tim12-contracts.json", "fdcan-contracts.json",
                       "tim15-contracts.json", "i3c-contracts.json", "lpuart-contracts.json", "usart-contracts.json",
                       "memory-bus-contracts.json", "electrical-contracts.json", "cpu-mpu-fpu-contracts.json",
                       "usb-contracts.json", "advanced-timer-contracts.json",
                       "dac-register-map.json", "interconnect-contracts.json")], indent=2))
