#!/usr/bin/env python3
"""Verify Dozen's D3D12-gated VK_EXT_shader_stencil_export path."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_NIR = MESA_ROOT / "src/microsoft/vulkan/dzn_nir.c"
DZN_PIPELINE = MESA_ROOT / "src/microsoft/vulkan/dzn_pipeline.c"
SPIRV_TO_NIR = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
DXIL_TRANSLATOR = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
DXIL_SIGNATURE = MESA_ROOT / "src/microsoft/compiler/dxil_signature.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class ShaderStencilExportSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.dzn_nir = DZN_NIR.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.spirv = SPIRV_TO_NIR.read_text(encoding="utf-8")
        cls.dxil = DXIL_TRANSLATOR.read_text(encoding="utf-8")
        cls.signature = DXIL_SIGNATURE.read_text(encoding="utf-8")
        cls.registry_text = REGISTRY.read_text(encoding="utf-8")
        cls.registry = ET.fromstring(cls.registry_text)

    def test_extension_is_gated_by_the_d3d12_pixel_shader_capability(self):
        self.assertRegex(
            self.device,
            r"\.EXT_shader_stencil_export\s*=\s*pdev->options\.PSSpecifiedStencilRefSupported\b",
        )
        self.assertIn(
            "D3D12_FEATURE_D3D12_OPTIONS, &pdev->options",
            self.device,
        )

    def test_registry_requires_spirv_extension_but_no_vulkan_feature_struct(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_shader_stencil_export']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("nofeatures"), "true")
        self.assertIn("SPV_EXT_shader_stencil_export", self.registry_text)
        self.assertFalse(extension.findall("./require/command"))
        self.assertIn("StencilExportEXT = true", self.spirv)

    def test_dzn_shader_path_translates_stencil_output_to_dxil(self):
        self.assertIn("nir_to_dxil(", self.pipeline)
        self.assertIn("FRAG_RESULT_STENCIL", self.dzn_nir)
        self.assertIn("ctx->mod.feats.stencil_ref = true", self.dxil)
        self.assertIn('"SV_StencilRef"', self.signature)
        self.assertIn("DXIL_SEM_STENCIL_REF", self.signature)


if __name__ == "__main__":
    unittest.main()

