#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_depth_clip_control shader-lowering path."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
NIR_CLIP_HALFZ = MESA_ROOT / "src/compiler/nir/nir_lower_clip_halfz.c"
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


class DepthClipControlSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.nir = NIR_CLIP_HALFZ.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_software_feature_are_reported(self):
        self.assertRegex(
            self.device,
            r"\.EXT_depth_clip_control\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.depthClipControl\s*=\s*true\b",
        )

    def test_registry_dependency_and_pipeline_payload_are_complete(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_depth_clip_control']"
        )
        self.assertIsNotNone(ext)
        self.assertIn("VK_VERSION_1_1", ext.get("depends", ""))
        self.assertEqual(ext.findall("./require/command"), [])
        self.assertEqual(
            {node.get("name") for node in ext.findall("./require/type")},
            {
                "VkPhysicalDeviceDepthClipControlFeaturesEXT",
                "VkPipelineViewportDepthClipControlCreateInfoEXT",
            },
        )
        self.assertEqual(
            {
                (node.get("name"), node.get("struct"))
                for node in ext.findall("./require/feature")
            },
            {
                (
                    "depthClipControl",
                    "VkPhysicalDeviceDepthClipControlFeaturesEXT",
                )
            },
        )

    def test_pipeline_consumes_negative_one_to_one_and_defaults_to_false(self):
        self.assertRegex(
            self.pipeline,
            r"vp_info\s*\?\s*vk_find_struct_const\(\s*vp_info->pNext,\s*"
            r"PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT\)",
        )
        self.assertRegex(
            self.pipeline,
            r"depth_clip_negative_one_to_one\s*=\s*"
            r"depth_clip_control\s*&&\s*"
            r"depth_clip_control->negativeOneToOne",
        )

    def test_lowering_runs_only_on_last_raster_stage_before_viewport_z_flip(self):
        options = re.search(r"struct dzn_nir_options\s*\{([^}]+)\}", self.pipeline)
        self.assertIsNotNone(options)
        self.assertIn("bool lower_clip_halfz;", options.group(1))
        self.assertRegex(
            self.pipeline,
            r"\.lower_clip_halfz\s*=\s*stage\s*==\s*last_raster_stage\s*&&\s*"
            r"depth_clip_negative_one_to_one",
        )
        get_nir = function_body(self.pipeline, "dzn_pipeline_get_nir_shader")
        self.assertIn("if (options->lower_clip_halfz)", get_nir)
        self.assertLess(
            get_nir.index("nir_lower_clip_halfz"),
            get_nir.index("dxil_spirv_nir_passes"),
        )

    def test_synthetic_geometry_shader_is_converted_before_z_flip(self):
        create_gs = self.pipeline.index(
            "dzn_nir_polygon_point_mode_gs(previous, &gs_info)"
        )
        halfz = self.pipeline.index(
            "NIR_PASS(_, pipeline->templates.shaders[MESA_SHADER_GEOMETRY].nir,\n"
            "                  nir_lower_clip_halfz)",
            create_gs,
        )
        z_flip = self.pipeline.index(
            "dxil_spirv_nir_lower_yz_flip", create_gs
        )
        self.assertLess(create_gs, halfz)
        self.assertLess(halfz, z_flip)

    def test_pipeline_and_shader_cache_keys_include_clip_mode(self):
        self.assertGreaterEqual(
            self.pipeline.count("&depth_clip_negative_one_to_one"), 3
        )
        self.assertIn(
            "_mesa_blake3_update(&pipeline_hash_ctx, &depth_clip_negative_one_to_one,",
            self.pipeline,
        )
        self.assertIn(
            "_mesa_blake3_update(&nir_hash_ctx, &depth_clip_negative_one_to_one,",
            self.pipeline,
        )
        self.assertIn(
            "_mesa_blake3_update(&dxil_hash_ctx, &depth_clip_negative_one_to_one,",
            self.pipeline,
        )

    def test_nir_transform_maps_minus_one_to_one_to_direct3d_zero_to_one(self):
        lower = function_body(self.nir, "lower_pos_write")
        self.assertIn("nir_channel(b, pos, 2)", lower)
        self.assertIn("nir_channel(b, pos, 3)", lower)
        self.assertIn("nir_fadd(b", lower)
        self.assertIn("nir_fmul_imm(b", lower)
        self.assertRegex(lower, r"nir_fmul_imm\(b,\s*nir_fadd\(b,[\s\S]*?0\.5\)")


if __name__ == "__main__":
    unittest.main()

