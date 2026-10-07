#!/usr/bin/env python3
"""Verify Dozen's VK_EXT_device_address_binding_report support path."""
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


class DeviceAddressBindingReportSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.runtime = (MESA_ROOT / "src/vulkan/runtime/vk_debug_utils.c").read_text(
            encoding="utf-8"
        )
        cls.registry = ET.parse(MESA_ROOT / "src/vulkan/registry/vk.xml").getroot()

    def test_extension_dependencies_and_instance_support_are_satisfied(self):
        self.assertRegex(
            self.device,
            r"\.EXT_device_address_binding_report\s*=\s*true\b",
        )
        self.assertRegex(self.device, r"\.KHR_get_physical_device_properties2\s*=\s*true\b")
        self.assertRegex(self.device, r"\.EXT_debug_utils\s*=\s*true\b")
        ext = self.registry.find(
            "./extensions/extension[@name='VK_EXT_device_address_binding_report']"
        )
        self.assertIsNotNone(ext)
        self.assertIn("VK_EXT_debug_utils", ext.get("depends", ""))
        self.assertIn("VK_KHR_get_physical_device_properties2", ext.get("depends", ""))
        self.assertIsNotNone(
            ext.find("./require/feature[@name='reportAddressBinding']")
        )
        self.assertEqual(ext.findall("./require/command"), [])

    def test_feature_is_reported_for_common_feature_chain_handling(self):
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertRegex(features, r"\.reportAddressBinding\s*=\s*true\b")
        core_header = (MESA_ROOT / "include/vulkan/vulkan_core.h").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "typedef struct VkPhysicalDeviceAddressBindingReportFeaturesEXT",
            core_header,
        )
        self.assertIn("reportAddressBinding", core_header)
        runtime_device = (MESA_ROOT / "src/vulkan/runtime/vk_device.c").read_text(
            encoding="utf-8"
        )
        enabled = function_body(runtime_device, "collect_enabled_features")
        self.assertIn("vk_set_physical_device_features", enabled)

    def test_reports_only_when_the_feature_was_enabled_and_gpuva_exists(self):
        report = function_body(self.device, "dzn_buffer_emit_address_binding_report")
        self.assertIn("device->vk.enabled_features.reportAddressBinding", report)
        self.assertIn("!buffer->gpuva", report)
        self.assertIn("vk_address_binding_report", report)
        self.assertIn("&buffer->base", report)
        self.assertIn("buffer->gpuva", report)
        self.assertIn("buffer->size", report)

    def test_buffer_bind_and_unbind_events_bracket_resource_lifetime(self):
        bind = function_body(self.device, "dzn_BindBufferMemory2")
        destroy = function_body(self.device, "dzn_buffer_destroy")
        gpuva_at = bind.index("ID3D12Resource_GetGPUVirtualAddress")
        bind_report_at = bind.index("VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT")
        self.assertLess(gpuva_at, bind_report_at)
        self.assertLess(
            destroy.index("VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT"),
            destroy.index("ID3D12Resource_Release(buf->res)"),
        )
        self.assertIn("dzn_buffer_emit_address_binding_report", bind)
        self.assertIn("dzn_buffer_emit_address_binding_report", destroy)

    def test_memory_allocation_and_import_report_dedicated_buffer_gpuva(self):
        create = function_body(self.device, "dzn_device_memory_create")
        info = function_body(self.device, "dzn_device_memory_get_address_binding_info")
        self.assertIn("if (!mem->dedicated_res)", info)
        self.assertIn("dzn_ID3D12Resource_GetDesc(mem->dedicated_res)", info)
        self.assertIn("ID3D12Resource_GetGPUVirtualAddress(mem->dedicated_res)", info)
        self.assertIn("desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER", info)
        self.assertIn("*size = desc.Width", info)
        self.assertIn("ID3D12Device_OpenSharedHandle", create)
        self.assertIn("mem->dedicated_res", create)
        self.assertLess(
            create.index("dzn_device_memory_emit_address_binding_report"),
            create.index("*out = dzn_device_memory_to_handle(mem)"),
        )
        self.assertIn("mem->address_binding_reported = true", create)

    def test_memory_unbind_precedes_dedicated_resource_release(self):
        destroy = function_body(self.device, "dzn_device_memory_destroy")
        self.assertIn("if (mem->address_binding_reported)", destroy)
        self.assertLess(
            destroy.index("VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT"),
            destroy.index("ID3D12Resource_Release(mem->dedicated_res)"),
        )

    def test_report_uses_real_d3d12_resource_gpuva_not_the_tagged_bda_token(self):
        report = function_body(self.device, "dzn_buffer_emit_address_binding_report")
        get_bda = function_body(self.device, "dzn_GetBufferDeviceAddress")
        bind = function_body(self.device, "dzn_BindBufferMemory2")
        self.assertIn("buffer->gpuva", report)
        self.assertIn("ID3D12Resource_GetGPUVirtualAddress(buffer->res)", bind)
        self.assertIn("0xD3ull << 56", get_bda)
        self.assertNotIn("dzn_GetBufferDeviceAddress", report)

    def test_common_runtime_wraps_the_ext_payload_in_debug_utils_callback_data(self):
        report = function_body(self.runtime, "vk_address_binding_report")
        self.assertIn("VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT", report)
        self.assertIn("VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT", report)
        self.assertIn("object->client_visible ? 0 :", report)
        self.assertIn("VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT", report)
        self.assertIn("VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT", report)


if __name__ == "__main__":
    unittest.main()

