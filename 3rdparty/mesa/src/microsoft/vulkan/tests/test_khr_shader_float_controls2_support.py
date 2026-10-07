#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify Vulkan and shader-compiler support for VK_KHR_shader_float_controls2."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
SPIRV_TO_NIR = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
VTN_ALU = MESA_ROOT / "src/compiler/spirv/vtn_alu.c"
SPIRV_TO_DXIL = MESA_ROOT / "src/microsoft/spirv_to_dxil/spirv_to_dxil.c"
DXIL_NIR = MESA_ROOT / "src/microsoft/spirv_to_dxil/dxil_spirv_nir.c"
FEATURES_GENERATOR = MESA_ROOT / "src/vulkan/util/vk_physical_device_features_gen.py"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class KhrShaderFloatControls2SupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.spirv_to_nir = SPIRV_TO_NIR.read_text(encoding="utf-8")
        cls.vtn_alu = VTN_ALU.read_text(encoding="utf-8")
        cls.spirv_to_dxil = SPIRV_TO_DXIL.read_text(encoding="utf-8")
        cls.dxil_nir = DXIL_NIR.read_text(encoding="utf-8")
        cls.features_generator = FEATURES_GENERATOR.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_are_reported_by_the_compiler_path(self):
        self.assertRegex(
            self.device,
            r"\.KHR_shader_float_controls2\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.shaderFloatControls2\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.KHR_shader_float_controls\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"#define DZN_API_VERSION VK_MAKE_VERSION\(1,\s*2,",
        )
        self.assertIn("Mesa SPIR-V/NIR consumes FloatControls2", self.device)

    def test_extension_feature_alias_and_api_dependency_are_registered(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_KHR_shader_float_controls2']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_4")
        self.assertIn("VK_VERSION_1_1", extension.get("depends", ""))
        self.assertIn("VK_KHR_shader_float_controls", extension.get("depends", ""))
        self.assertEqual(extension.findall("./require/command"), [])
        self.assertIsNotNone(
            extension.find(
                "./require/feature[@name='shaderFloatControls2']"
            )
        )
        self.assertIsNotNone(
            self.registry.find(
                "./types/type[@name='VkPhysicalDeviceVulkan14Features']"
                "/member[name='shaderFloatControls2']"
            )
        )
        self.assertIn(
            "['Vulkan14Features', 'ShaderFloatControls2Features']",
            self.features_generator,
        )
        self.assertIn("'shaderFloatControls2'", self.features_generator)

    def test_spirv_parser_accepts_float_controls2_capability_and_defaults(self):
        self.assertRegex(self.spirv_to_nir, r"\.FloatControls2\s*=\s*true\b")
        self.assertIn("case SpvExecutionModeFPFastMathDefault", self.spirv_to_nir)
        self.assertIn("vtn_fp_math_ctrl_for_base_type", self.spirv_to_nir)
        for control in (
            "nir_fp_no_contract",
            "nir_fp_no_reassoc",
            "nir_fp_no_transform",
            "nir_fp_preserve_nan",
            "nir_fp_preserve_inf",
            "nir_fp_preserve_signed_zero",
        ):
            self.assertIn(control, self.spirv_to_nir)

    def test_instruction_fast_math_modes_are_mapped_to_nir_constraints(self):
        handler = re.search(
            r"(?s)handle_fp_fast_math\(.*?\n}\n",
            self.vtn_alu,
        )
        self.assertIsNotNone(handler)
        body = handler.group(0)
        self.assertIn("SpvDecorationFPFastMathMode", body)
        self.assertIn("SpvFPFastMathModeAllowContractMask", body)
        self.assertIn("SpvFPFastMathModeAllowReassocMask", body)
        self.assertIn("SpvFPFastMathModeAllowTransformMask", body)
        self.assertIn("nir_fp_preserve_nan", body)
        self.assertIn("nir_fp_preserve_inf", body)
        self.assertIn("nir_fp_preserve_signed_zero", body)

    def test_dozen_uses_the_shared_spirv_to_nir_and_optimization_path(self):
        self.assertIn("spirv_to_nir(words, word_count, spec", self.spirv_to_dxil)
        self.assertIn("dxil_spirv_nir_prep(nir)", self.spirv_to_dxil)
        self.assertIn("nir_opt_algebraic", self.dxil_nir)
        self.assertIn("spirv_to_nir_options", self.dxil_nir)

    def test_common_runtime_maps_and_validates_the_khr_feature_chain(self):
        self.assertIn("VK_ERROR_FEATURE_NOT_PRESENT", self.features_generator)
        self.assertIn("vk_common_GetPhysicalDeviceFeatures2", self.features_generator)
        self.assertIn("supported_${f.c_type}", self.features_generator)
        capability = self.registry.find(
            "./spirvcapabilities/spirvcapability[@name='FloatControls2']"
        )
        self.assertIsNotNone(capability)
        self.assertTrue(
            any(
                enable.get("feature") == "shaderFloatControls2"
                and enable.get("requires") == "VK_KHR_shader_float_controls2"
                for enable in capability.findall("./enable")
            )
        )


if __name__ == "__main__":
    unittest.main()

