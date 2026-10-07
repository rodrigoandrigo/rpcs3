#!/usr/bin/env python3
"""Verify the host-only deferred-operation extension uses complete common handlers."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DEVICE_RUNTIME = MESA_ROOT / "src/vulkan/runtime/vk_device.c"
DEFERRED_RUNTIME = MESA_ROOT / "src/vulkan/runtime/vk_deferred_operation.c"
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


class KhrDeferredHostOperationsSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.device_runtime = DEVICE_RUNTIME.read_text(encoding="utf-8")
        cls.deferred_runtime = DEFERRED_RUNTIME.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_is_host_only_and_requires_all_five_commands(self):
        ext = self.registry.find(
            "./extensions/extension[@name='VK_KHR_deferred_host_operations']"
        )
        self.assertIsNotNone(ext)
        self.assertEqual(ext.get("nofeatures"), "true")
        self.assertIsNone(ext.get("depends"))
        expected = {
            "vkCreateDeferredOperationKHR",
            "vkDestroyDeferredOperationKHR",
            "vkGetDeferredOperationMaxConcurrencyKHR",
            "vkGetDeferredOperationResultKHR",
            "vkDeferredOperationJoinKHR",
        }
        actual = {command.get("name") for command in ext.findall("./require/command")}
        self.assertEqual(actual, expected)
        self.assertRegex(
            self.device,
            r"\.KHR_deferred_host_operations\s*=\s*true\b",
        )

    def test_vk_device_init_installs_common_device_entrypoints(self):
        body = function_body(self.device_runtime, "vk_device_init")
        self.assertIn("&vk_common_device_entrypoints", body)
        self.assertIn("vk_device_dispatch_table_from_entrypoints", body)
        self.assertIn("false", body)

    def test_common_runtime_implements_object_lifecycle_and_no_work_state(self):
        required = (
            "vk_common_CreateDeferredOperationKHR",
            "vk_common_DestroyDeferredOperationKHR",
            "vk_common_GetDeferredOperationMaxConcurrencyKHR",
            "vk_common_GetDeferredOperationResultKHR",
            "vk_common_DeferredOperationJoinKHR",
        )
        for name in required:
            with self.subTest(handler=name):
                self.assertIn(name, self.deferred_runtime)

        create = function_body(self.deferred_runtime, "vk_common_CreateDeferredOperationKHR")
        self.assertIn("vk_alloc2", create)
        self.assertIn("VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR", create)
        self.assertIn("VK_ERROR_OUT_OF_HOST_MEMORY", create)
        self.assertIn("vk_deferred_operation_to_handle", create)

        destroy = function_body(self.deferred_runtime, "vk_common_DestroyDeferredOperationKHR")
        self.assertIn("vk_object_base_finish", destroy)
        self.assertIn("vk_free2", destroy)

        concurrency = function_body(
            self.deferred_runtime,
            "vk_common_GetDeferredOperationMaxConcurrencyKHR",
        )
        self.assertRegex(concurrency, r"return\s+1\s*;")
        result = function_body(self.deferred_runtime, "vk_common_GetDeferredOperationResultKHR")
        join = function_body(self.deferred_runtime, "vk_common_DeferredOperationJoinKHR")
        self.assertIn("return VK_SUCCESS", result)
        self.assertIn("return VK_SUCCESS", join)


if __name__ == "__main__":
    unittest.main()

