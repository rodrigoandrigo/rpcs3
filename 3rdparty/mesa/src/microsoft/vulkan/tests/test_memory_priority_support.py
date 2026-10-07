#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_memory_priority allocation path."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]


def function_body(source, name):
    marker = "\n" + name + "("
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"function not found: {name}")
    brace = source.find("{", start + len(marker))
    if brace < 0:
        raise AssertionError(f"function body not found: {name}")
    depth = 1
    for pos in range(brace + 1, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:pos]
    raise AssertionError(f"unclosed function: {name}")


class MemoryPrioritySupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_and_feature_are_reported(self):
        extension_line = next(
            line for line in self.device.splitlines() if ".EXT_memory_priority" in line
        )
        self.assertTrue(extension_line.strip().endswith("= true,"))
        features = function_body(self.device, "dzn_physical_device_get_features")
        feature_line = next(
            line for line in features.splitlines() if ".memoryPriority" in line
        )
        self.assertTrue(feature_line.strip().endswith("= true,"))
        extension = self.registry.find("./extensions/extension[@name='VK_EXT_memory_priority']")
        self.assertIsNotNone(extension)
        self.assertIsNotNone(extension.find("./require/feature[@name='memoryPriority']"))
        self.assertIsNotNone(
            extension.find("./require/type[@name='VkMemoryPriorityAllocateInfoEXT']")
        )

    def test_allocation_pnext_is_consumed_and_applied_to_pageable_memory(self):
        allocation = function_body(self.device, "dzn_device_memory_create")
        self.assertIn("VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT", allocation)
        self.assertIn("VkMemoryPriorityAllocateInfoEXT", allocation)
        self.assertIn("dzn_device_memory_set_priority(device, mem, memory_priority)", allocation)
        apply_priority = function_body(self.device, "dzn_device_memory_set_priority")
        self.assertIn("mem->heap", apply_priority)
        self.assertIn("mem->dedicated_res", apply_priority)
        self.assertIn("ID3D12Device4_SetResidencyPriority", apply_priority)
        self.assertIn("(double)priority * UINT32_MAX", apply_priority)

    def test_priority_is_only_set_when_the_allocation_pnext_contains_the_struct(self):
        allocation = function_body(self.device, "dzn_device_memory_create")
        self.assertIn("bool has_memory_priority = false", allocation)
        self.assertIn("if (has_memory_priority)", allocation)
        self.assertIn(
            "if (has_memory_priority)\n      dzn_device_memory_set_priority",
            allocation,
        )
        self.assertIn("float memory_priority = 0.5f", allocation)


if __name__ == "__main__":
    unittest.main()

