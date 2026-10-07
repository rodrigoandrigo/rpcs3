#!/usr/bin/env python3
"""Verify the KHR spelling of Dozen's Vulkan 1.2 subgroup-type feature."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DXIL = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    match = re.search(rf"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\n{name}\(", source)
    if not match:
        raise AssertionError(f"function definition not found: {name}")
    brace = source.index("{", match.end())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {name}")


class KhrShaderSubgroupExtendedTypesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.dxil = DXIL.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_khr_extension_is_an_api12_alias_of_an_already_reported_core_feature(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_KHR_shader_subgroup_extended_types']"
        )
        self.assertIsNotNone(ext)
        self.assertEqual(ext.get("promotedto"), "VK_VERSION_1_2")
        self.assertRegex(self.device, r"DZN_API_VERSION\s+VK_MAKE_VERSION\(1,\s*2,")
        self.assertRegex(
            self.device,
            r"\.KHR_shader_subgroup_extended_types\s*=\s*true\b",
        )
        self.assertRegex(self.device, r"\.shaderSubgroupExtendedTypes\s*=\s*true\b")
        alias = self.registry.find(
            "./types/type[@name='VkPhysicalDeviceShaderSubgroupExtendedTypesFeaturesKHR']"
        )
        self.assertIsNotNone(alias)
        canonical = "VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures"
        self.assertEqual(alias.get("alias"), canonical)
        feature = self.registry.find(f"./types/type[@name='{canonical}']")
        self.assertIsNotNone(feature)
        self.assertIn("shaderSubgroupExtendedTypes", "".join(feature.itertext()))
        self.assertEqual(ext.findall("./require/command"), [])
        required_features = {
            (entry.get("name"), entry.get("struct"))
            for entry in ext.findall("./require/feature")
        }
        self.assertIn(
            ("shaderSubgroupExtendedTypes", "VkPhysicalDeviceShaderSubgroupExtendedTypesFeaturesKHR"),
            required_features,
        )

    def test_dzn_subgroup_capabilities_and_dxil_keep_the_intrinsic_bit_width(self):
        self.assertIn("VK_SUBGROUP_FEATURE_ARITHMETIC_BIT", self.device)
        body = function_body(self.dxil, "emit_reduce")
        self.assertIn("get_overload(alu_type, intr->def.bit_size)", body)
        self.assertIn("dxil_get_function(&ctx->mod", body)
        self.assertIn("nir_intrinsic_reduce", self.dxil)
        self.assertIn("nir_intrinsic_exclusive_scan", self.dxil)


if __name__ == "__main__":
    unittest.main()

