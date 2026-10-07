#!/usr/bin/env python3
# Copyright © 2026 Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Guard the four round42 Vulkan extension implementations and their gates."""
from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]


def function_body(source: str, name: str) -> str:
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


class RequestedExtensionCoverageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device_source = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.extensions = function_body(cls.device_source, "dzn_physical_device_get_extensions")
        cls.features = function_body(cls.device_source, "dzn_physical_device_get_features")
        cls.properties = function_body(cls.device_source, "dzn_physical_device_get_properties")
        cls.cmd_buffer = (DZN_DIR / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.descriptor = (DZN_DIR / "dzn_descriptor_set.c").read_text(encoding="utf-8")
        cls.private = (DZN_DIR / "dzn_private.h").read_text(encoding="utf-8")
        cls.pipeline = (DZN_DIR / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.dxil = (MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c").read_text(encoding="utf-8")
        cls.signature = (MESA_ROOT / "src/microsoft/compiler/dxil_signature.c").read_text(encoding="utf-8")
        cls.vk_device = (MESA_ROOT / "src/vulkan/runtime/vk_device.c").read_text(encoding="utf-8")
        cls.vk_physical_device = (MESA_ROOT / "src/vulkan/runtime/vk_physical_device.c").read_text(encoding="utf-8")
        cls.backlog = json.loads(
            (DZN_DIR / "tests/extension_support_backlog.json").read_text(encoding="utf-8")
        )
        cls.scaffold = json.loads(
            (DZN_DIR / "tests/extension_support_scaffold.json").read_text(encoding="utf-8")
        )

    def test_previously_implemented_extensions_remain_reported(self) -> None:
        self.assertRegex(self.extensions, r"\.EXT_shader_uniform_buffer_unsized_array\s*=\s*true\b")
        self.assertRegex(self.features, r"\.shaderUniformBufferUnsizedArray\s*=\s*true\b")
        self.assertRegex(self.extensions, r"\.EXT_border_color_swizzle\s*=\s*dzn_custom_border_color_supported\(pdev\)")
        self.assertRegex(self.features, r"\.borderColorSwizzle\s*=\s*dzn_custom_border_color_supported\(pdev\)")
        self.assertRegex(self.features, r"\.borderColorSwizzleFromImage\s*=\s*false\b")

    def test_four_extensions_and_calibrated_aliases_are_removed_from_backlog(self) -> None:
        expected = {
            "KHR_fragment_shader_barycentric",
            "KHR_push_descriptor",
            "EXT_robustness2",
            "EXT_calibrated_timestamps",
            "KHR_calibrated_timestamps",
        }
        implemented = set(self.backlog["implemented_fields"])
        pending = {entry["field"] for entry in self.backlog["pending_extensions"]}
        scaffolded = {entry["field"] for entry in self.scaffold["extensions"]}
        self.assertTrue(expected <= implemented)
        self.assertTrue(expected.isdisjoint(pending))
        self.assertTrue(expected.isdisjoint(scaffolded))

    def test_fragment_barycentrics_and_per_vertex_use_dxil_semantics(self) -> None:
        self.assertIn(".KHR_fragment_shader_barycentric", self.extensions)
        self.assertIn("pdev->options3.BarycentricsSupported", self.extensions)
        self.assertIn(".fragmentShaderBarycentric", self.features)
        self.assertIn("SV_Barycentrics", self.signature)
        self.assertIn("DXIL_SEM_BARYCENTRICS", self.signature)
        self.assertIn("nir_intrinsic_load_per_vertex_input", self.dxil)
        self.assertIn("DXIL_INTR_ATTRIBUTE_AT_VERTEX", self.dxil)
        self.assertIn("dx.op.attributeAtVertex", self.dxil)
        self.assertIn("nir_def_as_intrinsic(coord)", self.dxil)
        self.assertNotIn("coord->parent_instr", self.dxil)
        self.assertRegex(self.properties, r"\.triStripVertexOrderIndependentOfProvokingVertex\s*=\s*false\b")

    def test_push_descriptors_have_commands_templates_limits_and_state(self) -> None:
        self.assertRegex(self.extensions, r"\.KHR_push_descriptor\s*=\s*true\b")
        self.assertIn("dzn_CmdPushDescriptorSetKHR", self.cmd_buffer)
        self.assertIn("dzn_CmdPushDescriptorSetWithTemplateKHR", self.cmd_buffer)
        self.assertIn("dzn_cmd_buffer_push_descriptors", self.cmd_buffer)
        self.assertIn("dzn_descriptor_set_push_allocate", self.cmd_buffer)
        self.assertIn("dzn_cmd_buffer_disturb_descriptor_sets", self.cmd_buffer)
        self.assertIn("VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS", self.descriptor)
        self.assertIn("#define DZN_MAX_PUSH_DESCRIPTORS 32", self.private)
        self.assertIn(".maxPushDescriptors = DZN_MAX_PUSH_DESCRIPTORS", self.properties)

    def test_robustness2_reports_only_the_feature_with_complete_semantics(self) -> None:
        self.assertRegex(
            self.extensions,
            r"\.EXT_robustness2\s*=\s*pdev->root_sig_version\s*>=\s*D3D_ROOT_SIGNATURE_VERSION_1_1",
        )
        self.assertRegex(
            self.features,
            r"\.robustBufferAccess2\s*=\s*pdev->root_sig_version\s*>=\s*D3D_ROOT_SIGNATURE_VERSION_1_1",
        )
        self.assertRegex(self.features, r"\.robustBufferAccess\s*=\s*true\b")
        self.assertRegex(self.features, r"\.robustImageAccess2\s*=\s*false\b")
        self.assertRegex(self.features, r"\.nullDescriptor\s*=\s*false\b")
        self.assertIn("D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS", self.descriptor)
        self.assertIn("!device->vk.enabled_features.robustBufferAccess2", self.device_source)
        self.assertIn(".robustUniformBufferAccessSizeAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT", self.properties)
        self.assertIn(".robustStorageBufferAccessSizeAlignment = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT", self.properties)

    def test_calibrated_timestamps_use_real_gpu_host_calibration(self) -> None:
        self.assertIn(".EXT_calibrated_timestamps", self.extensions)
        self.assertIn(".KHR_calibrated_timestamps", self.extensions)
        self.assertGreaterEqual(self.device_source.count("GetClockCalibration"), 2)
        self.assertIn("pdev->calibrated_timestamps", self.device_source)
        self.assertIn("vk_common_GetCalibratedTimestampsKHR", self.vk_device)
        self.assertIn("pMaxDeviation", self.vk_device)
        self.assertIn("vk_common_GetPhysicalDeviceCalibrateableTimeDomainsKHR", self.vk_physical_device)

    def test_no_unimplemented_alias_is_accidentally_advertised(self) -> None:
        self.assertNotRegex(self.extensions, r"\.KHR_robustness2\s*=")


if __name__ == "__main__":
    unittest.main()

