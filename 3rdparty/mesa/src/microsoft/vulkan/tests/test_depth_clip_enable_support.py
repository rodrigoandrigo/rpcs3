#!/usr/bin/env python3
"""Verify Dozen's static VK_EXT_depth_clip_enable implementation path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class DepthClipEnableSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_are_reported(self):
        self.assertRegex(self.device, r"\.EXT_depth_clip_enable\s*=\s*true\b")
        self.assertRegex(self.device, r"\.depthClipEnable\s*=\s*true\b")

    def test_registry_extension_has_feature_and_static_pipeline_state_only(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_depth_clip_enable']"
        )
        self.assertIsNotNone(ext)
        self.assertEqual(ext.get("depends"), "VK_KHR_get_physical_device_properties2,VK_VERSION_1_1")
        features = {
            (node.get("name"), node.get("struct"))
            for node in ext.findall("./require/feature")
        }
        self.assertEqual(
            features,
            {
                (
                    "depthClipEnable",
                    "VkPhysicalDeviceDepthClipEnableFeaturesEXT",
                )
            },
        )
        self.assertEqual(ext.findall("./require/command"), [])

    def test_rasterizer_pnext_overrides_the_depth_clamp_default(self):
        self.assertRegex(
            self.pipeline,
            r"vk_find_struct_const\(in_rast->pNext,\s*"
            r"PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT\)",
        )
        self.assertRegex(
            self.pipeline,
            r"depth_clip_info\s*\?\s*depth_clip_info->depthClipEnable\s*:\s*"
            r"!in_rast->depthClampEnable",
        )

    def test_both_d3d12_rasterizer_descriptor_paths_use_the_resolved_state(self):
        self.assertEqual(
            len(re.findall(r"desc->DepthClipEnable\s*=\s*depth_clip_enable\s*;", self.pipeline)),
            2,
        )

    def test_dynamic_eds3_command_is_not_claimed_by_this_extension(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_extended_dynamic_state3']"
        )
        self.assertIsNotNone(ext)
        dynamic = ext.find("./require/command[@name='vkCmdSetDepthClipEnableEXT']")
        self.assertIsNotNone(dynamic)
        self.assertNotRegex(
            self.device,
            r"\.EXT_extended_dynamic_state3\s*=\s*true\b",
        )


if __name__ == "__main__":
    unittest.main()

