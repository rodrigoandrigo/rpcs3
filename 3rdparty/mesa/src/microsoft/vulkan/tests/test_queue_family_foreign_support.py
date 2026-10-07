#!/usr/bin/env python3
"""Check Dozen's external/foreign queue-family ownership handoff paths."""
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


class QueueFamilyForeignSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.cmd_buffer = (DZN_DIR / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_registry_dependency_and_no_new_commands_or_features(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_queue_family_foreign']"
        )
        self.assertIsNotNone(ext)
        self.assertIn("VK_KHR_external_memory", ext.get("depends", ""))
        self.assertEqual(ext.findall("./require/command"), [])
        self.assertEqual(ext.findall("./require/feature"), [])
        enum_names = {
            item.get("name")
            for item in ext.findall("./require/enum")
        }
        self.assertIn("VK_QUEUE_FAMILY_FOREIGN_EXT", enum_names)
        self.assertIn(".KHR_external_memory                   = true,", self.device)
        self.assertIn("VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT", self.device)
        self.assertIn("VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT", self.device)
        self.assertIn("ID3D12Device_OpenSharedHandle", self.device)
        self.assertRegex(self.device, r"\.EXT_queue_family_foreign\s*=\s*true\b")

    def test_transfer_classifier_recognizes_external_and_foreign(self):
        body = function_body(self.cmd_buffer, "dzn_queue_family_transfer_kind")
        self.assertIn("VK_QUEUE_FAMILY_EXTERNAL", body)
        self.assertIn("VK_QUEUE_FAMILY_FOREIGN_EXT", body)
        self.assertIn("DZN_QUEUE_FAMILY_TRANSFER_EXTERNAL", body)

    def test_legacy_image_handoff_splits_at_common_layout(self):
        body = function_body(self.cmd_buffer, "dzn_CmdPipelineBarrier2")
        self.assertIn("dzn_queue_family_transfer_kind(ibarrier->srcQueueFamilyIndex", body)
        self.assertIn("if (is_release)", body)
        self.assertIn("new_layout = VK_IMAGE_LAYOUT_GENERAL", body)
        self.assertIn("old_layout = VK_IMAGE_LAYOUT_GENERAL", body)
        self.assertIn("DZN_QUEUE_TRANSITION_FLUSH", body)

    def test_legacy_buffer_handoff_flushes_and_uses_submission_decay(self):
        body = function_body(self.cmd_buffer, "dzn_CmdPipelineBarrier2")
        self.assertIn("D3D12 buffers decay to COMMON when ExecuteCommandLists completes", body)
        self.assertIn("barrier.UAV.pResource = NULL", body)
        self.assertIn("transfer_kind == DZN_QUEUE_FAMILY_TRANSFER_EXTERNAL", body)

    def test_enhanced_buffers_images_and_linear_images_have_release_acquire_scopes(self):
        body = function_body(self.cmd_buffer, "dzn_CmdPipelineBarrier2_enhanced")
        self.assertIn("dzn_queue_family_transfer_kind(vk_barrier->srcQueueFamilyIndex", body)
        self.assertIn("SyncAfter = D3D12_BARRIER_SYNC_NONE", body)
        self.assertIn("SyncBefore = D3D12_BARRIER_SYNC_NONE", body)
        self.assertIn("buffer_barriers[i].AccessAfter = D3D12_BARRIER_ACCESS_COMMON", body)
        self.assertIn("buffer_barriers[i].AccessBefore = D3D12_BARRIER_ACCESS_COMMON", body)
        self.assertIn("D3D12_BARRIER_LAYOUT_COMMON", body)
        self.assertIn("External and FOREIGN queues meet D3D12 through COMMON", body)


if __name__ == "__main__":
    unittest.main()

