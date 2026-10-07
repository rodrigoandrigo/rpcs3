#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_tooling_info path through the Mesa common runtime."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"
PHYSICAL_DEVICE_RUNTIME = MESA_ROOT / "src/vulkan/runtime/vk_physical_device.c"


class ToolingInfoSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.runtime = PHYSICAL_DEVICE_RUNTIME.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_device_extension_is_advertised(self):
        self.assertRegex(self.device, r"\.EXT_tooling_info\s*=\s*true\b")

    def test_extension_has_no_hardware_feature_or_dependency_gate(self):
        extension = self.registry.find("./extensions/extension[@name='VK_EXT_tooling_info']")
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("nofeatures"), "true")
        self.assertIsNone(extension.get("depends"))
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_3")

    def test_ext_query_aliases_the_core_command_and_dzn_installs_common_entrypoints(self):
        command = self.registry.find(
            "./commands/command[@name='vkGetPhysicalDeviceToolPropertiesEXT']"
        )
        self.assertIsNotNone(command)
        self.assertEqual(command.get("alias"), "vkGetPhysicalDeviceToolProperties")
        self.assertIn("vk_common_physical_device_entrypoints", self.runtime)
        self.assertIn("vk_physical_device_init(&pdev->vk, instance", self.device)
        for entrypoint in ("GetPhysicalDeviceToolProperties", "GetPhysicalDeviceToolPropertiesEXT"):
            self.assertRegex(
                self.device,
                rf"dzn_{entrypoint}\([^)]*\)\s*\{{\s*return vk_common_GetPhysicalDeviceToolProperties\(",
            )

    def test_common_query_returns_a_valid_empty_tool_list(self):
        start = self.runtime.index("vk_common_GetPhysicalDeviceToolProperties(")
        end = self.runtime.index("VkDeviceSize VKAPI_CALL", start)
        body = self.runtime[start:end]
        self.assertIn("VK_OUTARRAY_MAKE_TYPED(VkPhysicalDeviceToolProperties", body)
        self.assertIn("return vk_outarray_status(&out);", body)
        self.assertNotIn("vk_outarray_append", body)


if __name__ == "__main__":
    unittest.main()

