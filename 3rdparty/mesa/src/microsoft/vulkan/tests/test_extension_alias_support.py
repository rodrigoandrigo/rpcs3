#!/usr/bin/env python3
"""Verify extension aliases backed by existing Dozen/D3D12 paths."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_COMMANDS = TEST_DIR.parent / "dzn_cmd_buffer.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
VK_GRAPHICS_STATE = MESA_ROOT / "src/vulkan/runtime/vk_graphics_state.c"
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


def extension(root, name):
    result = root.find(f"./extensions/extension[@name='{name}']")
    if result is None:
        raise AssertionError(f"extension not found: {name}")
    return result


def command(root, name):
    for entry in root.findall("./commands/command"):
        if entry.get("name") == name or entry.findtext("proto/name") == name:
            return entry
    raise AssertionError(f"command not found: {name}")


class AmdDrawIndirectCountAliasTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.commands = DZN_COMMANDS.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_amd_extension_is_gated_by_the_existing_khr_core_path(self):
        self.assertRegex(self.device, r"\.KHR_draw_indirect_count\s*=\s*true\b")
        self.assertRegex(self.device, r"\.AMD_draw_indirect_count\s*=\s*true\b")
        self.assertRegex(self.device, r"\.drawIndirectCount\s*=\s*true\b")
        ext = extension(self.registry, "VK_AMD_draw_indirect_count")
        self.assertEqual(ext.get("promotedto"), "VK_KHR_draw_indirect_count")
        khr = extension(self.registry, "VK_KHR_draw_indirect_count")
        self.assertEqual(khr.get("promotedto"), "VK_VERSION_1_2")

    def test_amd_commands_are_registry_aliases_of_implemented_core_commands(self):
        aliases = {
            "vkCmdDrawIndirectCountAMD": "vkCmdDrawIndirectCount",
            "vkCmdDrawIndexedIndirectCountAMD": "vkCmdDrawIndexedIndirectCount",
        }
        for alias, canonical in aliases.items():
            with self.subTest(alias=alias):
                self.assertEqual(command(self.registry, alias).get("alias"), canonical)
        for name in ("dzn_CmdDrawIndirectCount", "dzn_CmdDrawIndexedIndirectCount"):
            self.assertTrue(function_body(self.commands, name).strip())

    def test_draw_count_uses_d3d12_count_buffer_semantics(self):
        indirect = function_body(self.commands, "dzn_cmd_buffer_indirect_draw")
        self.assertIn("count_buf, count_buf_offset", indirect)
        self.assertIn("max_draw_count", indirect)
        self.assertIn("ID3D12GraphicsCommandList1_ExecuteIndirect", indirect)
        self.assertRegex(
            indirect,
            r"ExecuteIndirect\(cmdbuf->cmdlist, cmdsig,\s*max_draw_count,\s*draw_buf,\s*draw_buf_offset,\s*count_buf, count_buf_offset\)",
        )


class KhrMapMemory2AliasTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_has_no_new_feature_or_d3d12_capability_requirement(self):
        ext = extension(self.registry, "VK_KHR_map_memory2")
        self.assertEqual(ext.get("nofeatures"), "true")
        self.assertEqual(ext.get("promotedto"), "VK_VERSION_1_4")
        self.assertEqual(ext.findall("./require/feature"), [])
        commands = {entry.get("name") for entry in ext.findall("./require/command")}
        self.assertEqual(commands, {"vkMapMemory2KHR", "vkUnmapMemory2KHR"})
        self.assertRegex(self.device, r"\.KHR_map_memory2\s*=\s*true\b")
        self.assertNotRegex(self.device, r"\.EXT_map_memory_placed\s*=\s*true\b")

    def test_commands_and_structs_are_core_aliases(self):
        self.assertEqual(command(self.registry, "vkMapMemory2KHR").get("alias"), "vkMapMemory2")
        self.assertEqual(command(self.registry, "vkUnmapMemory2KHR").get("alias"), "vkUnmapMemory2")
        for alias, canonical in (
            ("VkMemoryMapInfoKHR", "VkMemoryMapInfo"),
            ("VkMemoryUnmapInfoKHR", "VkMemoryUnmapInfo"),
        ):
            with self.subTest(alias=alias):
                entry = self.registry.find(f"./types/type[@name='{alias}']")
                self.assertIsNotNone(entry)
                self.assertEqual(entry.get("alias"), canonical)

    def test_map_and_unmap_forward_all_core_arguments_and_reject_unsupported_chains(self):
        map_body = function_body(self.device, "dzn_MapMemory2KHR")
        self.assertIn("pMemoryMapInfo->flags", map_body)
        self.assertIn("pMemoryMapInfo->pNext", map_body)
        self.assertIn("pMemoryMapInfo->memory", map_body)
        self.assertIn("pMemoryMapInfo->offset", map_body)
        self.assertIn("pMemoryMapInfo->size", map_body)
        self.assertIn("return dzn_MapMemory(", map_body)
        unmap_body = function_body(self.device, "dzn_UnmapMemory2KHR")
        self.assertIn("pMemoryUnmapInfo->flags", unmap_body)
        self.assertIn("pMemoryUnmapInfo->pNext", unmap_body)
        self.assertIn("dzn_UnmapMemory(device, pMemoryUnmapInfo->memory)", unmap_body)
        self.assertIn("return VK_SUCCESS", unmap_body)


class KhrVertexAttributeDivisorAliasTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.common_graphics = VK_GRAPHICS_STATE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_khr_alias_uses_existing_ext_feature_and_property_gates(self):
        self.assertRegex(self.device, r"\.EXT_vertex_attribute_divisor\s*=\s*true\b")
        self.assertRegex(self.device, r"\.KHR_vertex_attribute_divisor\s*=\s*true\b")
        self.assertRegex(self.device, r"\.vertexAttributeInstanceRateDivisor\s*=\s*true\b")
        self.assertRegex(
            self.device,
            r"\.vertexAttributeInstanceRateZeroDivisor\s*=\s*true\b",
        )
        self.assertRegex(self.device, r"\.maxVertexAttribDivisor\s*=\s*UINT32_MAX\b")

    def test_khr_structs_are_abi_aliases_and_require_no_new_commands(self):
        ext = extension(self.registry, "VK_KHR_vertex_attribute_divisor")
        self.assertEqual(ext.get("promotedto"), "VK_VERSION_1_4")
        self.assertEqual(ext.findall("./require/command"), [])
        aliases = {
            "VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR":
                "VkPhysicalDeviceVertexAttributeDivisorFeatures",
            "VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR":
                "VkPhysicalDeviceVertexAttributeDivisorProperties",
            "VkVertexInputBindingDivisorDescriptionKHR":
                "VkVertexInputBindingDivisorDescription",
            "VkPipelineVertexInputDivisorStateCreateInfoKHR":
                "VkPipelineVertexInputDivisorStateCreateInfo",
        }
        for name, canonical in aliases.items():
            with self.subTest(name=name):
                entry = self.registry.find(f"./types/type[@name='{name}']")
                self.assertIsNotNone(entry)
                self.assertEqual(entry.get("alias"), canonical)

    def test_common_and_dzn_pipeline_preserve_per_instance_divisor_including_zero(self):
        self.assertIn("VkPipelineVertexInputDivisorStateCreateInfoKHR", self.common_graphics)
        self.assertIn("PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR", self.common_graphics)
        body = function_body(self.pipeline, "dzn_graphics_pipeline_translate_vi")
        self.assertIn("PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT", body)
        self.assertRegex(
            body,
            r"\.InstanceDataStepRate\s*=\s*\n\s*divisor\s*\?\s*divisor->divisor",
        )
        self.assertIn("D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA", body)


if __name__ == "__main__":
    unittest.main()

