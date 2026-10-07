#!/usr/bin/env python3
"""Verify Dozen's feature/query integration for shader demote semantics."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
SPIRV_PARSER = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
DXIL_BACKEND = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
FEATURES_GEN = MESA_ROOT / "src/vulkan/util/vk_physical_device_features_gen.py"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class ShaderDemoteToHelperInvocationSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.parser = SPIRV_PARSER.read_text(encoding="utf-8")
        cls.dxil = DXIL_BACKEND.read_text(encoding="utf-8")
        cls.features_gen = FEATURES_GEN.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_are_reported(self):
        self.assertRegex(self.device, r"\.EXT_shader_demote_to_helper_invocation\s*=\s*true\b")
        self.assertRegex(self.device, r"\.shaderDemoteToHelperInvocation\s*=\s*true\b")
        self.assertNotRegex(self.device, r"\.shaderDemoteToHelperInvocation\s*=\s*false\b")

    def test_vulkan_feature_generator_maps_core_feature_and_xml_alias(self):
        self.assertRegex(
            self.features_gen,
            r"\['Vulkan13Features',\s*'ShaderDemoteToHelperInvocationFeatures'\].*\['shaderDemoteToHelperInvocation'\]",
        )
        alias = self.registry.find(
            "./types/type[@name='VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT']"
        )
        self.assertIsNotNone(alias)
        self.assertEqual(
            alias.get("alias"),
            "VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures",
        )

    def test_registry_has_feature_alias_and_no_extension_commands(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_shader_demote_to_helper_invocation']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_3")
        self.assertFalse(any(req.findall("./command") for req in extension.findall("./require")))
        self.assertRegex(self.device, r"\.EXT_shader_demote_to_helper_invocation\s*=\s*true\b")

    def test_spirv_demote_is_lowered_to_nir(self):
        self.assertRegex(
            self.parser,
            r"case SpvOpDemoteToHelperInvocation:\s*\{\s*nir_demote\(&b->nb\);",
        )
        self.assertIn("DemoteToHelperInvocation", self.parser)

    def test_dxil_uses_discard_with_demote_semantics(self):
        self.assertRegex(self.dxil, r"\.discard_is_demote\s*=\s*true")
        self.assertIn("DXIL_INTR_DISCARD", self.dxil)
        self.assertIn('dx.op.discard', self.dxil)


if __name__ == "__main__":
    unittest.main()

