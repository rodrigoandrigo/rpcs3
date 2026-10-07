#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify VK_AMD_buffer_marker's D3D12 mapping and capability gate."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_CMD_BUFFER = TEST_DIR.parent / "dzn_cmd_buffer.c"
DZN_PRIVATE = TEST_DIR.parent / "dzn_private.h"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    match = re.search(r"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\n" + re.escape(name) + r"\s*\(", source)
    if match is None:
        raise AssertionError(f"function not found: {name}")
    brace = source.index("{", match.end())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unclosed function: {name}")


class AmdBufferMarkerSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.cmd_buffer = DZN_CMD_BUFFER.read_text(encoding="utf-8")
        cls.private = DZN_PRIVATE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_is_gated_on_every_exposed_d3d12_queue_type(self):
        self.assertRegex(
            self.device,
            r"\.AMD_buffer_marker\s*=\s*dzn_buffer_marker_supported\(pdev\)",
        )
        body = function_body(self.device, "dzn_buffer_marker_supported")
        self.assertIn("pdev->options3.WriteBufferImmediateSupportFlags", body)
        self.assertIn("pdev->queue_family_count", body)
        for queue_type, support_flag in (
            ("D3D12_COMMAND_LIST_TYPE_DIRECT", "D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT"),
            ("D3D12_COMMAND_LIST_TYPE_COMPUTE", "D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE"),
            ("D3D12_COMMAND_LIST_TYPE_COPY", "D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY"),
        ):
            self.assertIn(queue_type, body)
            self.assertIn(support_flag, body)
        self.assertIn("if (!(support_flags & type_flag))", body)
        self.assertIn("return false", body)

    def test_command_list2_is_queried_and_released(self):
        self.assertIn("ID3D12GraphicsCommandList2 *cmdlist2;", self.private)
        self.assertIn("IID_ID3D12GraphicsCommandList2", self.cmd_buffer)
        self.assertIn("ID3D12GraphicsCommandList2_Release(cmdbuf->cmdlist2)", self.cmd_buffer)

    def test_legacy_and_sync2_commands_share_marker_out_mapping(self):
        helper = function_body(self.cmd_buffer, "dzn_cmd_buffer_write_buffer_marker")
        self.assertIn("buffer->gpuva + dstOffset", helper)
        self.assertIn(".Value = marker", helper)
        self.assertIn("D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT", helper)
        self.assertIn("ID3D12GraphicsCommandList2_WriteBufferImmediate", helper)
        self.assertIn("assert(cmdbuf->cmdlist2)", helper)
        for command, stage in (
            ("dzn_CmdWriteBufferMarkerAMD", "VkPipelineStageFlagBits pipelineStage"),
            ("dzn_CmdWriteBufferMarker2AMD", "VkPipelineStageFlags2 stage"),
        ):
            body = function_body(self.cmd_buffer, command)
            self.assertIn(stage, self.cmd_buffer)
            self.assertIn("dzn_cmd_buffer_write_buffer_marker(cmdbuf, dstBuffer, dstOffset, marker)", body)

    def test_dozen_buffers_support_the_required_copy_dest_state(self):
        heap_body = function_body(self.device, "deduce_heap_properties_from_memory")
        buffer_body = function_body(self.device, "dzn_buffer_create")
        self.assertIn("D3D12_HEAP_TYPE_CUSTOM", heap_body)
        self.assertIn("D3D12_BARRIER_ACCESS_COPY_DEST", buffer_body)
        self.assertIn("D3D12_RESOURCE_STATE_COMMON", self.device)
        self.assertRegex(
            self.cmd_buffer,
            r"VK_ACCESS_2_TRANSFER_WRITE_BIT[\s\S]{0,400}D3D12_BARRIER_ACCESS_COPY_DEST",
        )

    def test_registry_commands_are_supplied_by_the_extension(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_AMD_buffer_marker']"
        )
        self.assertIsNotNone(extension)
        commands = {
            item.get("name")
            for item in extension.findall("./require/command")
        }
        self.assertEqual(
            commands,
            {"vkCmdWriteBufferMarkerAMD", "vkCmdWriteBufferMarker2AMD"},
        )
        self.assertEqual(extension.findall("./require/feature"), [])
        self.assertRegex(self.device, r"\.KHR_synchronization2\s*=\s*true\b")
        self.assertRegex(self.device, r"\.synchronization2\s*=\s*true\b")


if __name__ == "__main__":
    unittest.main()

