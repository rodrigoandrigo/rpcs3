#!/usr/bin/env python3
"""Verify Dozen support for relaxed non-semantic SPIR-V forward references."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
SPIRV_PARSER = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
SPIRV_HEADER = MESA_ROOT / "src/compiler/spirv/spirv.h"
SPIRV_GRAMMAR = MESA_ROOT / "src/compiler/spirv/spirv.core.grammar.json"
SPIRV_TEST = MESA_ROOT / "src/compiler/spirv/tests/non_semantic.cpp"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class ShaderRelaxedExtendedInstructionSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.parser = SPIRV_PARSER.read_text(encoding="utf-8")
        cls.spirv_header = SPIRV_HEADER.read_text(encoding="utf-8")
        cls.grammar = SPIRV_GRAMMAR.read_text(encoding="utf-8")
        cls.spirv_test = SPIRV_TEST.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_device_advertises_extension_and_feature(self):
        self.assertRegex(self.device, r"DZN_API_VERSION\s+VK_MAKE_VERSION\(1,\s*2,")
        self.assertRegex(
            self.device,
            r"\.KHR_shader_relaxed_extended_instruction\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.shaderRelaxedExtendedInstruction\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.KHR_shader_non_semantic_info\s*=\s*true\b",
        )

    def test_registry_declares_feature_and_vulkan_11_dependency(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_KHR_shader_relaxed_extended_instruction']"
        )
        self.assertIsNotNone(extension)
        self.assertIn("VK_VERSION_1_1", extension.get("depends", ""))
        self.assertTrue(
            any(
                feature.get("name") == "shaderRelaxedExtendedInstruction"
                and feature.get("struct")
                == "VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR"
                for feature in extension.findall("./require/feature")
            )
        )

    def test_registry_maps_the_spirv_extensions_to_enabled_vulkan_extensions(self):
        relaxed = self.registry.find(
            "./spirvextensions/spirvextension[@name='SPV_KHR_relaxed_extended_instruction']"
        )
        non_semantic = self.registry.find(
            "./spirvextensions/spirvextension[@name='SPV_KHR_non_semantic_info']"
        )
        self.assertIsNotNone(relaxed)
        self.assertIsNotNone(non_semantic)
        self.assertTrue(
            any(
                enable.get("extension")
                == "VK_KHR_shader_relaxed_extended_instruction"
                for enable in relaxed.findall("./enable")
            )
        )
        self.assertTrue(
            any(
                enable.get("extension") == "VK_KHR_shader_non_semantic_info"
                for enable in non_semantic.findall("./enable")
            )
        )

    def test_parser_drops_forward_ref_instructions_without_resolving_ids(self):
        self.assertIn("SpvOpExtInstWithForwardRefsKHR = 4433", self.spirv_header)
        self.assertIn('"SPV_KHR_relaxed_extended_instruction"', self.grammar)
        self.assertIn("case SpvOpExtInstWithForwardRefsKHR:", self.parser)
        self.assertIn(
            "vtn_handle_non_semantic_instruction(b, w[4], w, count);", self.parser
        )
        self.assertIn(
            "Do not run optional debug handlers here: their operands may be IDs",
            self.parser,
        )
        self.assertIn("Ignore its operands", self.parser)
        self.assertIn("forward references", self.parser)

    def test_parser_regression_exercises_preamble_and_body_with_debug_enabled(self):
        self.assertIn("relaxed_extended_instruction_forward_refs", self.spirv_test)
        self.assertIn("spirv_options.debug_info = true;", self.spirv_test)
        self.assertIn("0x00061151", self.spirv_test)
        self.assertIn("0x000a1151", self.spirv_test)

    def test_dzn_pipeline_uses_the_shared_spirv_to_nir_parser(self):
        self.assertIn("vk_pipeline_shader_stage_to_nir(", self.pipeline)
        self.assertIn("dxil_spirv_nir_get_spirv_options()", self.pipeline)


if __name__ == "__main__":
    unittest.main()

