#!/usr/bin/env python3
"""Attach Vulkan registry metadata to RADV-only extension-table field names.

This is a source inventory, not a runtime support or conformance test.
"""
from __future__ import annotations

import argparse
import json
import xml.etree.ElementTree as ET
from pathlib import Path

from extension_coverage_audit import build_inventory, default_source_paths

MESA_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def command_name(command: ET.Element) -> str | None:
    name = command.get("name")
    if name:
        return name
    node = command.find("./proto/name")
    return node.text if node is not None else None


def build_registry_inventory(radv: Path, dozen: Path, registry: Path) -> dict[str, object]:
    source_inventory = build_inventory(radv, dozen)
    tree = ET.parse(registry)
    root = tree.getroot()
    extensions = {
        extension.get("name"): extension
        for extension in root.findall("./extensions/extension")
        if extension.get("name")
    }
    commands = {}
    for command in root.findall("./commands/command"):
        name = command_name(command)
        if name:
            commands[name] = command

    missing = source_inventory["radv_missing_from_dozen"]
    assert isinstance(missing, list)
    entries = {}
    unresolved = []
    promoted = {}
    for field in missing:
        extension_name = f"VK_{field}"
        extension = extensions.get(extension_name)
        if extension is None:
            unresolved.append(field)
            entries[field] = {"registry_entry": False}
            continue

        required_commands = set()
        required_types = set()
        feature_structures = set()
        feature_members = set()
        for require in extension.findall("./require"):
            required_commands.update(
                node.get("name") for node in require.findall("./command") if node.get("name")
            )
            required_types.update(
                node.get("name") for node in require.findall("./type") if node.get("name")
            )
            feature_structures.update(
                node.get("struct") for node in require.findall("./feature") if node.get("struct")
            )
            feature_members.update(
                node.get("name") for node in require.findall("./feature") if node.get("name")
            )

        aliases = {}
        for name in sorted(required_commands):
            node = commands.get(name)
            if node is not None and node.get("alias"):
                aliases[name] = node.get("alias")

        attrs = extension.attrib
        promoted_to = attrs.get("promotedto")
        if promoted_to:
            promoted[field] = promoted_to
        entries[field] = {
            "registry_entry": True,
            "name": extension_name,
            "type": attrs.get("type"),
            "supported": attrs.get("supported"),
            "depends": attrs.get("depends"),
            "requires": attrs.get("requires"),
            "platform": attrs.get("platform"),
            "promotedto": promoted_to,
            "deprecatedby": attrs.get("deprecatedby"),
            "obsoletedby": attrs.get("obsoletedby"),
            "required_commands": sorted(required_commands),
            "command_aliases": aliases,
            "required_types": sorted(required_types),
            "feature_structures": sorted(feature_structures),
            "feature_members": sorted(feature_members),
        }

    return {
        "mesa_version": (MESA_ROOT / "VERSION").read_text(encoding="utf-8").strip(),
        "registry_source": str(registry),
        "radv_source": str(radv),
        "dozen_source": str(dozen),
        "static_field_counts": {
            key: source_inventory[key]
            for key in (
                "radv_initializer_count",
                "dozen_initializer_count",
                "dozen_later_true_assignment_count",
                "common_count",
                "radv_missing_from_dozen_count",
                "dozen_only_count",
            )
        },
        "scope_warning": (
            "Registry metadata describes Vulkan requirements only; it does not prove Dozen runtime support, "
            "D3D12 capability, advertised extensions, or Vulkan CTS conformance."
        ),
        "missing_field_registry_entries": entries,
        "fields_without_registry_entry": unresolved,
        "missing_fields_promoted_to_core": promoted,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_radv, default_dozen = default_source_paths()
    parser.add_argument("--radv", type=Path, default=default_radv)
    parser.add_argument("--dozen", type=Path, default=default_dozen)
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    inventory = build_registry_inventory(args.radv, args.dozen, args.registry)
    text = json.dumps(inventory, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    print(
        f"registry entries={len(inventory['missing_field_registry_entries']) - len(inventory['fields_without_registry_entry'])}; "
        f"without entry={len(inventory['fields_without_registry_entry'])}; "
        f"promoted missing fields={len(inventory['missing_fields_promoted_to_core'])}",
        file=__import__("sys").stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

