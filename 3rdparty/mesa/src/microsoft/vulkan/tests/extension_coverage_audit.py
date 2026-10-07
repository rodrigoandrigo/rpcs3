#!/usr/bin/env python3
"""Compare the explicit device-extension tables in RADV and Dozen source.

This is a source inventory only. It does not evaluate C preprocessor branches,
hardware/driver predicates, promoted core functionality, or runtime behavior.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def default_source_paths() -> tuple[Path, Path]:
    """Find the reference sources in either a full Mesa tree or a source subset."""
    for root in Path(__file__).resolve().parents:
        layouts = (
            (root / "src/amd/vulkan/radv_physical_device.c",
             root / "src/microsoft/vulkan/dzn_device.c"),
            (root / "vulkan/radv_physical_device.c",
             root / "microsoft/vulkan/dzn_device.c"),
        )
        for radv, dozen in layouts:
            if radv.is_file() and dozen.is_file():
                return radv, dozen

    # Keep useful defaults for explicit --radv/--dozen use when sources are elsewhere.
    root = Path(__file__).resolve().parents[3]
    return (root / "vulkan/radv_physical_device.c",
            root / "microsoft/vulkan/dzn_device.c")


DEFAULT_RADV, DEFAULT_DOZEN = default_source_paths()


def table_entries(path: Path, marker: str, description: str) -> set[str]:
    text = path.read_text(encoding="utf-8")
    marker_at = text.find(marker)
    if marker_at < 0:
        raise ValueError(f"Could not find {description} marker in {path}")
    open_brace = text.find("{", marker_at)
    close = text.find("};", open_brace)
    if open_brace < 0 or close < 0:
        raise ValueError(f"Could not locate complete {description} initializer in {path}")
    body = text[open_brace + 1 : close]
    return set(re.findall(r"^\s*\.([A-Za-z0-9_]+)\s*=", body, re.MULTILINE))


def later_dozen_true_assignments(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    return set(
        re.findall(
            r"\bsupported_extensions\.([A-Za-z0-9_]+)\s*=\s*true\b",
            text,
        )
    )


def build_inventory(radv_path: Path, dozen_path: Path) -> dict[str, object]:
    radv = table_entries(
        radv_path,
        "const struct vk_device_extension_table ext = {",
        "RADV device extension",
    )
    dozen = table_entries(
        dozen_path,
        "pdev->vk.supported_extensions = (struct vk_device_extension_table) {",
        "Dozen device extension",
    )
    later_true = later_dozen_true_assignments(dozen_path) - dozen
    combined_dozen = dozen | later_true
    common = radv & combined_dozen
    return {
        "scope": "explicit device-extension initializer names; not runtime support",
        "radv_source": str(radv_path),
        "dozen_source": str(dozen_path),
        "radv_initializer_count": len(radv),
        "dozen_initializer_count": len(dozen),
        "dozen_later_true_assignment_count": len(later_true),
        "common_count": len(common),
        "radv_missing_from_dozen_count": len(radv - combined_dozen),
        "dozen_only_count": len(combined_dozen - radv),
        "radv_extensions": sorted(radv),
        "dozen_initializer_extensions": sorted(dozen),
        "dozen_later_true_assignments": sorted(later_true),
        "common_extensions": sorted(common),
        "radv_missing_from_dozen": sorted(radv - combined_dozen),
        "dozen_only": sorted(combined_dozen - radv),
        "limitations": [
            "C preprocessor conditions are not evaluated.",
            "Hardware and runtime predicates are not evaluated.",
            "Promoted core functionality and indirect/shared implementations are not inferred.",
            "The inventory does not prove runtime behavior or Vulkan CTS conformance.",
        ],
    }


def markdown_report(data: dict[str, object]) -> str:
    missing = data["radv_missing_from_dozen"]
    assert isinstance(missing, list)
    grouped: dict[str, list[str]] = {}
    for name in missing:
        prefix = name.split("_", 1)[0]
        grouped.setdefault(prefix, []).append(name)
    lines = [
        "# Inventário estático de extensões: RADV × Dozen",
        "",
        f"- Campos na tabela RADV: **{data['radv_initializer_count']}**",
        f"- Campos no inicializador do Dozen: **{data['dozen_initializer_count']}**",
        f"- Habilitações posteriores explícitas no Dozen: **{data['dozen_later_true_assignment_count']}**",
        f"- Nomes em comum: **{data['common_count']}**",
        f"- Presentes no inicializador RADV, mas ausentes no Dozen: **{data['radv_missing_from_dozen_count']}**",
        f"- Nomes exclusivos do Dozen: **{data['dozen_only_count']}**",
        "",
        "> Este inventário compara nomes presentes em inicializadores C. Não equivale a uma lista de extensões efetivamente anunciadas por uma GPU: condições de compilação, predicados de hardware, promoções ao core e suporte indireto não são avaliados.",
        "",
        "A especificação Vulkan exige que as extensões anunciadas possam ser habilitadas conforme a versão exposta e proíbe anunciar combinações incompatíveis; veja [Extensões Vulkan](https://docs.vulkan.org/spec/latest/chapters/extensions.html). A implementação D3D12 expõe capacidades consultáveis por dispositivo via `ID3D12Device::CheckFeatureSupport`; isso precisa ser mapeado para a semântica Vulkan, não presumido pela semelhança dos nomes (documentação Microsoft: [CheckFeatureSupport](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-checkfeaturesupport)).",
        "",
        "## Nomes presentes no RADV e não listados no inicializador do Dozen",
        "",
    ]
    for prefix in sorted(grouped):
        lines.extend([f"### {prefix}", "", ", ".join(sorted(grouped[prefix])), ""])
    lines.extend(
        [
            "## Limites da validação",
            "",
            "O relatório é uma comparação estática das fontes fornecidas. Não compila Mesa, não executa o driver, não verifica cadeias de recursos nem substitui o Vulkan CTS. A ausência de um nome deste inicializador tampouco prova ausência de funcionalidade Vulkan equivalente no core ou em código compartilhado.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--radv", type=Path, default=DEFAULT_RADV, help="path to radv_physical_device.c")
    parser.add_argument("--dozen", type=Path, default=DEFAULT_DOZEN, help="path to dzn_device.c")
    parser.add_argument("--format", choices=("json", "markdown"), default="json")
    args = parser.parse_args()
    try:
        data = build_inventory(args.radv, args.dozen)
    except (OSError, ValueError) as error:
        print(f"extension audit failed: {error}", file=sys.stderr)
        return 2
    if args.format == "json":
        print(json.dumps(data, indent=2, ensure_ascii=False))
    else:
        print(markdown_report(data))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

