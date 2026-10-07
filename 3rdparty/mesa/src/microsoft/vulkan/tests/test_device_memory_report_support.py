#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_device_memory_report support path."""
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


class DeviceMemoryReportSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.private = (DZN_DIR / "dzn_private.h").read_text(encoding="utf-8")
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_and_feature_are_reported(self):
        self.assertRegex(self.device, r"\.EXT_device_memory_report\s*=\s*true\b")
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertRegex(features, r"\.deviceMemoryReport\s*=\s*true\b")
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_device_memory_report']"
        )
        self.assertIsNotNone(ext)
        self.assertIsNotNone(
            ext.find("./require/feature[@name='deviceMemoryReport']")
        )
        self.assertIsNotNone(
            ext.find(
                "./require/type[@name='VkDeviceDeviceMemoryReportCreateInfoEXT']"
            )
        )
        self.assertEqual(ext.findall("./require/command"), [])

    def test_each_allocation_receives_a_unique_monotonic_report_id(self):
        self.assertIn("uint64_t memory_report_next_id;", self.private)
        self.assertIn("uint64_t memory_report_id;", self.private)
        allocation = function_body(self.device, "dzn_device_memory_create")
        self.assertIn(
            "p_atomic_inc_return(&device->memory_report_next_id)", allocation
        )
        self.assertIn("mem->memory_report_id", allocation)

    def test_callback_data_is_emitted_with_memory_identity_and_heap(self):
        emit = function_body(self.device, "dzn_device_memory_emit_report")
        self.assertIn("device->vk.memory_reports", emit)
        self.assertIn("vk_emit_device_memory_report", emit)
        self.assertIn("mem->memory_report_id", emit)
        self.assertIn("VK_OBJECT_TYPE_DEVICE_MEMORY", emit)
        self.assertIn("heap_index", emit)

    def test_allocation_reports_success_import_and_failure(self):
        allocate = function_body(self.device, "dzn_AllocateMemory")
        self.assertIn("dzn_device_memory_create", allocate)
        self.assertIn("VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT", allocate)
        self.assertIn("VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT", allocate)
        self.assertIn(
            "VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT", allocate
        )
        self.assertIn("pAllocateInfo->allocationSize", allocate)
        self.assertIn("pdevice->memory.memoryTypes", allocate)

    def test_free_reports_free_or_unimport_before_destroy(self):
        free = function_body(self.device, "dzn_FreeMemory")
        self.assertIn("VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT", free)
        self.assertIn("VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT", free)
        self.assertLess(
            free.index("dzn_device_memory_emit_report"),
            free.index("dzn_device_memory_destroy"),
        )

    def test_import_state_is_saved_for_win32_fd_and_host_pointer_paths(self):
        self.assertIn("bool imported;", self.private)
        allocation = function_body(self.device, "dzn_device_memory_create")
        self.assertIn("bool imported_memory = false", allocation)
        self.assertGreaterEqual(allocation.count("imported_memory = true"), 2)
        self.assertIn("imported_memory = import_handle != NULL || import_name != NULL", allocation)
        self.assertIn("mem->imported = imported_memory", allocation)


if __name__ == "__main__":
    unittest.main()

