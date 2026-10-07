#!/usr/bin/env python3
"""Verify Dozen's incremental-present advertisement and shared WSI path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"
WSI_COMMON = MESA_ROOT / "src/vulkan/wsi/wsi_common.c"
WSI_WIN32 = MESA_ROOT / "src/vulkan/wsi/wsi_common_win32.cpp"
DZN_WSI = TEST_DIR.parent / "dzn_wsi.c"


class IncrementalPresentSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.common = WSI_COMMON.read_text(encoding="utf-8")
        cls.win32 = WSI_WIN32.read_text(encoding="utf-8")
        cls.dzn_wsi = DZN_WSI.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_swapchain_share_the_wsi_build_gate(self):
        start = self.device.index("dzn_physical_device_get_extensions(")
        end = self.device.index("\n}", start)
        table = self.device[start:end]
        gate = re.search(r"#ifdef DZN_USE_WSI_PLATFORM(.*?)#endif", table, re.S)
        self.assertIsNotNone(gate)
        self.assertRegex(gate.group(1), r"\.KHR_swapchain\s*=\s*true")
        self.assertRegex(gate.group(1), r"\.KHR_incremental_present\s*=\s*true")

    def test_registry_requires_swapchain_and_has_no_feature_or_command(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_KHR_incremental_present']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("depends"), "VK_KHR_swapchain")
        self.assertEqual(extension.get("nofeatures"), "true")
        self.assertEqual(extension.get("number"), "85")
        required_types = {
            node.get("name") for node in extension.findall("./require/type")
        }
        self.assertIn("VkPresentRegionsKHR", required_types)
        self.assertFalse(extension.findall("./require/command"))

    def test_common_queue_present_routes_region_for_each_swapchain(self):
        self.assertIn(
            "vk_find_struct_const(pPresentInfo->pNext, PRESENT_REGIONS_KHR)",
            self.common,
        )
        self.assertIn("region = &regions->pRegions[i]", self.common)
        self.assertIn("swapchain->queue_present(swapchain, image_index", self.common)
        self.assertIn("image_signal_infos[i].present_id,\n                                            region)", self.common)

    def test_win32_backend_maps_rectangles_to_dxgi_present1(self):
        start = self.win32.index("wsi_win32_queue_present_dxgi(")
        end = self.win32.index("\n}", start)
        body = self.win32[start:end]
        self.assertIn("damage->pRectangles[r].offset.x", body)
        self.assertIn("DXGI_PRESENT_PARAMETERS params", body)
        self.assertIn("chain->dxgi->Present1(sync_interval, present_flags, &params)", body)

    def test_dozen_initializes_the_shared_wsi_device(self):
        self.assertIn("dzn_wsi_init(struct dzn_physical_device *physical_device)", self.dzn_wsi)
        self.assertIn("physical_device->vk.wsi_device = &physical_device->wsi_device", self.dzn_wsi)


if __name__ == "__main__":
    unittest.main()

