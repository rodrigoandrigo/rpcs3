#!/usr/bin/env python3
"""Check Dozen's VK_EXT_hdr_metadata forwarding to Win32 DXGI."""
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


class HdrMetadataSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.wsi_win32 = (MESA_ROOT / "src/vulkan/wsi/wsi_common_win32.cpp").read_text(encoding="utf-8")
        cls.wsi_common = (MESA_ROOT / "src/vulkan/wsi/wsi_common.c").read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_depends_on_swapchain_and_has_no_feature_bit(self):
        ext = self.registry.find("./extensions/extension[@name='VK_EXT_hdr_metadata']")
        self.assertIsNotNone(ext)
        self.assertIn("VK_KHR_swapchain", ext.get("depends", ""))
        self.assertEqual(ext.findall("./require/feature"), [])
        self.assertEqual(
            [command.get("name") for command in ext.findall("./require/command")],
            ["vkSetHdrMetadataEXT"],
        )

    def test_dozen_gate_matches_win32_wsi_and_swapchain_dependency(self):
        self.assertIn(".KHR_swapchain                         = true,", self.device)
        self.assertIn(
            "#if defined(VK_USE_PLATFORM_WIN32_KHR) && !defined(_XBOX_UWP)",
            self.device,
        )
        self.assertIn(".EXT_hdr_metadata                      = true,", self.device)

    def test_common_wsi_command_applies_metadata_to_each_swapchain(self):
        body = function_body(self.wsi_common, "wsi_SetHdrMetadataEXT")
        self.assertIn("for (uint32_t i = 0; i < swapchainCount; i++)", body)
        self.assertIn("swapchain->set_hdr_metadata(swapchain, &pMetadata[i])", body)
        self.assertIn("&wsi_device_entrypoints", self.device)

    def test_win32_path_converts_vulkan_units_and_calls_dxgi_hdr10(self):
        body = function_body(self.wsi_win32, "wsi_win32_swapchain_set_hdr_metadata")
        self.assertIn("DXGI_HDR_METADATA_HDR10", body)
        self.assertIn("metadata->displayPrimaryRed.x", body)
        self.assertIn("metadata->displayPrimaryGreen.y", body)
        self.assertIn("metadata->displayPrimaryBlue.x", body)
        self.assertIn("metadata->whitePoint.y", body)
        self.assertIn("metadata->maxLuminance, 1.0", body)
        self.assertIn("metadata->minLuminance, 10000.0", body)
        self.assertIn("metadata->maxContentLightLevel", body)
        self.assertIn("metadata->maxFrameAverageLightLevel", body)
        self.assertIn("DXGI_HDR_METADATA_TYPE_HDR10", body)
        self.assertIn("SetHDRMetaData", body)
        self.assertIn("IDXGISwapChain4", self.wsi_win32)

    def test_callback_is_registered_only_for_a_dxgi_swapchain(self):
        body = function_body(self.wsi_win32, "wsi_win32_surface_create_swapchain")
        self.assertIn("image_params->image_type == WSI_IMAGE_TYPE_DXGI", body)
        self.assertIn("chain->base.set_hdr_metadata = wsi_win32_swapchain_set_hdr_metadata", body)

    def test_wsi_does_not_claim_to_select_the_color_space(self):
        body = function_body(self.wsi_win32, "wsi_win32_swapchain_set_hdr_metadata")
        self.assertNotIn("SetColorSpace1", body)


if __name__ == "__main__":
    unittest.main()

