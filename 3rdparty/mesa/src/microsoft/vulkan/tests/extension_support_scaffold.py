#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Generate an auditable implementation scaffold for pending Dozen extensions.

The scaffold records Vulkan registry requirements and a per-extension checklist.
It deliberately does not advertise or implement extensions.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path
from typing import Any

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]
DEFAULT_BACKLOG = TEST_DIR / "extension_support_backlog.json"
DEFAULT_REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"
DEFAULT_DOZEN = DZN_DIR / "dzn_device.c"
DEFAULT_OUTPUT = TEST_DIR / "extension_support_scaffold.json"

ARCHITECTURE_LAYERS = [
    {
        "layer": "registry_and_generated_api",
        "inputs": ["src/vulkan/registry/vk.xml"],
        "purpose": "Define nomes Vulkan, estruturas, aliases e comandos; os geradores Mesa produzem metadados de entrypoints.",
        "driver_work_required": True,
        "warning": "Declarações geradas e processamento pNext comum não implementam a semântica D3D12.",
    },
    {
        "layer": "backend_capability_probe",
        "inputs": ["ID3D12Device::CheckFeatureSupport", "device interfaces", "shader model"],
        "purpose": "Consultar uma capacidade D3D12 real por adaptador e normalizá-la para as consultas Vulkan.",
        "driver_work_required": True,
    },
    {
        "layer": "vulkan_feature_property_and_dependency_reporting",
        "inputs": ["dzn_device.c", "vk_features", "vk_properties"],
        "purpose": "Reportar apenas features/properties implementadas e aplicar dependências e regras de versão da API.",
        "driver_work_required": True,
    },
    {
        "layer": "pnext_and_object_translation",
        "inputs": ["device, pipeline, descriptor, image, memory creation paths"],
        "purpose": "Consumir cada estrutura de entrada e preservar a validação e a semântica de objetos Vulkan.",
        "driver_work_required": True,
    },
    {
        "layer": "command_dispatch_and_execution",
        "inputs": ["dzn entrypoints", "command-buffer/device handlers"],
        "purpose": "Implementar comandos ou comprovar o caminho comum/alias, incluindo falhas e sincronização.",
        "driver_work_required": True,
    },
    {
        "layer": "shader_translation_and_d3d12_mapping",
        "inputs": ["SPIR-V/NIR", "DXIL", "D3D12 descriptors and barriers"],
        "purpose": "Implementar semânticas de shader e recursos que o runtime Vulkan comum não fornece.",
        "driver_work_required": True,
    },
    {
        "layer": "verification",
        "inputs": ["static tests", "Windows/D3D12 runtime", "Vulkan CTS"],
        "purpose": "Verificar descoberta, habilitação, estruturas, comportamento, dependências e conformidade em runtime real.",
        "driver_work_required": True,
    },
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def display_path(path: Path) -> str:
    """Use repository-relative paths in generated, portable artifacts."""
    resolved = path.resolve()
    try:
        return resolved.relative_to(MESA_ROOT.resolve()).as_posix()
    except ValueError:
        return str(resolved)


def import_review(review_path: Path) -> dict[str, Any]:
    """Reduce the reviewed 171-candidate matrix to portable backlog policy data."""
    review = json.loads(review_path.read_text(encoding="utf-8"))
    candidates = review.get("candidates")
    if not isinstance(candidates, list):
        raise ValueError("review JSON does not contain a candidates array")
    pending = []
    implemented = []
    for candidate in candidates:
        entry = {
            "field": candidate["field"],
            "extension": candidate["extension"],
            "decision_category": candidate.get("decision_category", "unclassified"),
            "decision_reason": candidate.get("decision_reason", ""),
            "group_tags": candidate.get("group_tags", []),
        }
        if candidate.get("status") == "rejected_not_advertised":
            pending.append(entry)
        elif candidate.get("status") == "implemented":
            implemented.append(candidate["field"])
        else:
            raise ValueError(
                f"unexpected candidate status for {candidate.get('field')}: "
                f"{candidate.get('status')}"
            )
    return {
        "schema_version": 1,
        "mesa_version": review.get("mesa_version"),
        "reviewed_candidate_count": len(candidates),
        "review_input_sha256": sha256(review_path),
        "implemented_fields": sorted(implemented),
        "pending_extensions": sorted(pending, key=lambda entry: entry["field"]),
        "source": f"{review_path.name}; reviewed status is policy input, not runtime evidence.",
    }


def extension_table_fields(dozen_path: Path) -> set[str]:
    text = dozen_path.read_text(encoding="utf-8")
    marker = "pdev->vk.supported_extensions = (struct vk_device_extension_table) {"
    marker_at = text.find(marker)
    if marker_at < 0:
        raise ValueError(f"could not find Dozen extension table in {dozen_path}")
    open_brace = text.find("{", marker_at)
    close = text.find("};", open_brace)
    if open_brace < 0 or close < 0:
        raise ValueError(f"could not parse Dozen extension table in {dozen_path}")
    body = text[open_brace + 1:close]
    fields = set(re.findall(r"^\s*\.([A-Za-z0-9_]+)\s*=", body, re.MULTILINE))
    fields.update(
        re.findall(r"\bsupported_extensions\.([A-Za-z0-9_]+)\s*=\s*true\b", text)
    )
    return fields


def registry_requirements(extension: ET.Element, root: ET.Element) -> dict[str, Any]:
    commands: set[str] = set()
    types: set[str] = set()
    feature_structures: set[str] = set()
    feature_members: set[str] = set()
    for require in extension.findall("./require"):
        commands.update(
            item.get("name") for item in require.findall("./command") if item.get("name")
        )
        types.update(item.get("name") for item in require.findall("./type") if item.get("name"))
        for feature in require.findall("./feature"):
            if feature.get("struct"):
                feature_structures.add(feature.get("struct"))
            if feature.get("name"):
                feature_members.add(feature.get("name"))

    type_elements = {
        element.get("name"): element
        for element in root.findall("./types/type")
        if element.get("name")
    }
    pnext_structures = sorted(
        name for name in types
        if name in type_elements and type_elements[name].get("structextends")
    )
    feature_types = sorted(
        name for name in types
        if name.startswith("VkPhysicalDevice") and "Features" in name
    )
    property_types = sorted(
        name for name in types
        if name.startswith("VkPhysicalDevice") and "Properties" in name
    )
    command_elements = {
        element.get("name"): element
        for element in root.findall("./commands/command")
        if element.get("name")
    }
    aliases = {
        name: command_elements[name].get("alias")
        for name in sorted(commands)
        if name in command_elements and command_elements[name].get("alias")
    }
    return {
        "depends": extension.get("depends"),
        "requires": extension.get("requires"),
        "platform": extension.get("platform"),
        "promotedto": extension.get("promotedto"),
        "deprecatedby": extension.get("deprecatedby"),
        "required_commands": sorted(commands),
        "command_aliases": aliases,
        "required_types": sorted(types),
        "pnext_structures": pnext_structures,
        "feature_structures": sorted(feature_structures),
        "feature_members": sorted(feature_members),
        "feature_payload_types": feature_types,
        "property_payload_types": property_types,
    }


def build_work_items(requirements: dict[str, Any], candidate: dict[str, Any]) -> list[dict[str, Any]]:
    items = [
        {
            "id": "backend_capability_probe",
            "status": "todo",
            "required": True,
            "exit_criterion": "Cada bit reportado é respaldado por consulta D3D12 por adaptador ou caminho de software explicitamente compatível com a spec.",
        },
        {
            "id": "dependency_and_advertisement_gate",
            "status": "todo",
            "required": True,
            "exit_criterion": "Anunciar somente quando todas as dependências, regras de versão da API e condições de runtime forem satisfeitas.",
        },
    ]
    if requirements["feature_members"] or requirements["feature_structures"]:
        items.append({
            "id": "feature_queries_and_device_enablement",
            "status": "todo",
            "required": True,
            "exit_criterion": "Consultas de feature retornam valores verdadeiros e a criação do device rejeita bits solicitados sem suporte.",
        })
    if requirements["property_payload_types"]:
        items.append({
            "id": "property_queries",
            "status": "todo",
            "required": True,
            "exit_criterion": "Cada propriedade requerida vem de consulta D3D12 válida ou limite Vulkan documentado.",
        })
    if requirements["pnext_structures"]:
        items.append({
            "id": "pnext_and_object_semantics",
            "status": "todo",
            "required": True,
            "exit_criterion": "Cada estrutura de entrada/saída é consumida, validada e testada no caminho de API correspondente.",
        })
    if requirements["required_commands"]:
        items.append({
            "id": "commands_and_dispatch",
            "status": "todo",
            "required": True,
            "exit_criterion": "Cada comando requerido tem implementação Dozen ou implementação comum/alias comprovada com semântica de execução correta.",
        })
    shader_related = any("shader" in tag.lower() for tag in candidate.get("group_tags", []))
    if shader_related:
        items.append({
            "id": "shader_translation",
            "status": "todo",
            "required": True,
            "exit_criterion": "Operações SPIR-V/NIR e lowering DXIL necessários estão implementados e validados conforme a semântica da extensão.",
        })
    items.extend([
        {
            "id": "static_and_runtime_tests",
            "status": "todo",
            "required": True,
            "exit_criterion": "Testes de regressão focados passam; runtime Windows/D3D12 e Vulkan CTS aplicável passam antes de reivindicar conformidade.",
        },
    ])
    return items


def build_scaffold(backlog_path: Path, registry_path: Path, dozen_path: Path) -> dict[str, Any]:
    backlog = json.loads(backlog_path.read_text(encoding="utf-8"))
    root = ET.parse(registry_path).getroot()
    registry_extensions = {
        item.get("name"): item
        for item in root.findall("./extensions/extension")
        if item.get("name")
    }
    advertised_fields = extension_table_fields(dozen_path)
    candidates = backlog.get("pending_extensions", [])
    if len(candidates) != len({entry.get("field") for entry in candidates}):
        raise ValueError("backlog contains duplicate pending fields")

    extensions = []
    missing_registry_entries = []
    category_counts: Counter[str] = Counter()
    group_counts: Counter[str] = Counter()
    for candidate in candidates:
        field = candidate["field"]
        extension_name = candidate["extension"]
        category_counts[candidate.get("decision_category", "unclassified")] += 1
        group_counts.update(candidate.get("group_tags", []))
        entry = registry_extensions.get(extension_name)
        if entry is None:
            missing_registry_entries.append(extension_name)
            registry_data = {"registry_entry": False}
            work_items = build_work_items({
                "feature_members": [], "feature_structures": [],
                "property_payload_types": [], "pnext_structures": [],
                "required_commands": [],
            }, candidate)
        else:
            registry_data = {
                "registry_entry": True,
                **registry_requirements(entry, root),
            }
            work_items = build_work_items(registry_data, candidate)
        extensions.append({
            "field": field,
            "name": extension_name,
            "review_status": "rejected_not_advertised",
            "currently_advertised_in_dozen_source": field in advertised_fields,
            "decision_category": candidate.get("decision_category", "unclassified"),
            "group_tags": candidate.get("group_tags", []),
            "reason": candidate.get("decision_reason", ""),
            "registry": registry_data,
            "work_items": work_items,
        })

    return {
        "schema_version": 1,
        "mesa_version": backlog.get("mesa_version"),
        "scope": "Implementation scaffold for reviewed candidates only; not a runtime capability report.",
        "policy": {
            "pending_extensions_must_not_be_advertised": True,
            "common_runtime_or_generated_entrypoints_are_not_by_themselves_backend_support": True,
            "only_mark_implemented_after_semantic_mapping_and_runtime_validation": True,
        },
        "counts": {
            "reviewed_candidates": backlog.get("reviewed_candidate_count"),
            "implemented_in_review": len(backlog.get("implemented_fields", [])),
            "pending_scaffolded": len(extensions),
            "pending_advertised_in_source": sum(
                1 for item in extensions if item["currently_advertised_in_dozen_source"]
            ),
            "without_registry_entry": len(missing_registry_entries),
            "by_decision_category": dict(sorted(category_counts.items())),
            "by_group_tag": dict(sorted(group_counts.items())),
        },
        "inputs": {
            "backlog": display_path(backlog_path),
            "backlog_sha256": sha256(backlog_path),
            "registry": display_path(registry_path),
            "dozen_source": display_path(dozen_path),
        },
        "architecture_layers": ARCHITECTURE_LAYERS,
        "missing_registry_entries": sorted(missing_registry_entries),
        "extensions": extensions,
    }


def markdown(scaffold: dict[str, Any]) -> str:
    counts = scaffold["counts"]
    lines = [
        f"# Base de implementação Vulkan do Dozen — Mesa {scaffold['mesa_version']}",
        "",
        f"A base registra **{counts['pending_scaffolded']} extensões pendentes** sem anunciá-las. Ela converte os requisitos do `vk.xml` em contratos de trabalho verificáveis; não representa suporte runtime.",
        "",
        "## Camadas reutilizáveis",
        "",
    ]
    for layer in scaffold["architecture_layers"]:
        lines.append(f"- **{layer['layer']}**: {layer['purpose']}")
        if layer.get("warning"):
            lines.append(f"  - Limite: {layer['warning']}")
    lines.extend([
        "",
        "## Cobertura do scaffold",
        "",
        f"- Candidatas revisadas: {counts['reviewed_candidates']}",
        f"- Implementadas na matriz: {counts['implemented_in_review']}",
        f"- Pendentes com checklist por extensão: {counts['pending_scaffolded']}",
        f"- Pendentes que aparecem anunciadas na fonte: {counts['pending_advertised_in_source']}",
        f"- Extensões não encontradas no `vk.xml`: {counts['without_registry_entry']}",
        "",
        "## Bloqueios registrados",
        "",
    ])
    for category, count in counts["by_decision_category"].items():
        lines.append(f"- `{category}`: {count}")
    lines.extend([
        "",
        "## Regra para promover uma extensão",
        "",
        "Uma extensão só pode sair da pendência depois que probe de capacidade, feature/property, dependências, estruturas `pNext`, comandos, tradução D3D12/compilador e testes aplicáveis estiverem cobertos. Símbolos gerados ou handlers comuns, isoladamente, não provam que a semântica esteja implementada.",
        "",
        "A lista completa e os requisitos de cada extensão estão em `extension_support_scaffold.json`. O teste `test_extension_support_scaffold.py` falha se uma extensão pendente for anunciada sem atualizar a matriz e comprovar sua implementação.",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backlog", type=Path, default=DEFAULT_BACKLOG)
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    parser.add_argument("--dozen", type=Path, default=DEFAULT_DOZEN)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--markdown", type=Path, help="também grava um resumo Markdown")
    parser.add_argument("--check", action="store_true", help="falha se alguma extensão pendente estiver anunciada")
    parser.add_argument("--import-review", type=Path, help="importa a matriz revisada de candidatas para --backlog")
    args = parser.parse_args()
    try:
        if args.import_review:
            args.backlog.write_text(
                json.dumps(import_review(args.import_review), indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
        scaffold = build_scaffold(args.backlog, args.registry, args.dozen)
        if args.check and scaffold["counts"]["pending_advertised_in_source"]:
            raise ValueError("one or more pending extensions are advertised in dzn_device.c")
        if args.output == Path("-"):
            print(json.dumps(scaffold, indent=2, ensure_ascii=False))
        else:
            args.output.write_text(
                json.dumps(scaffold, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
            )
        if args.markdown:
            args.markdown.write_text(markdown(scaffold), encoding="utf-8")
        print(
            f"pending={scaffold['counts']['pending_scaffolded']} "
            f"advertised_pending={scaffold['counts']['pending_advertised_in_source']} "
            f"registry_missing={scaffold['counts']['without_registry_entry']}",
            file=sys.stderr,
        )
        return 0
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        print(f"extension support scaffold failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

