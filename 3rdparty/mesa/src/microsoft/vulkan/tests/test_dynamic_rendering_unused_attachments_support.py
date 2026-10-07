#!/usr/bin/env python3
"""Check Dozen's support for unused dynamic-rendering attachments."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DIR = TEST_DIR.parent
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


class DynamicRenderingUnusedAttachmentsSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.cmd_buffer = (DZN_DIR / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_registry_extension_requires_dynamic_rendering_and_one_feature(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_dynamic_rendering_unused_attachments']"
        )
        self.assertIsNotNone(ext)
        self.assertIn("VK_KHR_dynamic_rendering", ext.get("depends", ""))
        self.assertEqual(
            [feature.get("name") for feature in ext.findall("./require/feature")],
            ["dynamicRenderingUnusedAttachments"],
        )
        self.assertEqual(ext.findall("./require/command"), [])

    def test_extension_and_feature_are_enabled_in_dozen(self):
        self.assertIn(".KHR_dynamic_rendering                 = true,", self.device)
        self.assertIn(".EXT_dynamic_rendering_unused_attachments = true,", self.device)
        self.assertIn(".dynamicRendering                   = true,", self.device)
        self.assertIn(".dynamicRenderingUnusedAttachments  = true,", self.device)

    def test_null_color_attachment_uses_null_rtv_and_skips_image_access(self):
        body = function_body(self.cmd_buffer, "dzn_CmdBeginRendering")
        null_color = body.index("if (!iview)")
        null_branch = body[null_color : body.index("struct dzn_image *img", null_color)]
        self.assertIn("dzn_cmd_buffer_get_null_rtv(cmdbuf)", null_branch)
        self.assertIn("continue;", null_branch)
        self.assertIn("dzn_rendering_attachment_initial_transition", body)
        self.assertIn("if (iview != NULL && dzn_attachment_load_op_needs_clear", body)

    def test_null_attachment_is_not_resolved_at_rendering_end(self):
        body = function_body(
            self.cmd_buffer, "dzn_cmd_buffer_resolve_rendering_attachment"
        )
        self.assertIn("if (!src || !dst || att->resolve.mode == VK_RESOLVE_MODE_NONE)", body)


if __name__ == "__main__":
    unittest.main()

