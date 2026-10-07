#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Source-level coverage checks for VK_INTEL_shader_integer_functions2."""
from __future__ import annotations
import unittest
import re
import xml.etree.ElementTree as ET
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]

OPS = {
    "UCountLeadingZerosINTEL": "nir_op_uclz",
    "AbsISubINTEL": "nir_op_uabs_isub",
    "AbsUSubINTEL": "nir_op_uabs_usub",
    "IAddSatINTEL": "nir_op_iadd_sat",
    "UAddSatINTEL": "nir_op_uadd_sat",
    "IAverageINTEL": "nir_op_ihadd",
    "UAverageINTEL": "nir_op_uhadd",
    "IAverageRoundedINTEL": "nir_op_irhadd",
    "UAverageRoundedINTEL": "nir_op_urhadd",
    "ISubSatINTEL": "nir_op_isub_sat",
    "USubSatINTEL": "nir_op_usub_sat",
    "IMul32x16INTEL": "nir_op_imul_32x16",
    "UMul32x16INTEL": "nir_op_umul_32x16",
}


class ShaderIntegerFunctions2SupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dzn_device = (MESA_ROOT / "src/microsoft/vulkan/dzn_device.c").read_text()
        cls.dxil_nir = (MESA_ROOT / "src/microsoft/spirv_to_dxil/dxil_spirv_nir.c").read_text()
        cls.dxil_compiler = (MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c").read_text()
        cls.spirv_alu = (MESA_ROOT / "src/compiler/spirv/vtn_alu.c").read_text()
        cls.spirv_nir = (MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c").read_text()
        cls.algebraic = (MESA_ROOT / "src/compiler/nir/nir_opt_algebraic.py").read_text()
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_and_feature_are_reported(self):
        self.assertIn(".INTEL_shader_integer_functions2       = true,", self.dzn_device)
        self.assertIn(".shaderIntegerFunctions2            = true,", self.dzn_device)
        extension = self.registry.find(
            "./extensions/extension[@name='VK_INTEL_shader_integer_functions2']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(
            extension.get("depends"),
            "VK_KHR_get_physical_device_properties2,VK_VERSION_1_1",
        )
        self.assertEqual(
            [(f.get("name"), f.get("struct")) for f in extension.findall("./require/feature")],
            [("shaderIntegerFunctions2", "VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL")],
        )

    def test_dxil_frontend_accepts_the_spirv_capability(self):
        self.assertIn(".IntegerFunctions2INTEL = true,", self.dxil_nir)
        self.assertIn(".Int8 = true,", self.dxil_nir)
        self.assertIn("unsigned min_bit_size = opts->lower_int16 ? 32 : 16;", self.dxil_compiler)

    def test_every_arithmetic_opcode_maps_to_nir(self):
        for spirv_opcode, nir_opcode in OPS.items():
            with self.subTest(opcode=spirv_opcode):
                self.assertIn(f"SpvOp{spirv_opcode}:", self.spirv_alu)
                self.assertIn(f"{nir_opcode};", self.spirv_alu)
                self.assertIn(f"SpvOp{spirv_opcode}", self.spirv_nir)
        self.assertIn("case SpvOpUCountTrailingZerosINTEL:", self.spirv_alu)
        self.assertIn("nir_umin(&b->nb,", self.spirv_alu)
        self.assertRegex(self.spirv_alu, r"nir_find_lsb\(&b->nb,\s*src\[0\]\)")

    def test_dxil_uses_semantics_preserving_integer_lowers(self):
        for option in (
            ".lower_hadd = true,",
            ".lower_uadd_sat = true,",
            ".lower_usub_sat = true,",
            ".lower_iadd_sat = true,",
            ".lower_mul_32x16 = true,",
        ):
            with self.subTest(option=option):
                self.assertIn(option, self.dxil_compiler)
        for lower in (
            "('uabs_isub', a, b)",
            "('uabs_usub', a, b)",
            "('ihadd', a, b)",
            "('uhadd', a, b)",
            "('irhadd', a, b)",
            "('urhadd', a, b)",
            "('imul_32x16', a, b)",
            "('umul_32x16', a, b)",
            "('iadd_sat@' + str(bit_size), a, b)",
            "('isub_sat@' + str(bit_size), a, b)",
        ):
            with self.subTest(lower=lower):
                self.assertIn(lower, self.algebraic)
        self.assertIn("options->has_find_msb_rev", self.algebraic)

    def test_opcodes_are_enabled_in_spirv_parser(self):
        self.assertIn(".IntegerFunctions2INTEL = true,", self.spirv_nir)
        self.assertIn(".IntegerFunctions2INTEL = true,", self.dxil_nir)


if __name__ == "__main__":
    unittest.main()

