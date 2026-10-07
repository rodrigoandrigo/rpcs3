#!/usr/bin/env python3
"""Source-level regression test for Dozen's VK_KHR_copy_commands2 support.

This verifies the advertised extension is backed by all six command handlers.
It is not a substitute for compiling the ICD or running Vulkan CTS.
"""
from pathlib import Path
import re
import unittest

DOZEN_DIR = Path(__file__).resolve().parents[1]
DEVICE_SOURCE = DOZEN_DIR / "dzn_device.c"
COMMAND_SOURCE = DOZEN_DIR / "dzn_cmd_buffer.c"

REQUIRED_HANDLERS = (
    "dzn_CmdBlitImage2",
    "dzn_CmdCopyBuffer2",
    "dzn_CmdCopyBufferToImage2",
    "dzn_CmdCopyImage2",
    "dzn_CmdCopyImageToBuffer2",
    "dzn_CmdResolveImage2",
)


class CopyCommands2SupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device_source = DEVICE_SOURCE.read_text(encoding="utf-8")
        cls.command_source = COMMAND_SOURCE.read_text(encoding="utf-8")

    def test_extension_is_enabled_unconditionally(self):
        self.assertRegex(
            self.device_source,
            r"\.KHR_copy_commands2\s*=\s*true\b",
            "Dozen must not advertise copy_commands2 without its support gate",
        )

    def test_all_six_extension_commands_have_driver_handlers(self):
        for handler in REQUIRED_HANDLERS:
            with self.subTest(handler=handler):
                self.assertRegex(
                    self.command_source,
                    rf"VKAPI_ATTR\s+void\s+VKAPI_CALL\s+{handler}\s*\(",
                    f"Missing required VK_KHR_copy_commands2 handler: {handler}",
                )


if __name__ == "__main__":
    unittest.main()

