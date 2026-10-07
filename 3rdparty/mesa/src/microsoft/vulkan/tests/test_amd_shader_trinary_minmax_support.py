#!/usr/bin/env python3
"""Verify the Dozen VK_AMD_shader_trinary_minmax path is complete and gated."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
SPIRV_PARSER = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
SPIRV_AMD = MESA_ROOT / "src/compiler/spirv/vtn_amd.c"
DXIL_BACKEND = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class AmdShaderTrinaryMinmaxSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.parser = SPIRV_PARSER.read_text(encoding="utf-8")
        cls.amd = SPIRV_AMD.read_text(encoding="utf-8")
        cls.dxil = DXIL_BACKEND.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_dozen_advertises_extension_without_a_fake_feature_bit(self):
        self.assertRegex(
            self.device,
            r"\.AMD_shader_trinary_minmax\s*=\s*true\b",
        )
        extension = self.registry.find(
            "./extensions/extension[@name='VK_AMD_shader_trinary_minmax']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("nofeatures"), "true")
        self.assertFalse(
            any(
                requirement.findall("./command") or requirement.findall("./feature")
                for requirement in extension.findall("./require")
            )
        )

    def test_registry_maps_vulkan_enablement_to_the_spirv_extension(self):
        spirv_extension = self.registry.find(
            "./spirvextensions/spirvextension[@name='SPV_AMD_shader_trinary_minmax']"
        )
        self.assertIsNotNone(spirv_extension)
        self.assertTrue(
            any(
                node.get("extension") == "VK_AMD_shader_trinary_minmax"
                for node in spirv_extension.findall("./enable")
            )
        )

    def test_parser_handler_is_gated_by_the_logical_device_extension(self):
        self.assertRegex(
            self.pipeline,
            r"spirv_opts\.amd_trinary_minmax\s*=\s*"
            r"device->vk\.enabled_extensions\.AMD_shader_trinary_minmax\s*;",
        )
        self.assertRegex(
            self.pipeline,
            r"vk_pipeline_shader_stage_to_nir\([\s\S]*?&spirv_opts,",
        )
        self.assertRegex(
            self.parser,
            r'SPV_AMD_shader_trinary_minmax"\)\s*==\s*0\)\s*'
            r"&&\s*\(b->options\s*&&\s*b->options->amd_trinary_minmax\)",
        )
        self.assertIn("vtn_handle_amd_shader_trinary_minmax_instruction", self.parser)

    def test_all_nine_extended_instructions_lower_to_nir_minmax_ops(self):
        opcodes = (
            "FMin3AMD", "UMin3AMD", "SMin3AMD",
            "FMax3AMD", "UMax3AMD", "SMax3AMD",
            "FMid3AMD", "UMid3AMD", "SMid3AMD",
        )
        for opcode in opcodes:
            with self.subTest(opcode=opcode):
                self.assertIn(f"case {opcode}:", self.amd)
        for nir_op in ("nir_fmin", "nir_fmax", "nir_umin", "nir_umax", "nir_imin", "nir_imax"):
            self.assertIn(nir_op, self.amd)

    def test_dxil_backend_emits_each_binary_minmax_operation(self):
        for nir_op, intrinsic in (
            ("nir_op_imax", "DXIL_INTR_IMAX"),
            ("nir_op_imin", "DXIL_INTR_IMIN"),
            ("nir_op_umax", "DXIL_INTR_UMAX"),
            ("nir_op_umin", "DXIL_INTR_UMIN"),
            ("nir_op_fmax", "DXIL_INTR_FMAX"),
            ("nir_op_fmin", "DXIL_INTR_FMIN"),
        ):
            with self.subTest(nir_op=nir_op):
                self.assertIn(nir_op, self.dxil)
                self.assertIn(intrinsic, self.dxil)


if __name__ == "__main__":
    unittest.main()

