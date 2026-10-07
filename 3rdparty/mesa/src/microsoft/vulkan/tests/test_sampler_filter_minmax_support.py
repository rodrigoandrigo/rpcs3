#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_sampler_filter_minmax path and D3D12 capability gate."""
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


class SamplerFilterMinmaxSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.util = (DZN_DIR / "dzn_util.c").read_text(encoding="utf-8")
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_feature_and_capability_share_the_12_0_gate(self):
        gate = function_body(self.device, "dzn_sampler_filter_minmax_supported")
        self.assertIn("pdev->feature_level >= D3D_FEATURE_LEVEL_12_0", gate)
        extensions = function_body(self.device, "dzn_physical_device_get_extensions")
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertRegex(
            extensions,
            r"\.EXT_sampler_filter_minmax\s*=\s*dzn_sampler_filter_minmax_supported\(pdev\)",
        )
        self.assertRegex(
            features,
            r"\.samplerFilterMinmax\s*=\s*dzn_sampler_filter_minmax_supported\(pdev\)",
        )

    def test_minmax_format_bit_is_gated_and_requires_sampled_image(self):
        get_formats = function_body(self.device, "dzn_physical_device_get_format_properties")
        self.assertIn("dzn_sampler_filter_minmax_supported(pdev)", get_formats)
        self.assertIn("D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE", get_formats)
        self.assertIn("VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT", get_formats)
        self.assertIn("VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT", get_formats)

    def test_sampler_pnext_is_consumed_and_invalid_paths_are_rejected(self):
        create = function_body(self.device, "dzn_sampler_create")
        self.assertIn("SAMPLER_REDUCTION_MODE_CREATE_INFO", create)
        self.assertIn("device->vk.enabled_features.samplerFilterMinmax", create)
        self.assertIn("VK_ERROR_FEATURE_NOT_PRESENT", create)
        self.assertIn("pCreateInfo->compareEnable", create)
        self.assertIn("dzn_translate_sampler_filter(pdev, pCreateInfo)", create)

    def test_all_vulkan_reduction_modes_map_to_d3d12(self):
        translate = function_body(self.util, "dzn_translate_sampler_filter")
        expected = {
            "VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE": "D3D12_FILTER_REDUCTION_TYPE_STANDARD",
            "VK_SAMPLER_REDUCTION_MODE_MIN": "D3D12_FILTER_REDUCTION_TYPE_MINIMUM",
            "VK_SAMPLER_REDUCTION_MODE_MAX": "D3D12_FILTER_REDUCTION_TYPE_MAXIMUM",
        }
        for vk_mode, d3d_mode in expected.items():
            with self.subTest(mode=vk_mode):
                self.assertIn(vk_mode, translate)
                self.assertIn(d3d_mode, translate)
        self.assertIn("D3D12_FILTER_REDUCTION_TYPE_COMPARISON", translate)

    def test_registry_declares_struct_alias_and_extension(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_sampler_filter_minmax']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_2")
        self.assertIsNotNone(
            self.registry.find(
                "./types/type[@name='VkSamplerReductionModeCreateInfoEXT']"
            )
        )
        self.assertIsNotNone(
            self.registry.find(
                "./types/type[@name='VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT']"
            )
        )


if __name__ == "__main__":
    unittest.main()

