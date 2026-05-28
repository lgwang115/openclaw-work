#!/usr/bin/env python3
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path


ROOT = Path("/workspace/chip-kb")
ZIP_PATH = Path("/workspace/chip-kb.zip")

ITEMS = [
    {
        "dir": "rp2040",
        "name": "rp2040-datasheet.pdf",
        "url": "https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf",
    },
    {
        "dir": "rp2040",
        "name": "hardware-design-with-rp2040.pdf",
        "url": "https://pip.raspberrypi.com/documents/RP-008279-DS-hardware-design-with-rp2040.pdf",
    },
    {
        "dir": "rp2040",
        "name": "rp2040-documentation.adoc",
        "url": "https://raw.githubusercontent.com/raspberrypi/documentation/master/documentation/asciidoc/microcontrollers/microcontroller-chips/rp2040.adoc",
    },
    {
        "dir": "esp32",
        "name": "esp32-datasheet.pdf",
        "url": "https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf",
    },
    {
        "dir": "esp32",
        "name": "esp32-technical-reference-manual.pdf",
        "url": "https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf",
    },
    {
        "dir": "esp32",
        "name": "esp32-datasheet.html",
        "url": "https://documentation.espressif.com/esp32_datasheet_en.html",
    },
    {
        "dir": "stm32",
        "name": "stm32f4-rm0090-reference-manual.pdf",
        "url": "https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405-415-stm32f407-417-stm32f427-437-and-stm32f429-439-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf",
    },
    {
        "dir": "stm32",
        "name": "stm32f103-documentation.html",
        "url": "https://www.st.com/en/microcontrollers-microprocessors/stm32f103/documentation.html",
    },
    {
        "dir": "stm32",
        "name": "stm32-mcu-developer-zone.html",
        "url": "https://www.st.com/content/st_com/en/stm32-mcu-developer-zone.html",
    },
    {
        "dir": "nrf52840",
        "name": "nrf52840-product-brief.pdf",
        "url": "https://www.nordicsemi.com/-/media/Software-and-other-downloads/Product-Briefs/nRF52840-SoC-PB-23.pdf?la=en&hash=60847B0615FA41626CC73C9E5286C34F2481685E",
    },
    {
        "dir": "nrf52840",
        "name": "nrf52840-product-specification.html",
        "url": "https://docs.nordicsemi.com/bundle/ps_nrf52840/page/keyfeatures_html5.html",
    },
    {
        "dir": "riscv",
        "name": "riscv-isa-manual-main.zip",
        "url": "https://github.com/riscv/riscv-isa-manual/archive/refs/heads/main.zip",
    },
    {
        "dir": "riscv",
        "name": "riscv-isa-manual-snapshot.html",
        "url": "https://riscv.github.io/riscv-isa-manual/snapshot/spec/",
    },
    {
        "dir": "riscv",
        "name": "riscv-isa-manual-readme.md",
        "url": "https://raw.githubusercontent.com/riscv/riscv-isa-manual/main/README.md",
    },
    {
        "dir": "opentitan",
        "name": "opentitan-book-index.html",
        "url": "https://opentitan.org/book/",
    },
    {
        "dir": "opentitan",
        "name": "opentitan-hardware-design.html",
        "url": "https://opentitan.org/book/doc/contributing/hw/design.html",
    },
    {
        "dir": "opentitan",
        "name": "opentitan-design-methodology.html",
        "url": "https://opentitan.org/book/doc/contributing/hw/methodology.html",
    },
    {
        "dir": "opentitan",
        "name": "opentitan-readme.md",
        "url": "https://raw.githubusercontent.com/lowRISC/opentitan/master/README.md",
    },
    {
        "dir": "ibex",
        "name": "ibex-readme.md",
        "url": "https://raw.githubusercontent.com/lowRISC/ibex/master/README.md",
    },
    {
        "dir": "ibex",
        "name": "ibex-doc-index.rst",
        "url": "https://raw.githubusercontent.com/lowRISC/ibex/master/doc/index.rst",
    },
    {
        "dir": "ibex",
        "name": "ibex-core-readthedocs.html",
        "url": "https://ibex-core.readthedocs.io/",
    },
    {
        "dir": "ch32v307",
        "name": "ch32v307-datasheet.pdf",
        "url": "https://static.chipdip.ru/lib/164/DOC045164022.pdf",
    },
    {
        "dir": "ch32v307",
        "name": "ch32fv2x-v3x-reference-manual.pdf",
        "url": "https://www.wch-ic.com/download/file?id=324",
    },
    {
        "dir": "ch32v307",
        "name": "ch32v307-download-page.html",
        "url": "https://www.wch-ic.com/downloads/CH32V307DS0_PDF.html",
    },
    {
        "dir": "gd32vf103",
        "name": "gd32vf103-user-manual-rev1.5.pdf",
        "url": "https://www.gd32mcu.com/data/documents/userManual/GD32VF103_User_Manual_Rev1.5.pdf",
    },
    {
        "dir": "gd32vf103",
        "name": "gd32vf103-datasheet-rev1.1.pdf",
        "url": "https://files.pine64.org/doc/datasheet/pinecil/GD32VF103_Datasheet_Rev%201.1.pdf",
    },
    {
        "dir": "gd32vf103",
        "name": "gd32vf103-download-page.html",
        "url": "https://www.gd32mcu.com/en/download/0?kw=GD32VF1",
    },
    {
        "dir": "samd21",
        "name": "sam-d21-da1-family-datasheet.pdf",
        "url": "https://ww1.microchip.com/downloads/en/DeviceDoc/SAM-D21DA1-Family-Data-Sheet-DS40001882G.pdf",
    },
    {
        "dir": "samd21",
        "name": "atsamd21g18-product-page.html",
        "url": "https://www.microchip.com/en-us/product/atsamd21g18",
    },
    {
        "dir": "imxrt1060",
        "name": "imxrt1060-reference-manual.pdf",
        "url": "https://www.pjrc.com/teensy/IMXRT1060RM_rev3.pdf",
    },
    {
        "dir": "imxrt1060",
        "name": "imxrt1060-datasheet.pdf",
        "url": "https://www.nxp.com/docs/en/nxp/data-sheets/IMXRT1060CEC.pdf",
    },
    {
        "dir": "imxrt1060",
        "name": "imxrt1060-product-page.html",
        "url": "https://www.nxp.com/products/i.MX-RT1060",
    },
    {
        "dir": "ti-msp430",
        "name": "msp430fr5994-datasheet.pdf",
        "url": "https://www.ti.com/lit/ds/slase54d/slase54d.pdf",
    },
    {
        "dir": "ti-msp430",
        "name": "msp430fr58xx-59xx-6xx-family-users-guide.pdf",
        "url": "https://www.ti.com/lit/ug/slau367p/slau367p.pdf",
    },
    {
        "dir": "ti-msp430",
        "name": "msp430fr5994-product-page.html",
        "url": "https://www.ti.com/product/MSP430FR5994",
    },
    {
        "dir": "sifive",
        "name": "sifive-u74-core-complex-manual.pdf",
        "url": "https://www.scs.stanford.edu/~zyedidia/docs/sifive/sifive-u74.pdf",
    },
    {
        "dir": "sifive",
        "name": "sifive-u74mc-core-complex-manual.pdf",
        "url": "https://www.scs.stanford.edu/~zyedidia/docs/sifive/sifive-u74mc.pdf",
    },
    {
        "dir": "sifive",
        "name": "sifive-documentation.html",
        "url": "https://www.sifive.com/documentation",
    },
    {
        "dir": "intel-x86",
        "name": "intel-sdm-combined-volumes.pdf",
        "url": "https://cdrdv2-public.intel.com/851038/325462-087-sdm-vol-1-2abcd-3abcd-4.pdf",
    },
    {
        "dir": "intel-x86",
        "name": "intel-sdm-page.html",
        "url": "https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html",
    },
    {
        "dir": "amd64",
        "name": "amd-open-source-register-reference-family17h.pdf",
        "url": "https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/56255_OSRR.pdf",
    },
    {
        "dir": "amd64",
        "name": "amd64-volume5-media-x87-instructions.pdf",
        "url": "http://bluewaters.ncsa.illinois.edu/liferay-content/document-library/amd_5_26569.pdf",
    },
    {
        "dir": "amd64",
        "name": "amd-technical-information-portal.html",
        "url": "https://docs.amd.com/",
    },
]


def safe_dir(value: str) -> Path:
    parts = [p for p in value.split("/") if p and p not in {".", ".."}]
    return Path(*parts)


def download(item: dict, dest: Path) -> dict:
    url = item["url"]
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": "Mozilla/5.0 (compatible; OpenClaw-KB-Test-Downloader/1.0)",
            "Accept": "*/*",
        },
    )
    started = time.time()
    try:
        with urllib.request.urlopen(req, timeout=120) as response:
            final_url = response.geturl()
            status = getattr(response, "status", 200)
            content_type = response.headers.get("content-type", "")
            data = response.read()
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        return {
            "status": "ok",
            "url": url,
            "final_url": final_url,
            "path": str(dest.relative_to(ROOT)),
            "bytes": len(data),
            "http_status": status,
            "content_type": content_type,
            "elapsed_seconds": round(time.time() - started, 2),
        }
    except Exception as exc:
        return {
            "status": "failed",
            "url": url,
            "path": str(dest.relative_to(ROOT)),
            "error": f"{type(exc).__name__}: {exc}",
            "elapsed_seconds": round(time.time() - started, 2),
        }


def write_markdown_manifest(results: list[dict]) -> None:
    lines = [
        "# Chip KB Download Manifest",
        "",
        "This directory contains public chip/MCU/SoC/CPU architecture documents collected for OpenClaw + QMD local knowledge base testing.",
        "",
        "## Successful downloads",
        "",
        "| Directory | File | Size | Source |",
        "| --- | --- | ---: | --- |",
    ]
    for result in results:
        if result["status"] != "ok":
            continue
        path = Path(result["path"])
        lines.append(
            f"| `{path.parent}` | `{path.name}` | {result['bytes']} | {result['url']} |"
        )
    lines.extend(["", "## Failed downloads", ""])
    failures = [r for r in results if r["status"] != "ok"]
    if not failures:
        lines.append("No failed downloads.")
    else:
        lines.extend(["| Directory | File | Source | Error |", "| --- | --- | --- | --- |"])
        for result in failures:
            path = Path(result["path"])
            error = str(result.get("error", "")).replace("|", "\\|")
            lines.append(f"| `{path.parent}` | `{path.name}` | {result['url']} | {error} |")
    lines.extend(
        [
            "",
            "## Suggested QMD path",
            "",
            "```json5",
            "{",
            "  memory: {",
            '    backend: "qmd",',
            "    qmd: {",
            '      paths: [{ name: "chip-kb", path: "/path/to/chip-kb", pattern: "**/*.{md,html,rst,pdf,zip}" }],',
            "    },",
            "  },",
            "}",
            "```",
            "",
            "For the most stable QMD results, convert PDFs and HTML pages to Markdown before indexing.",
            "",
        ]
    )
    (ROOT / "README.md").write_text("\n".join(lines), encoding="utf-8")


def create_zip() -> None:
    if ZIP_PATH.exists():
        ZIP_PATH.unlink()
    with zipfile.ZipFile(ZIP_PATH, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for path in sorted(ROOT.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(ROOT.parent))


def main() -> int:
    ROOT.mkdir(parents=True, exist_ok=True)
    results = []
    for index, item in enumerate(ITEMS, start=1):
        dest = ROOT / safe_dir(item["dir"]) / item["name"]
        print(f"[{index:02d}/{len(ITEMS):02d}] {item['dir']}/{item['name']}", flush=True)
        result = download(item, dest)
        print(f"  -> {result['status']} ({result.get('bytes', 0)} bytes)", flush=True)
        results.append(result)
    (ROOT / "manifest.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    write_markdown_manifest(results)
    create_zip()
    print(f"Created {ZIP_PATH} ({ZIP_PATH.stat().st_size} bytes)", flush=True)
    return 0 if all(r["status"] == "ok" for r in results) else 2


if __name__ == "__main__":
    sys.exit(main())
