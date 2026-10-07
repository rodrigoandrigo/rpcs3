#!/usr/bin/env python3
"""Verify Dozen can ignore Vulkan non-semantic SPIR-V instructions."""
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


class ShaderNonSemanticInfoSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.parser = SPIRV_PARSER.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_is_advertised_for_the_vulkan_12_device(self):
        self.assertRegex(cls := self.device, r"DZN_API_VERSION\s+VK_MAKE_VERSION\(1,\s*2,")
        self.assertRegex(cls, r"\.KHR_shader_non_semantic_info\s*=\s*true\b")

    def test_registry_has_no_commands_features_or_d3d12_gate(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_KHR_shader_non_semantic_info']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("nofeatures"), "true")
        self.assertIsNone(extension.get("depends"))
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_3")
        required = extension.findall("./require")
        self.assertFalse(any(req.findall("./command") or req.findall("./feature") for req in required))

    def test_common_parser_accepts_non_semantic_imports_as_noop(self):
        self.assertIn('strstr(ext, "NonSemantic.") == ext', self.parser)
        self.assertIn("val->ext_handler = vtn_handle_non_semantic_instruction;", self.parser)
        self.assertRegex(
            self.parser,
            r"vtn_handle_non_semantic_instruction\([^)]*\)\s*\{\s*/\* Do nothing\. \*/\s*return true;",
        )

    def test_dzn_pipeline_uses_the_shared_spirv_to_nir_path(self):
        self.assertIn("dxil_spirv_nir_get_spirv_options()", self.pipeline)
        self.assertIn("vk_pipeline_shader_stage_to_nir(", self.pipeline)

    def test_registry_enables_spirv_extension_through_this_vulkan_extension(self):
        spirv_extension = self.registry.find(
            "./spirvextensions/spirvextension[@name='SPV_KHR_non_semantic_info']"
        )
        self.assertIsNotNone(spirv_extension)
        self.assertTrue(
            any(node.get("extension") == "VK_KHR_shader_non_semantic_info"
                for node in spirv_extension.findall("./enable"))
        )


if __name__ == "__main__":
    unittest.main()

