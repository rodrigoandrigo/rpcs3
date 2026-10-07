#!/usr/bin/env python3
"""Verify Dozen's spec-compliant no-op path for the external-memory acquire hint."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DIR = TEST_DIR.parent
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"
VK_SYNC = MESA_ROOT / "src/vulkan/runtime/vk_synchronization.c"


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


class ExternalMemoryAcquireUnmodifiedSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.cmd_buffer = (DZN_DIR / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.sync = VK_SYNC.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_dependency_and_payload_match_registry(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_external_memory_acquire_unmodified']"
        )
        self.assertIsNotNone(ext)
        self.assertIn("VK_KHR_external_memory", ext.get("depends", ""))
        self.assertEqual(ext.findall("./require/command"), [])
        self.assertEqual(ext.findall("./require/feature"), [])
        type_names = {node.get("name") for node in ext.findall("./require/type")}
        self.assertIn("VkExternalMemoryAcquireUnmodifiedEXT", type_names)
        self.assertIn(
            ".KHR_external_memory                   = true,", self.device
        )
        self.assertRegex(
            self.device,
            r"\.EXT_external_memory_acquire_unmodified\s*=\s*true\b",
        )

    def test_legacy_barrier_upgrade_preserves_extension_pnext(self):
        for name in (
            "upgrade_buffer_memory_barrier",
            "upgrade_image_memory_barrier",
        ):
            body = function_body(self.sync, name)
            self.assertIn(".pNext", body)
            self.assertIn("barrier->pNext", body)

    def test_hint_never_suppresses_required_d3d12_barriers(self):
        body = function_body(self.cmd_buffer, "dzn_CmdPipelineBarrier2")
        self.assertIn("VkExternalMemoryAcquireUnmodifiedEXT", body)
        self.assertIn("optional performance hint", body)
        self.assertIn("keep issuing the", body)
        self.assertIn("for (uint32_t i = 0; i < info->bufferMemoryBarrierCount; i++)", body)
        self.assertIn("for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++)", body)
        self.assertNotIn("acquireUnmodifiedMemory", body)

    def test_enhanced_barriers_keep_the_common_ownership_handoff(self):
        body = function_body(self.cmd_buffer, "dzn_CmdPipelineBarrier2_enhanced")
        self.assertIn("VkExternalMemoryAcquireUnmodifiedEXT", body)
        self.assertIn("Keep the COMMON ownership handoff", body)
        self.assertIn("LayoutBefore = is_acquire", body)
        self.assertIn("LayoutAfter = is_release", body)
        self.assertNotIn("acquireUnmodifiedMemory", body)


if __name__ == "__main__":
    unittest.main()

