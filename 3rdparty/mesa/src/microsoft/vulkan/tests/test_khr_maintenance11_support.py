#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Guard the Dozen VK_KHR_maintenance11 mappings and their source evidence."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
DZN_DIR = TEST_DIR.parent
MESA_ROOT = TEST_DIR.parents[3]


def function_body(source: str, name: str) -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", source, re.S)
    if not match:
        raise AssertionError(f"could not find function {name}")
    start = source.find("{", match.start())
    depth = 0
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:pos]
    raise AssertionError(f"unterminated function {name}")


class Maintenance11SupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device = (DZN_DIR / "dzn_device.c").read_text(encoding="utf-8")
        cls.image = (DZN_DIR / "dzn_image.c").read_text(encoding="utf-8")
        cls.pipeline = (DZN_DIR / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.command_buffer = (DZN_DIR / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.vk_image = (MESA_ROOT / "src/vulkan/runtime/vk_image.c").read_text(encoding="utf-8")
        cls.registry = (MESA_ROOT / "src/vulkan/registry/vk.xml").read_text(encoding="utf-8")

    def test_extension_and_feature_are_reported(self) -> None:
        self.assertRegex(self.device, r"\.KHR_maintenance11\s*=\s*true")
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertRegex(features, r"\.maintenance11\s*=\s*true")

    def test_all_exposed_queues_guarantee_unit_transfer_granularity(self) -> None:
        start = self.device.index("pdev->queue_families[pdev->queue_family_count++]")
        end = self.device.index("assert(pdev->queue_family_count", start)
        families = self.device[start:end]
        self.assertEqual(families.count(".minImageTransferGranularity = { 1, 1, 1 }"), 2)
        self.assertNotRegex(families, r"\.queueFlags\s*=\s*VK_QUEUE_TRANSFER_BIT\s*,")
        self.assertIn("D3D12_COMMAND_LIST_TYPE_DIRECT", families)
        # Public compute-only queues use direct native lists for raster-based
        # transfer emulation; their Vulkan queue flags stay compute/transfer.
        self.assertIn(".queueFlags = VK_QUEUE_COMPUTE_BIT |", families)
        self.assertEqual(families.count(".Type = D3D12_COMMAND_LIST_TYPE_DIRECT"), 2)

    def test_optimal_granularity_property_is_filled_conservatively(self) -> None:
        query = function_body(self.device, "dzn_GetPhysicalDeviceQueueFamilyProperties2")
        self.assertIn(
            "VK_STRUCTURE_TYPE_QUEUE_FAMILY_OPTIMAL_IMAGE_TRANSFER_GRANULARITY_PROPERTIES_KHR",
            query,
        )
        self.assertIn(
            "optimalImageTransferGranularity = (VkExtent3D) { 0, 0, 0 }",
            query,
        )

    def test_single_layer_srv_and_uav_paths_use_d3d12_compatible_shapes(self) -> None:
        srv = function_body(self.image, "dzn_image_view_prepare_srv_desc")
        uav = function_body(self.image, "dzn_image_view_prepare_uav_desc")
        self.assertIn("iview->vk.base_array_layer > 0", srv)
        self.assertIn("iview->vk.layer_count / layers_per_elem", srv)
        self.assertIn("D3D12_SRV_DIMENSION_TEXTURE2DARRAY", srv)
        self.assertIn("D3D12_SRV_DIMENSION_TEXTURE2D", srv)
        self.assertIn("iview->vk.base_array_layer > 0", uav)
        self.assertIn("iview->vk.layer_count > 1", uav)
        self.assertIn("D3D12_UAV_DIMENSION_TEXTURE2DARRAY", uav)
        self.assertIn("D3D12_UAV_DIMENSION_TEXTURE2D", uav)
        self.assertIn("VK_IMAGE_CREATE_ALIAS_SINGLE_LAYER_DESCRIPTOR_BIT_KHR", self.registry)
        image_init = function_body(self.vk_image, "vk_image_init")
        self.assertIn("image->create_flags = create_flags", image_init)

    def test_depth_clip_default_matches_maintenance11_clarification(self) -> None:
        pipeline = function_body(self.pipeline, "dzn_graphics_pipeline_translate_rast")
        self.assertRegex(
            pipeline,
            r"depth_clip_info\s*\?\s*depth_clip_info->depthClipEnable\s*:\s*!in_rast->depthClampEnable",
        )
        self.assertIn("desc->DepthClipEnable = depth_clip_enable", pipeline)

    def test_one_queue_family_concurrent_sharing_needs_no_special_backend_path(self) -> None:
        image_create = function_body(self.image, "dzn_image_create")
        buffer_create = function_body(self.device, "dzn_buffer_create")
        self.assertIn("vk_image_init(&device->vk, &image->vk, pCreateInfo)", image_create)
        self.assertNotIn("queueFamilyIndexCount", image_create)
        self.assertNotIn("pQueueFamilyIndices", image_create)
        self.assertNotIn("queueFamilyIndexCount", buffer_create)
        self.assertNotIn("pQueueFamilyIndices", buffer_create)
        image_init = function_body(self.vk_image, "vk_image_init")
        self.assertIn("image->sharing_mode = pCreateInfo->sharingMode", image_init)

    def test_partial_image_copies_are_implemented_on_d3d12_lists(self) -> None:
        to_image = function_body(self.command_buffer, "dzn_cmd_buffer_copy_buf2img_region")
        from_image = function_body(self.command_buffer, "dzn_cmd_buffer_copy_img2buf_region")
        self.assertIn("region.imageOffset.x", to_image)
        self.assertIn("region.imageOffset.y", to_image)
        self.assertIn("region.imageExtent.width", to_image)
        self.assertIn("ID3D12GraphicsCommandList1_CopyTextureRegion", to_image)
        self.assertIn("region.imageOffset.x", from_image)
        self.assertIn("region.imageExtent.width", from_image)
        self.assertIn("ID3D12GraphicsCommandList1_CopyTextureRegion", from_image)


if __name__ == "__main__":
    unittest.main()

