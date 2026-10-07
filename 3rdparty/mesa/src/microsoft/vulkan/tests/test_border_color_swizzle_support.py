#!/usr/bin/env python3
"""Verify Dozen's explicit VK_EXT_border_color_swizzle path."""
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


class BorderColorSwizzleSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_and_feature_are_gated_by_custom_border_color_support(self):
        extension_line = next(
            line for line in self.device.splitlines()
            if ".EXT_border_color_swizzle" in line
        )
        self.assertIn("= dzn_custom_border_color_supported(pdev)", extension_line)
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertIn(
            ".borderColorSwizzle                 = dzn_custom_border_color_supported(pdev)",
            features,
        )
        self.assertIn(".borderColorSwizzleFromImage        = false", features)

        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_border_color_swizzle']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("depends"), "VK_EXT_custom_border_color")
        self.assertIsNotNone(
            extension.find("./require/type[@name='VkSamplerBorderColorComponentMappingCreateInfoEXT']")
        )
        self.assertIsNotNone(
            extension.find("./require/type[@name='VkPhysicalDeviceBorderColorSwizzleFeaturesEXT']")
        )

    def test_sampler_consumes_the_mapping_and_requires_the_enabled_feature(self):
        sampler = function_body(self.device, "dzn_sampler_create")
        self.assertIn("SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT", sampler)
        self.assertIn("device->vk.enabled_features.borderColorSwizzle", sampler)
        self.assertIn("dzn_sampler_apply_border_color_mapping", sampler)
        self.assertIn("reads_border_color && border_mapping", sampler)

    def test_mapping_helper_encodes_srgb_before_component_swizzle(self):
        helper = function_body(self.device, "dzn_sampler_apply_border_color_mapping")
        encode = helper.index("util_format_linear_to_srgb_float")
        swizzle = helper.index("vk_swizzle_color_value")
        self.assertLess(encode, swizzle)
        self.assertIn("mapping->components", helper)
        self.assertIn("sampler->static_border_color = (D3D12_STATIC_BORDER_COLOR)-1", helper)
        self.assertIn("sampler->desc.FloatBorderColor", helper)
        self.assertIn("sampler->desc.UintBorderColor", helper)


if __name__ == "__main__":
    unittest.main()

