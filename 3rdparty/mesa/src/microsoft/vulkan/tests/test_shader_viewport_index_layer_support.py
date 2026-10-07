#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify Vulkan 1.2 viewport/layer output support is gated by D3D12."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_CMD_BUFFER = TEST_DIR.parent / "dzn_cmd_buffer.c"
DZN_PRIVATE = TEST_DIR.parent / "dzn_private.h"
PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
DXIL_SIGNATURE = MESA_ROOT / "src/microsoft/compiler/dxil_signature.c"
SPIRV_VARIABLES = MESA_ROOT / "src/compiler/spirv/vtn_variables.c"
DXIL_SPIRV = MESA_ROOT / "src/microsoft/spirv_to_dxil/dxil_spirv_nir.c"
D3D12_HEADER = MESA_ROOT / "subprojects/DirectX-Headers-1.0/include/directx/d3d12.h"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"
HARNESS = TEST_DIR / "run-round37-viewport-layer-compile-tests.py"


class ShaderViewportIndexLayerSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.cmd_buffer = DZN_CMD_BUFFER.read_text(encoding="utf-8")
        cls.private = DZN_PRIVATE.read_text(encoding="utf-8")
        cls.pipeline = PIPELINE.read_text(encoding="utf-8")
        cls.signature = DXIL_SIGNATURE.read_text(encoding="utf-8")
        cls.spirv_variables = SPIRV_VARIABLES.read_text(encoding="utf-8")
        cls.dxil_spirv = DXIL_SPIRV.read_text(encoding="utf-8")
        cls.header = D3D12_HEADER.read_text(encoding="utf-8")
        cls.registry_text = REGISTRY.read_text(encoding="utf-8")
        cls.registry = ET.fromstring(cls.registry_text)

    def test_core_features_are_gated_by_the_adapter_capability(self):
        capability = "pdev->options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation"
        self.assertRegex(
            self.device,
            r"\.shaderOutputViewportIndex\s*=\s*" + capability + r"\s*,",
        )
        self.assertRegex(
            self.device,
            r"\.shaderOutputLayer\s*=\s*" + capability + r"\s*,",
        )
        self.assertIn("D3D12_FEATURE_D3D12_OPTIONS, &pdev->options", self.device)
        self.assertIn("VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation", self.header)
        self.assertRegex(
            self.device,
            r"\.EXT_shader_viewport_index_layer\s*=\s*\n\s*pdev->options\.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation\s*&&\s*MAX_VP\s*>\s*1\s*,",
        )

    def test_multi_viewport_uses_d3d12_array_paths_and_limit(self):
        self.assertRegex(self.device, r"\.multiViewport\s*=\s*MAX_VP\s*>\s*1\s*,")
        self.assertIn(".maxViewports = MAX_VP", self.device)
        self.assertIn("#define MAX_VP D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE", self.private)
        self.assertRegex(
            self.header,
            r"D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE\s*\(\s*16\s*\)",
        )
        self.assertIn("RSSetViewports(cmdbuf->cmdlist, count", self.cmd_buffer)
        self.assertIn("RSSetScissorRects(cmdbuf->cmdlist, count", self.cmd_buffer)
        self.assertIn("cmdbuf->state.viewport_count : pipeline->vp.count", self.cmd_buffer)
        self.assertIn("cmdbuf->state.scissor_count : pipeline->scissor.count", self.cmd_buffer)
        self.assertIn("pipeline->vp.count = in_vp->viewportCount", self.pipeline)

    def test_vulkan_12_feature_fields_and_promoted_extension_are_registered(self):
        core = self.registry.find("./types/type[@name='VkPhysicalDeviceVulkan12Features']")
        self.assertIsNotNone(core)
        member_names = {member.findtext("name") for member in core.findall("member")}
        self.assertIn("shaderOutputViewportIndex", member_names)
        self.assertIn("shaderOutputLayer", member_names)
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_shader_viewport_index_layer']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_2")
        self.assertEqual(extension.get("nofeatures"), "true")

    def test_dxil_signature_maps_both_vulkan_builtins_to_system_values(self):
        self.assertIn('"SV_ViewportArrayIndex"', self.signature)
        self.assertIn("DXIL_SEM_VIEWPORT_ARRAY_INDEX", self.signature)
        self.assertIn('"SV_RenderTargetArrayIndex"', self.signature)
        self.assertIn("DXIL_SEM_RENDERTARGET_ARRAY_INDEX", self.signature)

    def test_spirv_parser_accepts_core_capabilities_in_pre_raster_stages(self):
        self.assertIn("b->supported_capabilities.ShaderLayer", self.spirv_variables)
        self.assertIn("b->supported_capabilities.ShaderViewportIndex", self.spirv_variables)
        self.assertIn(".ShaderLayer = true", self.dxil_spirv)
        self.assertIn(".ShaderViewportIndex = true", self.dxil_spirv)
        self.assertIn(".ShaderViewportIndexLayerEXT = true", self.dxil_spirv)

    def test_compile_harness_covers_both_system_value_outputs(self):
        harness = HARNESS.read_text(encoding="utf-8")
        self.assertIn("(\"ViewportIndex\", 10, 70, b\"SV_ViewportArrayIndex\", False)", harness)
        self.assertIn("(\"Layer\", 9, 69, b\"SV_RenderTargetArrayIndex\", False)", harness)
        self.assertIn("(\"ViewportIndex_EXT\", 10, 5254", harness)
        self.assertIn("(\"Layer_EXT\", 9, 5254", harness)
        self.assertIn('"--stage", "vertex"', harness)


if __name__ == "__main__":
    unittest.main()

