#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_image_view_min_lod support path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]


class ImageViewMinLodSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.image = (DZN_DIR / "dzn_image.c").read_text(encoding="utf-8")
        cls.descriptor_set = (DZN_DIR / "dzn_descriptor_set.c").read_text(
            encoding="utf-8"
        )
        cls.runtime_image = (MESA_ROOT / "src/vulkan/runtime/vk_image.c").read_text(
            encoding="utf-8"
        )
        cls.runtime_header = (MESA_ROOT / "src/vulkan/runtime/vk_image.h").read_text(
            encoding="utf-8"
        )
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_and_feature_are_advertised(self):
        self.assertRegex(
            self.device,
            r"\.EXT_image_view_min_lod\s*=\s*true\b",
        )
        self.assertRegex(self.device, r"\.minLod\s*=\s*true\b")
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_image_view_min_lod']"
        )
        self.assertIsNotNone(ext)
        self.assertIsNotNone(
            ext.find("./require/feature[@name='minLod']")
        )
        self.assertIsNotNone(
            ext.find("./require/type[@name='VkImageViewMinLodCreateInfoEXT']")
        )

    def test_common_runtime_stores_the_image_view_min_lod(self):
        self.assertIn("IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT", self.runtime_image)
        self.assertRegex(
            self.runtime_image,
            r"image_view->min_lod\s*=\s*min_lod_info\s*\?\s*min_lod_info->minLod\s*:\s*0\.0f",
        )
        self.assertIn("float min_lod;", self.runtime_header)

    def test_every_srv_dimension_uses_the_view_min_lod(self):
        assignments = re.findall(
            r"iview->srv_desc\.[A-Za-z0-9_]+\.ResourceMinLODClamp\s*=\s*iview->srv_min_lod_clamp\s*;",
            self.image,
        )
        self.assertEqual(len(assignments), 7)
        self.assertEqual(
            len(re.findall(r"\.MostDetailedMip\s*=\s*0\s*;", self.image)),
            7,
        )
        self.assertEqual(
            len(re.findall(r"\.MipLevels\s*\+=\s*iview->vk\.base_mip_level\s*;", self.image)),
            7,
        )

    def test_base_mip_is_folded_into_clamp_without_double_setting_d3d12_fields(self):
        self.assertIn(
            "if (!iview->vk.base.device->enabled_features.minLod)", self.image
        )
        self.assertIn(
            "MAX2((float)iview->vk.base_mip_level, iview->vk.min_lod)",
            self.image,
        )
        helper = self.image.split(
            "dzn_image_view_apply_min_lod_clamp(struct dzn_image_view *iview)", 1
        )[1].split(
            "\nstatic void\ndzn_image_view_prepare_srv_desc", 1
        )[0]
        self.assertEqual(helper.count("MostDetailedMip = 0;"), 7)
        self.assertEqual(helper.count("MipLevels += iview->vk.base_mip_level;"), 7)
        self.assertIn("iview->srv_min_lod_clamp =", helper)
        self.assertIn("max(baseMipLevel, minLod)", helper)

    def test_cube_as_array_descriptor_preserves_min_lod(self):
        self.assertRegex(
            self.descriptor_set,
            r"srv_desc\.Texture2DArray\.ResourceMinLODClamp\s*=\s*iview->srv_min_lod_clamp\s*;",
        )


if __name__ == "__main__":
    unittest.main()

