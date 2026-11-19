# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Recreate the external page extractions and relocated Cube build inputs."""
import argparse
import hashlib
import json
from pathlib import Path


PACKAGE = Path(__file__).resolve().parent
TEMPLATE = Path("STM32CubeG5/examples/templates/no_os/NUCLEO-G564RE/cmake")


def checked_output(path, data, digest):
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"recreated bytes differ from package hash: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError(f"existing output differs: {path}")
    else:
        with path.open("xb") as new:
            new.write(data)


def pages(root):
    import pymupdf

    manifest = json.loads((PACKAGE / "catalog/stm32g5.json").read_bytes())
    sources = {s["id"]: s for s in manifest["sources"]}
    for row in sources.values():
        if row["kind"] != "pages":
            continue
        original = sources[row["options"]["origin_source"]]
        pdf = root / original["path"]
        if hashlib.sha256(pdf.read_bytes()).hexdigest() != original["sha256"]:
            raise ValueError(f"source hash mismatch: {pdf}")
        with pymupdf.open(pdf) as document:
            extracted = {
                "document_sha256": original["sha256"],
                "extractor": f"PyMuPDF {pymupdf.VersionBind} get_text(text)",
                "page_count": len(document),
                "pages": [
                    {"page": number, "text": page.get_text("text")}
                    for number, page in enumerate(document, 1)
                ],
            }
        data = (json.dumps(extracted, indent=2, ensure_ascii=False) + "\n").encode()
        checked_output(root / row["path"], data, row["sha256"])
        graph_inputs = json.loads((PACKAGE / "source-inputs.json").read_bytes())["external"]
        for source in graph_inputs:
            prefix = f"pages/{original['id']}-page-"
            if source["path"].startswith(prefix):
                page = int(Path(source["path"]).stem.rsplit("-", 1)[1])
                text = extracted["pages"][page - 1]["text"]
                checked_output(root / source["path"], text.encode(), source["sha256"])


def foundation(root):
    manifest = json.loads((PACKAGE / "foundation-manifest.json").read_bytes())
    for row in manifest["files"]:
        if not row["source"].startswith("foundation-inputs/"):
            continue
        original = root / TEMPLATE / row["destination"]
        text = original.read_text(encoding="utf-8")
        # The Cube sample builds from a nested cmake/ directory. The
        # materialized project keeps its SDK and generated sources at its root.
        for old, new in (
            ("template_no_os", "{{name}}"),
            ("../../../../../", ""),
            ("${CMAKE_BINARY_DIR}/../generated", "${CMAKE_BINARY_DIR}/generated"),
            ("../user_modifiable", "user_modifiable"),
            ("../generated", "generated"),
            ("../../application/", ""),
            ("../../application)", ".)"),
        ):
            text = text.replace(old, new)
        checked_output(root / row["source"], text.encode(), row["sha256"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", type=Path)
    args = parser.parse_args()
    pages(args.source_root)
    foundation(args.source_root)
    manifest = (PACKAGE / "foundation-manifest.json").read_bytes()
    checked_output(args.source_root / "foundation-inputs/manifest.json", manifest, hashlib.sha256(manifest).hexdigest())
    for path in (PACKAGE / "sources").rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            checked_output(args.source_root / path.relative_to(PACKAGE / "sources"), data, hashlib.sha256(data).hexdigest())
