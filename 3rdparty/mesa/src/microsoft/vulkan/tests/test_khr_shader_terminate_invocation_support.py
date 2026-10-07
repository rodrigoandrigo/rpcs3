#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify VK_KHR_shader_terminate_invocation support in Dozen."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
DXIL_NIR = MESA_ROOT / "src/microsoft/compiler/dxil_nir.c"
DXIL_NIR_PASSES = MESA_ROOT / "src/microsoft/spirv_to_dxil/dxil_spirv_nir.c"
FEATURES_GENERATOR = MESA_ROOT / "src/vulkan/util/vk_physical_device_features_gen.py"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    match = re.search(
        r"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\n" + re.escape(name) + r"\s*\(",
        source,
    )
    if match is None:
        raise AssertionError(f"function not found: {name}")
    brace = source.index("{", match.end())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unclosed function: {name}")


class ShaderTerminateInvocationSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.dxil_nir = DXIL_NIR.read_text(encoding="utf-8")
        cls.dxil_nir_passes = DXIL_NIR_PASSES.read_text(encoding="utf-8")
        cls.features_generator = FEATURES_GENERATOR.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_core_feature_are_reported(self):
        self.assertRegex(
            self.device,
            r"\.KHR_shader_terminate_invocation\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.shaderTerminateInvocation\s*=\s*true\b",
        )

    def test_registry_promotes_the_feature_to_vulkan_13(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_KHR_shader_terminate_invocation']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_3")
        feature = extension.find(
            "./require/feature[@name='shaderTerminateInvocation']"
        )
        self.assertIsNotNone(feature)
        self.assertEqual(
            feature.get("struct"),
            "VkPhysicalDeviceShaderTerminateInvocationFeaturesKHR",
        )
        core_feature = self.registry.find(
            "./types/type[@name='VkPhysicalDeviceVulkan13Features']"
        )
        self.assertIsNotNone(core_feature)
        self.assertIsNotNone(
            core_feature.find("./member[name='shaderTerminateInvocation']")
        )

    def test_common_runtime_queries_and_validates_the_pnext_feature(self):
        # The Vulkan runtime generator emits feature queries and feature-not-present
        # validation for every vk.xml feature structure, including this KHR alias.
        self.assertIn("VK_ERROR_FEATURE_NOT_PRESENT", self.features_generator)
        self.assertIn("vk_common_GetPhysicalDeviceFeatures2", self.features_generator)
        self.assertIn("pdevice->supported_features.", self.features_generator)
        self.assertIn("supported_${f.c_type}", self.features_generator)

    def test_dozen_compiler_runs_the_fragment_terminate_lowering(self):
        get_nir = function_body(self.pipeline, "dzn_pipeline_get_nir_shader")
        self.assertIn("vk_pipeline_shader_stage_to_nir", get_nir)
        self.assertIn("dxil_spirv_nir_passes(*nir", get_nir)

        passes = function_body(self.dxil_nir_passes, "dxil_spirv_nir_passes")
        lower = passes.index("dxil_nir_lower_discard_and_terminate")
        lower_returns = passes.index("nir_lower_returns", lower)
        self.assertLess(lower, lower_returns)
        self.assertIn("nir->info.stage == MESA_SHADER_FRAGMENT", passes)

    def test_terminate_is_lowered_to_demote_and_immediate_return(self):
        lower = function_body(self.dxil_nir, "dxil_nir_lower_discard_and_terminate")
        self.assertIn("MESA_SHADER_FRAGMENT", lower)
        kill = function_body(self.dxil_nir, "lower_kill")
        self.assertIn("nir_intrinsic_terminate", kill)
        self.assertIn("nir_intrinsic_terminate_if", kill)
        self.assertIn("nir_demote(builder)", kill)
        self.assertIn("nir_demote_if(builder", kill)
        self.assertIn("nir_jump(builder, nir_jump_return)", kill)


if __name__ == "__main__":
    unittest.main()

