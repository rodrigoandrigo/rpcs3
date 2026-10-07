#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify Dozen's VK_EXT_image_sliced_view_of_3d implementation path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_IMAGE = TEST_DIR.parent / "dzn_image.c"
VK_IMAGE = MESA_ROOT / "src/vulkan/runtime/vk_image.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    match = re.search(rf"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\n{name}\(", source)
    if not match:
        raise AssertionError(f"function definition not found: {name}")
    brace = source.index("{", match.end())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {name}")


class ImageSlicedViewOf3DSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.image = DZN_IMAGE.read_text(encoding="utf-8")
        cls.common_image = VK_IMAGE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_are_reported(self):
        self.assertRegex(
            self.device,
            r"\.EXT_image_sliced_view_of_3d\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.imageSlicedViewOf3D\s*=\s*true\b",
        )

    def test_registry_feature_dependency_and_view_structure_match(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_image_sliced_view_of_3d']"
        )
        self.assertIsNotNone(ext)
        self.assertIn("VK_VERSION_1_1", ext.get("depends", ""))
        self.assertEqual(ext.findall("./require/command"), [])
        self.assertEqual(
            {node.get("name") for node in ext.findall("./require/type")},
            {
                "VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT",
                "VkImageViewSlicedCreateInfoEXT",
            },
        )
        self.assertEqual(
            {
                (node.get("name"), node.get("struct"))
                for node in ext.findall("./require/feature")
            },
            {
                (
                    "imageSlicedViewOf3D",
                    "VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT",
                )
            },
        )

    def test_common_runtime_normalizes_offset_and_remaining_slice_count(self):
        body = function_body(self.common_image, "vk_image_view_init")
        self.assertRegex(
            body,
            r"vk_find_struct_const\(pCreateInfo,\s*"
            r"IMAGE_VIEW_SLICED_CREATE_INFO_EXT\)",
        )
        self.assertIn(
            "image_view->storage.z_slice_offset = sliced_info->sliceOffset",
            body,
        )
        self.assertIn(
            "image_view->storage.z_slice_count = total - image_view->storage.z_slice_offset",
            body,
        )

    def test_3d_storage_uav_uses_runtime_slice_range(self):
        body = function_body(self.image, "dzn_image_view_prepare_uav_desc")
        self.assertRegex(
            body,
            r"Texture3D\.FirstWSlice\s*=\s*iview->vk\.storage\.z_slice_offset",
        )
        self.assertRegex(
            body,
            r"Texture3D\.WSize\s*=\s*iview->vk\.storage\.z_slice_count",
        )
        self.assertIn("D3D12_UAV_DIMENSION_TEXTURE3D", body)

    def test_slice_range_is_not_applied_to_srv_or_attachment_views(self):
        srv = function_body(self.image, "dzn_image_view_prepare_srv_desc")
        rtv = function_body(self.image, "dzn_image_view_prepare_rtv_desc")
        self.assertNotIn("z_slice_offset", srv)
        self.assertNotIn("z_slice_count", srv)
        self.assertNotIn("z_slice_offset", rtv)
        self.assertNotIn("z_slice_count", rtv)


if __name__ == "__main__":
    unittest.main()

