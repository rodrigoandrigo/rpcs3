#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_private_data wiring to Mesa's generic runtime."""
from pathlib import Path
import re
import unittest

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]
DEVICE_SOURCE = DZN_DIR / "dzn_device.c"
OBJECT_SOURCE = MESA_ROOT / "src/vulkan/runtime/vk_object.c"
REGISTRY_SOURCE = MESA_ROOT / "src/vulkan/registry/vk.xml"

COMMON_HANDLERS = (
    "vk_common_CreatePrivateDataSlot",
    "vk_common_DestroyPrivateDataSlot",
    "vk_common_SetPrivateData",
    "vk_common_GetPrivateData",
)


class PrivateDataSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DEVICE_SOURCE.read_text(encoding="utf-8")
        cls.runtime = OBJECT_SOURCE.read_text(encoding="utf-8")
        cls.registry = REGISTRY_SOURCE.read_text(encoding="utf-8")

    def test_extension_and_feature_are_advertised(self):
        self.assertRegex(self.device, r"\.EXT_private_data\s*=\s*true\b")
        self.assertRegex(self.device, r"\.privateData\s*=\s*true\b")

    def test_common_runtime_implements_all_extension_commands(self):
        for handler in COMMON_HANDLERS:
            with self.subTest(handler=handler):
                self.assertRegex(
                    self.runtime,
                    rf"\b{handler}\s*\(",
                    f"Missing Mesa common private-data handler: {handler}",
                )

    def test_ext_commands_alias_the_core_handlers(self):
        aliases = {
            "vkCreatePrivateDataSlotEXT": "vkCreatePrivateDataSlot",
            "vkDestroyPrivateDataSlotEXT": "vkDestroyPrivateDataSlot",
            "vkSetPrivateDataEXT": "vkSetPrivateData",
            "vkGetPrivateDataEXT": "vkGetPrivateData",
        }
        for ext_command, core_command in aliases.items():
            with self.subTest(command=ext_command):
                self.assertRegex(
                    self.registry,
                    rf'<command name="{ext_command}" alias="{core_command}"\s*/>',
                )

    def test_dozen_installs_common_device_entrypoints_and_object_storage(self):
        self.assertIn("&vk_common_device_entrypoints", self.device)
        self.assertIn("vk_object_base_init", self.runtime)
        self.assertRegex(
            self.runtime,
            r"util_sparse_array_init\(&base->private_data",
        )


if __name__ == "__main__":
    unittest.main()

