#!/usr/bin/env python3
"""Verify Dozen's EXT_host_query_reset compatibility wiring."""
from pathlib import Path
import re
import unittest

DZN_DIR = Path(__file__).resolve().parents[1]
MESA_ROOT = Path(__file__).resolve().parents[4]
DEVICE_SOURCE = DZN_DIR / "dzn_device.c"
QUERY_SOURCE = DZN_DIR / "dzn_query.c"
REGISTRY_SOURCE = MESA_ROOT / "src/vulkan/registry/vk.xml"


class HostQueryResetSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DEVICE_SOURCE.read_text(encoding="utf-8")
        cls.query = QUERY_SOURCE.read_text(encoding="utf-8")
        cls.registry = REGISTRY_SOURCE.read_text(encoding="utf-8")

    def test_extension_is_backed_by_core_12_feature_and_api(self):
        self.assertRegex(self.device, r"DZN_API_VERSION\s+VK_MAKE_VERSION\(1,\s*2,")
        self.assertRegex(self.device, r"\.EXT_host_query_reset\s*=\s*true\b")
        self.assertRegex(self.device, r"\.hostQueryReset\s*=\s*true\b")

    def test_host_reset_command_is_implemented(self):
        self.assertRegex(
            self.query,
            r"VKAPI_ATTR\s+void\s+VKAPI_CALL\s+dzn_ResetQueryPool\s*\(",
        )

    def test_ext_command_is_an_alias_of_the_core_reset(self):
        self.assertRegex(
            self.registry,
            r'<command name="vkResetQueryPoolEXT" alias="vkResetQueryPool"\s*/>',
        )


if __name__ == "__main__":
    unittest.main()

