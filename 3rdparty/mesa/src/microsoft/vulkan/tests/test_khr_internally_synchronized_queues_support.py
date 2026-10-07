#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify VK_KHR_internally_synchronized_queues support in Dozen."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
VK_QUEUE_H = MESA_ROOT / "src/vulkan/runtime/vk_queue.h"
VK_QUEUE_C = MESA_ROOT / "src/vulkan/runtime/vk_queue.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source, name):
    match = re.search(
        r"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\n" + re.escape(name) + r"\s*\(",
        source,
    )
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


class InternallySynchronizedQueuesSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.queue_h = VK_QUEUE_H.read_text(encoding="utf-8")
        cls.queue_c = VK_QUEUE_C.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_are_reported(self):
        self.assertRegex(
            self.device,
            r"\.KHR_internally_synchronized_queues\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.internallySynchronizedQueues\s*=\s*true\b",
        )

    def test_registry_defines_the_feature_and_queue_create_flag(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_KHR_internally_synchronized_queues']"
        )
        self.assertIsNotNone(extension)
        self.assertIn("VK_VERSION_1_1", extension.get("depends", ""))
        self.assertIsNotNone(
            extension.find(
                ".//enum[@name='VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR']"
            )
        )
        self.assertIsNotNone(
            extension.find(
                ".//type[@name='VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR']"
            )
        )

    def test_create_device_accepts_only_the_implemented_queue_flag(self):
        body = function_body(self.device, "dzn_CreateDevice")
        self.assertIn(
            "flags &\n          ~VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR",
            body,
        )
        self.assertIn("VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME", body)
        self.assertIn("PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR", body)
        self.assertIn("features2->pNext", body)
        self.assertIn("VK_ERROR_EXTENSION_NOT_PRESENT", body)
        self.assertIn("VK_ERROR_FEATURE_NOT_PRESENT", body)
        self.assertNotIn("pQueueCreateInfos[i].flags != 0", body)

    def test_common_queue_runtime_preserves_flag_and_locks_driver_submission(self):
        dozen_init = function_body(self.device, "dzn_queue_init")
        self.assertIn(
            "vk_queue_init(&queue->vk, &device->vk, pCreateInfo, index_in_family)",
            dozen_init,
        )
        init = function_body(self.queue_c, "vk_queue_init")
        self.assertIn("queue->flags = pCreateInfo->flags", init)
        submit = function_body(self.queue_c, "vk_queue_submit_final")
        lock = submit.index("vk_queue_lock(queue)")
        driver_submit = submit.index("queue->driver_submit(queue, submit)")
        unlock = submit.index("vk_queue_unlock(queue)")
        self.assertLess(lock, driver_submit)
        self.assertLess(driver_submit, unlock)
        self.assertIn(
            "VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR",
            self.queue_h,
        )

    def test_queue_flags_outside_the_supported_mask_remain_rejected(self):
        body = function_body(self.device, "dzn_CreateDevice")
        self.assertIn(
            "flags &\n          ~VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR",
            body,
        )


if __name__ == "__main__":
    unittest.main()

