#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify conservative VK_EXT_global_priority_query support in Dozen."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    match = re.search(
        r"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\n" + re.escape(name) + r"\s*\(",
        source,
    )
    if match is None:
        raise AssertionError(f"function not found: {name}")
    brace = source.index("{", match.end())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unclosed function: {name}")


class GlobalPriorityQuerySupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_base_and_query_extensions_are_advertised_with_feature(self):
        self.assertRegex(self.device, r"\.EXT_global_priority\s*=\s*true\b")
        self.assertRegex(self.device, r"\.EXT_global_priority_query\s*=\s*true\b")
        self.assertRegex(self.device, r"\.globalPriorityQuery\s*=\s*true\b")

    def test_query_extension_depends_on_the_implemented_base_extension(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_global_priority_query']"
        )
        self.assertIsNotNone(extension)
        self.assertIn("VK_EXT_global_priority", extension.get("depends", ""))
        self.assertEqual(extension.findall("./require/command"), [])

    def test_each_queue_family_reports_only_the_supported_default_priority(self):
        body = function_body(
            self.device, "dzn_GetPhysicalDeviceQueueFamilyProperties2"
        )
        self.assertIn(
            "VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES", body
        )
        self.assertIn("vk_foreach_struct(ext, p->pNext)", body)
        self.assertIn("priority_props->priorityCount = 1", body)
        self.assertIn(
            "priority_props->priorities[0] = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM",
            body,
        )
        self.assertNotIn("VK_QUEUE_GLOBAL_PRIORITY_HIGH", body)
        self.assertNotIn("VK_QUEUE_GLOBAL_PRIORITY_REALTIME", body)

    def test_unreported_global_priorities_fail_before_queue_creation(self):
        body = function_body(self.device, "dzn_queue_init")
        validation = body.index(
            "global_priority_info->globalPriority != VK_QUEUE_GLOBAL_PRIORITY_MEDIUM"
        )
        queue_init = body.index("vk_queue_init(")
        self.assertLess(validation, queue_init)
        self.assertIn("VK_ERROR_INITIALIZATION_FAILED", body)
        self.assertNotIn("VK_ERROR_NOT_PERMITTED_EXT", body)

    def test_medium_global_priority_overrides_local_queue_priority(self):
        body = function_body(self.device, "dzn_queue_init")
        self.assertIn("global_priority_info ? D3D12_COMMAND_QUEUE_PRIORITY_NORMAL", body)
        self.assertIn("priority_in > 0.5f ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH", body)

    def test_registry_payloads_match_the_implemented_query_path(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_global_priority_query']"
        )
        types = {
            item.get("name")
            for require in extension.findall("./require")
            for item in require.findall("./type")
        }
        self.assertIn("VkPhysicalDeviceGlobalPriorityQueryFeaturesEXT", types)
        self.assertIn("VkQueueFamilyGlobalPriorityPropertiesEXT", types)


if __name__ == "__main__":
    unittest.main()

