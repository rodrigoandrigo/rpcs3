#!/usr/bin/env python3
"""Verify Dozen's end-to-end VK_EXT_color_write_enable implementation."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]


def function_body(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", source)
    if match is None:
        raise AssertionError(f"function not found: {name}")
    start = match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos]
    raise AssertionError(f"unclosed function: {name}")


class ColorWriteEnableSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.pipeline = (DZN_DIR / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.cmd_buffer = (DZN_DIR / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.private = (DZN_DIR / "dzn_private.h").read_text(encoding="utf-8")
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_feature_and_command_are_registered(self):
        self.assertRegex(self.device, r"\.EXT_color_write_enable\s*=\s*true\b")
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertRegex(features, r"\.colorWriteEnable\s*=\s*true\b")
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_color_write_enable']"
        )
        self.assertIsNotNone(extension)
        self.assertIsNotNone(extension.find("./require/command[@name='vkCmdSetColorWriteEnableEXT']"))
        self.assertIsNotNone(extension.find("./require/feature[@name='colorWriteEnable']"))

    def test_dynamic_state_creates_variants_and_consumes_pipeline_pnext(self):
        create = function_body(self.pipeline, "dzn_graphics_pipeline_create")
        self.assertIn("VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT", create)
        self.assertIn("pipeline->blend.dynamic_color_write_enable = true", create)
        self.assertIn("dzn_graphics_pipeline_prepare_for_variants(device, pipeline)", create)

        translate = function_body(self.pipeline, "dzn_graphics_pipeline_translate_blend")
        self.assertIn("PIPELINE_COLOR_WRITE_CREATE_INFO_EXT", translate)
        self.assertIn("desc->IndependentBlendEnable = true", translate)
        self.assertIn("color_write_info && !pipeline->blend.dynamic_color_write_enable", translate)
        self.assertIn("RenderTargetWriteMask", translate)
        self.assertIn("color_write_info->pColorWriteEnables[i]", translate)

        get_state = function_body(self.pipeline, "dzn_graphics_pipeline_get_state_locked")
        self.assertIn("masked_key.color_write_enables = key->color_write_enables", get_state)
        self.assertIn("blend->RenderTarget[i].RenderTargetWriteMask = 0", get_state)
        self.assertIn("BITFIELD_BIT(i)", get_state)

    def test_command_updates_common_state_and_invalidates_graphics_pipeline(self):
        command = function_body(self.cmd_buffer, "dzn_CmdSetColorWriteEnableEXT")
        self.assertIn("vk_common_CmdSetColorWriteEnableEXT", command)
        self.assertIn("dynamic_graphics_state.cb.color_write_enables", command)
        self.assertIn("DZN_CMD_BINDPOINT_DIRTY_PIPELINE", command)
        self.assertIn("uint8_t color_write_enables", self.private)
        self.assertIn("bool dynamic_color_write_enable", self.private)
        self.assertIn("uint32_t blend;", self.private)


if __name__ == "__main__":
    unittest.main()

