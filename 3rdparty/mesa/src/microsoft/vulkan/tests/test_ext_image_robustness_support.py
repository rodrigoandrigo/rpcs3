#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify Dozen's VK_EXT_image_robustness implementation path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
ROBUST_LOWERING = MESA_ROOT / "src/compiler/nir/nir_lower_robust_access.c"
DXIL_BACKEND = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class ImageRobustnessSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.robust_lowering = ROBUST_LOWERING.read_text(encoding="utf-8")
        cls.dxil_backend = DXIL_BACKEND.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_device_advertises_extension_and_feature(self):
        self.assertRegex(
            self.device,
            r"\.EXT_image_robustness\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.robustImageAccess\s*=\s*true\b",
        )
        self.assertNotRegex(
            self.device,
            r"\.robustImageAccess\s*=\s*false\b",
        )

    def test_registry_declares_feature_type_and_version_dependency(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_image_robustness']"
        )
        self.assertIsNotNone(extension)
        self.assertIn("VK_VERSION_1_1", extension.get("depends", ""))
        self.assertTrue(
            any(
                feature.get("name") == "robustImageAccess"
                and feature.get("struct")
                == "VkPhysicalDeviceImageRobustnessFeaturesEXT"
                for feature in extension.findall("./require/feature")
            )
        )
        self.assertTrue(
            any(
                item.get("name")
                == "VkPhysicalDeviceImageRobustnessFeaturesEXT"
                for item in extension.findall("./require/type")
            )
        )

    def test_filter_covers_vulkan_storage_image_intrinsics(self):
        match = re.search(
            r"dzn_robust_image_access_intrinsic_filter\(.*?\n}\n",
            self.pipeline,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        filter_body = match.group(0)
        for intrinsic in (
            "image_load",
            "bindless_image_load",
            "image_store",
            "bindless_image_store",
            "image_atomic",
            "bindless_image_atomic",
            "image_atomic_swap",
            "bindless_image_atomic_swap",
            "image_deref_load",
            "image_deref_store",
            "image_deref_atomic",
            "image_deref_atomic_swap",
            "image_heap_load",
            "image_heap_store",
            "image_heap_atomic",
            "image_heap_atomic_swap",
        ):
            self.assertIn(f"nir_intrinsic_{intrinsic}", filter_body)
        self.assertIn("default:", filter_body)
        self.assertIn("return false;", filter_body)

        for intrinsic in (
            "bindless_image_load",
            "bindless_image_store",
            "bindless_image_atomic",
            "bindless_image_atomic_swap",
        ):
            self.assertIn(f"case nir_intrinsic_{intrinsic}:", self.robust_lowering)

    def test_pipeline_lowers_only_when_feature_is_enabled(self):
        guard = "if (device->vk.enabled_features.robustImageAccess ||"
        lowering = "NIR_PASS(_, *nir, nir_lower_robust_access,"
        self.assertIn(guard, self.pipeline)
        self.assertIn("device->vk.enabled_features.robustImageAccess2)", self.pipeline)
        self.assertIn(lowering, self.pipeline)
        self.assertLess(self.pipeline.index(guard), self.pipeline.index(lowering))
        self.assertLess(
            self.pipeline.index(lowering),
            self.pipeline.index("dxil_spirv_nir_passes(*nir"),
        )

    def test_robust_lowering_checks_view_dimensions_and_masks_invalid_accesses(self):
        self.assertIn("nir_image_size(b,", self.robust_lowering)
        self.assertIn("nir_ball(b, nir_ult(b, coord, size))", self.robust_lowering)
        self.assertIn("wrap_in_if(b, instr, in_bounds, is_load)", self.robust_lowering)
        self.assertIn("nir_imm_zero(b, instr->def.num_components", self.robust_lowering)

    def test_bindless_image_robustness_uses_bindless_size_and_sample_queries(self):
        self.assertIn("case nir_image_intrinsic_type_bindless:", self.robust_lowering)
        self.assertIn("nir_intrinsic_bindless_image_size", self.robust_lowering)
        self.assertIn("nir_intrinsic_bindless_image_samples", self.robust_lowering)
        self.assertIn("if (!desc)", self.robust_lowering)
        self.assertIn("return nir_imm_zero(b, num_components, bit_size);", self.robust_lowering)

    def test_dxil_backend_emits_image_view_dimensions(self):
        image_size = re.search(
            r"emit_image_size\(.*?\n}\n", self.dxil_backend, re.DOTALL
        )
        texture_size = re.search(
            r"emit_texture_size\(.*?\n}\n", self.dxil_backend, re.DOTALL
        )
        self.assertIsNotNone(image_size)
        self.assertIsNotNone(texture_size)
        self.assertIn("nir_intrinsic_image_size", self.dxil_backend)
        self.assertIn("get_resource_handle(ctx, &intr->src[0]", image_size.group(0))
        self.assertIn("emit_texture_size(ctx, &params)", image_size.group(0))
        self.assertIn('"dx.op.getDimensions"', texture_size.group(0))

    def test_robustness_state_partitions_graphics_and_compute_nir_caches(self):
        self.assertEqual(
            len(re.findall(
                r"&device->vk.enabled_features\.robustImageAccess(?=,)",
                self.pipeline,
            )),
            2,
        )
        self.assertEqual(
            len(re.findall(
                r"&device->vk.enabled_features\.robustImageAccess2(?=,)",
                self.pipeline,
            )),
            2,
        )


if __name__ == "__main__":
    unittest.main()

