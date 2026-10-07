#!/usr/bin/env python3
"""Verify VK_GOOGLE_user_type uses the existing metadata-only SPIR-V path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
SPIRV_PARSER = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class GoogleUserTypeSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.parser = SPIRV_PARSER.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_is_advertised(self):
        self.assertRegex(self.device, r"\.GOOGLE_user_type\s*=\s*true\b")
        self.assertRegex(self.device, r"\.GOOGLE_decorate_string\s*=\s*true\b")

    def test_registry_has_no_commands_or_feature_structures(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_GOOGLE_user_type']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("nofeatures"), "true")
        self.assertIsNone(extension.get("depends"))
        self.assertFalse(any(
            req.findall("./command") or req.findall("./feature")
            for req in extension.findall("./require")
        ))

    def test_spirv_extension_is_enabled_by_the_vulkan_extension(self):
        spirv_extension = self.registry.find(
            "./spirvextensions/spirvextension[@name='SPV_GOOGLE_user_type']"
        )
        self.assertIsNotNone(spirv_extension)
        self.assertTrue(any(
            node.get("extension") == "VK_GOOGLE_user_type"
            for node in spirv_extension.findall("./enable")
        ))

    def test_parser_accepts_string_decorations_and_ignores_user_type(self):
        self.assertIn("SpvOpDecorateString", self.parser)
        self.assertIn("SpvOpMemberDecorateString", self.parser)
        self.assertRegex(
            self.parser,
            r"case SpvDecorationUserTypeGOOGLE:\s*/\* User semantic decorations can safely be ignored by the driver\. \*/\s*break;",
        )

    def test_dozen_pipeline_uses_shared_spirv_to_nir_path(self):
        self.assertIn("vk_pipeline_shader_stage_to_nir(", self.pipeline)
        self.assertIn("dxil_spirv_nir_get_spirv_options()", self.pipeline)


if __name__ == "__main__":
    unittest.main()

