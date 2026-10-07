#!/usr/bin/env python3
"""Verify Dozen's direct-resource implementation of EXT/KHR load_store_op_none."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_CMD_BUFFER = TEST_DIR.parent / "dzn_cmd_buffer.c"
VK_RENDER_PASS = MESA_ROOT / "src/vulkan/runtime/vk_render_pass.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


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


class LoadStoreOpNoneSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.cmd_buffer = DZN_CMD_BUFFER.read_text(encoding="utf-8")
        cls.render_pass = VK_RENDER_PASS.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_is_advertised_without_fabricated_feature_or_command(self):
        self.assertRegex(self.device, r"\.EXT_load_store_op_none\s*=\s*true\b")
        self.assertRegex(self.device, r"\.KHR_load_store_op_none\s*=\s*true\b")
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_load_store_op_none']"
        )
        self.assertIsNotNone(ext)
        self.assertEqual(ext.get("nofeatures"), "true")
        self.assertEqual(ext.findall("./require/feature"), [])
        self.assertEqual(ext.findall("./require/command"), [])
        enum_names = {node.get("name") for node in ext.findall("./require/enum")}
        self.assertIn("VK_ATTACHMENT_LOAD_OP_NONE_EXT", enum_names)
        self.assertIn("VK_ATTACHMENT_STORE_OP_NONE_EXT", enum_names)
        self.assertNotRegex(self.device, r"\.loadStoreOpNone\s*=")

        khr = self.registry.find(
            "./extensions/extension[@name='VK_KHR_load_store_op_none']"
        )
        self.assertIsNotNone(khr)
        self.assertEqual(khr.get("nofeatures"), "true")
        self.assertEqual(khr.get("promotedto"), "VK_VERSION_1_4")
        self.assertEqual(khr.findall("./require/feature"), [])
        self.assertEqual(khr.findall("./require/command"), [])
        khr_enum_names = {node.get("name") for node in khr.findall("./require/enum")}
        self.assertIn("VK_ATTACHMENT_LOAD_OP_NONE_KHR", khr_enum_names)
        self.assertIn("VK_ATTACHMENT_STORE_OP_NONE_KHR", khr_enum_names)

    def test_load_none_is_an_explicit_no_load_path_and_not_d3d_no_access(self):
        load_op = function_body(self.cmd_buffer, "dzn_attachment_load_op_needs_clear")
        self.assertRegex(
            load_op,
            r"case\s+VK_ATTACHMENT_LOAD_OP_NONE\s*:\s*return false\s*;",
        )
        for op in (
            "VK_ATTACHMENT_LOAD_OP_LOAD",
            "VK_ATTACHMENT_LOAD_OP_DONT_CARE",
            "VK_ATTACHMENT_LOAD_OP_NONE",
        ):
            self.assertIn(op, load_op)
        begin = function_body(self.cmd_buffer, "dzn_CmdBeginRendering")
        self.assertEqual(begin.count("dzn_attachment_load_op_needs_clear("), 3)
        self.assertIn("OMSetRenderTargets", begin)
        self.assertIn("NONE must not become D3D12 NO_ACCESS", begin)
        self.assertNotIn("D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS", begin)
        self.assertIsNone(
            re.search(r"ID3D12GraphicsCommandList\d+_BeginRenderPass", self.cmd_buffer)
        )

    def test_store_none_has_no_separate_store_stage_and_resolves_remain_independent(self):
        store_op = function_body(
            self.cmd_buffer, "dzn_cmd_buffer_end_rendering_store_op"
        )
        for op in (
            "VK_ATTACHMENT_STORE_OP_STORE",
            "VK_ATTACHMENT_STORE_OP_DONT_CARE",
            "VK_ATTACHMENT_STORE_OP_NONE",
        ):
            self.assertIn("case " + op, store_op)
        end = function_body(self.cmd_buffer, "dzn_CmdEndRendering")
        self.assertEqual(end.count("dzn_cmd_buffer_end_rendering_store_op("), 3)
        self.assertEqual(end.count("dzn_cmd_buffer_resolve_rendering_attachment("), 3)
        self.assertLess(
            end.index("dzn_cmd_buffer_end_rendering_store_op("),
            end.index("dzn_cmd_buffer_resolve_rendering_attachment("),
        )

    def test_common_render_pass_path_preserves_none_and_skips_explicit_load(self):
        self.assertIn("color_attachment->loadOp = rp_att->load_op;", self.render_pass)
        self.assertIn("depth_attachment.loadOp = rp_att->load_op;", self.render_pass)
        load = function_body(self.render_pass, "load_attachment")
        self.assertIn("rp_att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR", load)
        self.assertIn("if (!need_load_store)\n      return;", load)
        self.assertIn("rp_att->store_op", self.render_pass)


if __name__ == "__main__":
    unittest.main()

