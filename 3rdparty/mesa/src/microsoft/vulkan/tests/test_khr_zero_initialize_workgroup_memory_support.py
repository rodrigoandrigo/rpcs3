#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify VK_KHR_zero_initialize_workgroup_memory support in Dozen."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DXIL_NIR_PASSES = MESA_ROOT / "src/microsoft/spirv_to_dxil/dxil_spirv_nir.c"
DXIL_NIR = MESA_ROOT / "src/microsoft/compiler/dxil_nir.c"
DXIL_CODEGEN = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
SPIRV_VARIABLES = MESA_ROOT / "src/compiler/spirv/vtn_variables.c"
NIR_INITIALIZERS = MESA_ROOT / "src/compiler/nir/nir_lower_variable_initializers.c"
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


class ZeroInitializeWorkgroupMemorySupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.dxil_nir_passes = DXIL_NIR_PASSES.read_text(encoding="utf-8")
        cls.dxil_nir = DXIL_NIR.read_text(encoding="utf-8")
        cls.dxil_codegen = DXIL_CODEGEN.read_text(encoding="utf-8")
        cls.spirv_variables = SPIRV_VARIABLES.read_text(encoding="utf-8")
        cls.nir_initializers = NIR_INITIALIZERS.read_text(encoding="utf-8")
        cls.features_generator = FEATURES_GENERATOR.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_core_feature_are_reported(self):
        self.assertRegex(
            self.device,
            r"\.KHR_zero_initialize_workgroup_memory\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.shaderZeroInitializeWorkgroupMemory\s*=\s*true\b",
        )

    def test_registry_promotes_khr_feature_to_vulkan_13(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_KHR_zero_initialize_workgroup_memory']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_3")
        feature = extension.find(
            "./require/feature[@name='shaderZeroInitializeWorkgroupMemory']"
        )
        self.assertIsNotNone(feature)
        self.assertEqual(
            feature.get("struct"),
            "VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeaturesKHR",
        )
        core_feature = self.registry.find(
            "./types/type[@name='VkPhysicalDeviceVulkan13Features']"
        )
        self.assertIsNotNone(core_feature)
        self.assertIsNotNone(
            core_feature.find(
                "./member[name='shaderZeroInitializeWorkgroupMemory']"
            )
        )

    def test_common_runtime_queries_and_validates_feature_chains(self):
        # vk.xml drives both the KHR alias and the Vulkan 1.3 feature member.
        self.assertIn("VK_ERROR_FEATURE_NOT_PRESENT", self.features_generator)
        self.assertIn("vk_common_GetPhysicalDeviceFeatures2", self.features_generator)
        self.assertIn("pdevice->supported_features.", self.features_generator)
        self.assertIn("supported_${f.c_type}", self.features_generator)

    def test_spirv_parser_marks_null_workgroup_initializers(self):
        self.assertIn("VK_KHR_zero_initialize_workgroup_memory", self.spirv_variables)
        self.assertIn(
            "b->shader->info.zero_initialize_shared_memory = true",
            self.spirv_variables,
        )
        self.assertIn("SpvStorageClassWorkgroup", self.spirv_variables)
        self.assertIn("initializer->is_null_constant", self.spirv_variables)

    def test_dxil_pipeline_lowers_before_translating_shared_memory(self):
        passes = function_body(self.dxil_nir_passes, "dxil_spirv_nir_passes")
        shared_route = passes.index("nir->info.zero_initialize_shared_memory")
        explicit_io = passes.index(
            "nir_lower_explicit_io, nir_var_mem_shared", shared_route
        )
        size_gate = passes.index("nir->info.shared_size > 0", explicit_io)
        zero_init = passes.index("nir_zero_initialize_shared_memory", explicit_io)
        dxil_shared = passes.index("dxil_nir_scratch_and_shared_to_dxil", zero_init)
        self.assertIn("nir->info.stage == MESA_SHADER_COMPUTE", passes[shared_route:])
        self.assertIn("align(nir->info.shared_size, chunk_size)", passes)
        self.assertLess(explicit_io, zero_init)
        self.assertLess(size_gate, zero_init)
        self.assertLess(zero_init, dxil_shared)

    def test_zero_init_is_cooperative_and_dxil_maps_group_index_shared_memory_and_barrier(self):
        zero_init = function_body(
            self.nir_initializers, "nir_zero_initialize_shared_memory"
        )
        self.assertIn("nir_load_local_invocation_index", zero_init)
        self.assertIn("nir_store_shared", zero_init)
        self.assertIn("nir_barrier(&b, SCOPE_WORKGROUP, SCOPE_WORKGROUP", zero_init)

        shared_lower = function_body(self.dxil_nir, "lower_shared_to_var")
        self.assertIn("nir_intrinsic_store_shared", shared_lower)
        self.assertIn("nir_store_array_var", shared_lower)
        shared_allocation = function_body(
            self.dxil_nir, "dxil_nir_lower_shared_to_var"
        )
        self.assertIn("glsl_array_type(glsl_uint_type(), words, 1)", shared_allocation)
        self.assertIn("DXIL_AS_GROUPSHARED", self.dxil_codegen)
        self.assertIn("DXIL_BARRIER_MODE_SYNC_THREAD_GROUP", self.dxil_codegen)
        self.assertIn("DXIL_BARRIER_MODE_GROUPSHARED_MEM_FENCE", self.dxil_codegen)
        self.assertIn("emit_load_local_invocation_index", self.dxil_codegen)


if __name__ == "__main__":
    unittest.main()

