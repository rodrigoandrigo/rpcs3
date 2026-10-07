#!/usr/bin/env python3
"""Verify Dozen's conservative VK_EXT_texel_buffer_alignment support."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]


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


class TexelBufferAlignmentSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.image = (DZN_DIR / "dzn_image.c").read_text(encoding="utf-8")
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_is_advertised_and_feature_is_reported(self):
        self.assertRegex(self.device, r"\.EXT_texel_buffer_alignment\s*=\s*true\b")
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertRegex(features, r"\.texelBufferAlignment\s*=\s*true\b")

        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_texel_buffer_alignment']"
        )
        self.assertIsNotNone(extension)
        self.assertIsNotNone(
            extension.find("./require/feature[@name='texelBufferAlignment']")
        )
        self.assertIsNotNone(
            extension.find(
                "./require/type[@name='VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT']"
            )
        )
        self.assertIsNotNone(
            extension.find(
                "./require/type[@name='VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT']"
            )
        )

    def test_reported_alignment_is_power_of_two_and_no_stricter_than_core(self):
        self.assertRegex(
            self.device,
            r"#define DZN_TEXEL_BUFFER_OFFSET_ALIGNMENT 32\b",
        )
        properties = function_body(self.device, "dzn_physical_device_get_properties")
        self.assertRegex(
            properties,
            r"\.minTexelBufferOffsetAlignment\s*=\s*DZN_TEXEL_BUFFER_OFFSET_ALIGNMENT",
        )
        for field in (
            "storageTexelBufferOffsetAlignmentBytes",
            "uniformTexelBufferOffsetAlignmentBytes",
        ):
            self.assertRegex(
                properties,
                rf"\.{field}\s*=\s*DZN_TEXEL_BUFFER_OFFSET_ALIGNMENT",
            )
        for field in (
            "storageTexelBufferOffsetSingleTexelAlignment",
            "uniformTexelBufferOffsetSingleTexelAlignment",
        ):
            self.assertRegex(properties, rf"\.{field}\s*=\s*false")

    def test_buffer_views_translate_aligned_byte_offsets_to_texel_indices(self):
        create = function_body(self.image, "dzn_buffer_view_create")
        self.assertIn("pCreateInfo->offset / blksz", create)
        self.assertIn("size / blksz", create)
        self.assertIn("VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT", create)
        self.assertIn("VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT", create)


if __name__ == "__main__":
    unittest.main()

