#!/usr/bin/env python3
"""Verify Dozen's conservative VK_KHR_format_feature_flags2 support."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", source)
    if match is None:
        raise AssertionError(f"function not found: {name}")
    start = match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos]
    raise AssertionError(f"unclosed function: {name}")


class FormatFeatureFlags2SupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_is_gated_by_properties2_core_promotion(self):
        self.assertRegex(self.device, r"\.KHR_format_feature_flags2\s*=\s*true\b")
        ext = self.registry.find(
            "./extensions/extension[@name='VK_KHR_format_feature_flags2']"
        )
        self.assertIsNotNone(ext)
        self.assertEqual(ext.get("promotedto"), "VK_VERSION_1_3")
        self.assertEqual(ext.get("nofeatures"), "true")
        self.assertIn("VK_KHR_get_physical_device_properties2", ext.get("depends"))
        self.assertEqual(ext.findall("./require/command"), [])
        self.assertEqual(ext.findall("./require/feature"), [])
        required_types = {node.get("name") for node in ext.findall("./require/type")}
        self.assertIn("VkFormatProperties3KHR", required_types)

    def test_properties3_widens_only_the_verified_legacy_feature_masks(self):
        helper = function_body(
            self.device, "dzn_physical_device_fill_format_properties3"
        )
        self.assertIn("VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3", helper)
        for field in (
            "linearTilingFeatures",
            "optimalTilingFeatures",
            "bufferFeatures",
        ):
            self.assertIn(
                f"properties3->{field} =", helper,
                f"VkFormatProperties3.{field} must be populated",
            )
            self.assertIn(f"formatProperties.{field}", helper)
        self.assertGreaterEqual(helper.count("(VkFormatFeatureFlags2)"), 3)
        self.assertIn("vk_debug_ignored_stype(ext->sType)", helper)
        self.assertNotIn("VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT", helper)
        self.assertNotIn("VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT", helper)
        self.assertNotIn("VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT", helper)

    def test_extended_properties_are_filled_for_known_and_unknown_dxgi_formats(self):
        query = function_body(
            self.device, "dzn_physical_device_get_format_properties"
        )
        self.assertEqual(
            query.count("dzn_physical_device_fill_format_properties3(properties);"),
            2,
        )
        unknown = query.index("if (dfmt_info.Format == DXGI_FORMAT_UNKNOWN)")
        early_fill = query.index("dzn_physical_device_fill_format_properties3(properties);", unknown)
        early_return = query.index("return;", unknown)
        self.assertLess(early_fill, early_return)
        self.assertGreater(
            query.rindex("dzn_physical_device_fill_format_properties3(properties);"),
            query.index("if (vk_format_is_depth_or_stencil(format))"),
        )


if __name__ == "__main__":
    unittest.main()

