#!/usr/bin/env python3
"""Verify Dozen's KHR subgroup rotate gate and NIR-to-DXIL lowering path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DXIL_NIR = MESA_ROOT / "src/microsoft/spirv_to_dxil/dxil_spirv_nir.c"
NIR_SUBGROUPS = MESA_ROOT / "src/compiler/nir/nir_lower_subgroups.c"
DXIL = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
FEATURES_GEN = MESA_ROOT / "src/vulkan/util/vk_physical_device_features_gen.py"
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


class KhrShaderSubgroupRotateSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.dxil_nir = DXIL_NIR.read_text(encoding="utf-8")
        cls.nir_subgroups = NIR_SUBGROUPS.read_text(encoding="utf-8")
        cls.dxil = DXIL.read_text(encoding="utf-8")
        cls.features_gen = FEATURES_GEN.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_use_the_existing_waveops_gate(self):
        self.assertRegex(
            self.device,
            r"\.KHR_shader_subgroup_rotate\s*=\s*pdev->options1\.WaveOps\b",
        )
        self.assertRegex(
            self.device,
            r"\.shaderSubgroupRotate\s*=\s*pdev->options1\.WaveOps\b",
        )
        self.assertRegex(
            self.device,
            r"\.shaderSubgroupRotateClustered\s*=\s*false\b",
        )

    def test_khr_extension_is_available_without_changing_api_version(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_KHR_shader_subgroup_rotate']"
        )
        self.assertIsNotNone(ext)
        self.assertEqual(ext.get("promotedto"), "VK_VERSION_1_4")
        self.assertIn("VK_VERSION_1_1", ext.get("depends"))
        features = {
            (node.get("name"), node.get("struct"))
            for node in ext.findall("./require/feature")
        }
        self.assertEqual(
            features,
            {
                (
                    "shaderSubgroupRotate",
                    "VkPhysicalDeviceShaderSubgroupRotateFeaturesKHR",
                )
            },
        )
        self.assertEqual(ext.findall("./require/command"), [])
        self.assertRegex(
            self.device, r"#define DZN_API_VERSION VK_MAKE_VERSION\(1, 2,"
        )
        self.assertIn(
            "['Vulkan14Features', 'ShaderSubgroupRotateFeatures']",
            self.features_gen,
        )
        self.assertIn("'shaderSubgroupRotate'", self.features_gen)
        capability = self.registry.find(
            "./spirvcapabilities/spirvcapability[@name='GroupNonUniformRotateKHR']"
        )
        self.assertIsNotNone(capability)
        self.assertTrue(
            any(
                node.get("feature") == "shaderSubgroupRotate"
                and node.get("requires") == "VK_KHR_shader_subgroup_rotate"
                for node in capability.findall("./enable")
            )
        )

    def test_rotate_lowers_to_cluster_aware_shuffle_and_dxil_wave_read(self):
        self.assertIn(".lower_rotate_to_shuffle = true", self.dxil_nir)
        lowering = function_body(self.nir_subgroups, "lower_to_shuffle")
        self.assertIn("case nir_intrinsic_rotate", lowering)
        self.assertIn("nir_intrinsic_cluster_size(intrin)", lowering)
        self.assertIn("nir_shuffle(b, intrin->src[0].ssa, index)", lowering)
        dispatch = function_body(self.nir_subgroups, "lower_subgroups_instr")
        self.assertIn("options->lower_rotate_to_shuffle", dispatch)
        emitter = function_body(self.dxil, "emit_read_invocation")
        self.assertIn("ctx->mod.feats.wave_ops = 1", emitter)
        self.assertIn("dx.op.waveReadLaneAt", emitter)
        self.assertIn("DXIL_INTR_WAVE_READ_LANE_AT", emitter)

    def test_only_nonclustered_feature_is_exposed_on_vulkan_12(self):
        self.assertRegex(self.device, r"\.apiVersion\s*=\s*DZN_API_VERSION")
        self.assertRegex(
            self.device, r"#define DZN_API_VERSION VK_MAKE_VERSION\(1, 2,"
        )
        self.assertIn(".shaderSubgroupRotateClustered       = false", self.device)


if __name__ == "__main__":
    unittest.main()

