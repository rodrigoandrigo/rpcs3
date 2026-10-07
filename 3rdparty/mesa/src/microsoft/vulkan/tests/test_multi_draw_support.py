#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_multi_draw feature, limit and ordered draw handlers."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_COMMANDS = TEST_DIR.parent / "dzn_cmd_buffer.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    start = source.index(f"{name}(")
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {name}")


class MultiDrawSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.commands = DZN_COMMANDS.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_feature_and_limit_are_published(self):
        self.assertRegex(self.device, r"\.EXT_multi_draw\s*=\s*true\b")
        self.assertRegex(self.device, r"\.multiDraw\s*=\s*true\b")
        self.assertRegex(self.device, r"\.maxMultiDrawCount\s*=\s*UINT32_MAX\b")

    def test_registry_has_feature_property_and_both_commands(self):
        extension = self.registry.find("./extensions/extension[@name='VK_EXT_multi_draw']")
        self.assertIsNotNone(extension)
        required_commands = {
            command.get("name") for command in extension.findall("./require/command")
        }
        self.assertIn("vkCmdDrawMultiEXT", required_commands)
        self.assertIn("vkCmdDrawMultiIndexedEXT", required_commands)
        feature = self.registry.find(
            "./types/type[@name='VkPhysicalDeviceMultiDrawFeaturesEXT']"
        )
        properties = self.registry.find(
            "./types/type[@name='VkPhysicalDeviceMultiDrawPropertiesEXT']"
        )
        self.assertIsNotNone(feature)
        self.assertIsNotNone(properties)
        self.assertIn("multiDraw", "".join(feature.itertext()))
        self.assertIn("maxMultiDrawCount", "".join(properties.itertext()))

    def test_unindexed_multi_draw_honors_stride_and_reuses_dzn_draw(self):
        body = function_body(self.commands, "dzn_CmdDrawMultiEXT")
        self.assertIn("for (uint32_t i = 0; i < drawCount; i++)", body)
        self.assertIn("draw_data + (size_t)i * stride", body)
        self.assertRegex(
            body,
            r"dzn_CmdDraw\(commandBuffer,\s*draw->vertexCount, instanceCount,\s*draw->firstVertex, firstInstance\)",
        )

    def test_indexed_multi_draw_honors_stride_and_optional_vertex_offset(self):
        body = function_body(self.commands, "dzn_CmdDrawMultiIndexedEXT")
        self.assertIn("for (uint32_t i = 0; i < drawCount; i++)", body)
        self.assertIn("draw_data + (size_t)i * stride", body)
        self.assertIn("pVertexOffset ? *pVertexOffset : 0", body)
        self.assertIn("pVertexOffset ? vertex_offset : draw->vertexOffset", body)
        self.assertRegex(
            body,
            r"dzn_CmdDrawIndexed\(commandBuffer, draw->indexCount, instanceCount,\s*draw->firstIndex,",
        )


if __name__ == "__main__":
    unittest.main()

