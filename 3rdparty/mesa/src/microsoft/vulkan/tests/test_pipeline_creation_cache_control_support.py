#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify conservative VK_EXT_pipeline_creation_cache_control support."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
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


class PipelineCreationCacheControlSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_are_reported(self):
        self.assertRegex(
            self.device,
            r"\.EXT_pipeline_creation_cache_control\s*=\s*true\b",
        )
        self.assertRegex(
            self.device,
            r"\.pipelineCreationCacheControl\s*=\s*true\b",
        )

    def test_registry_feature_and_promotion_are_consistent(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_pipeline_creation_cache_control']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_3")
        self.assertIn("VK_VERSION_1_1", extension.get("depends", ""))
        self.assertIn(
            (
                "pipelineCreationCacheControl",
                "VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT",
            ),
            {
                (node.get("name"), node.get("struct"))
                for node in extension.findall("./require/feature")
            },
        )
        self.assertIsNotNone(
            self.registry.find(
                ".//enum[@name='VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT']"
            )
        )
        self.assertIsNotNone(
            self.registry.find(
                ".//enum[@name='VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT']"
            )
        )

    def assert_wrapper_refuses_compilation(self, function, flags_getter, create):
        body = function_body(self.pipeline, function)
        self.assertIn(
            f"{flags_getter}(&pCreateInfos[i])",
            body,
        )
        self.assertIn("VkResult pipeline_result = VK_PIPELINE_COMPILE_REQUIRED;", body)
        self.assertIn(
            "VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR",
            body,
        )
        self.assertLess(
            body.index("VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR"),
            body.index(create),
        )
        self.assertIn("result = pipeline_result;", body)
        self.assertIn("pPipelines[i] = VK_NULL_HANDLE;", body)
        self.assertIn(
            "VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR",
            body,
        )

    def test_graphics_pipeline_flag_returns_compile_required_before_translation(self):
        self.assert_wrapper_refuses_compilation(
            "dzn_CreateGraphicsPipelines",
            "vk_graphics_pipeline_create_flags",
            "dzn_graphics_pipeline_create(",
        )

    def test_compute_pipeline_flag_returns_compile_required_before_translation(self):
        self.assert_wrapper_refuses_compilation(
            "dzn_CreateComputePipelines",
            "vk_compute_pipeline_create_flags",
            "dzn_compute_pipeline_create(",
        )

    def test_flags2_are_checked_and_compile_required_survives_later_batch_success(self):
        for function, flags_getter in (
            ("dzn_CreateGraphicsPipelines", "vk_graphics_pipeline_create_flags"),
            ("dzn_CreateComputePipelines", "vk_compute_pipeline_create_flags"),
        ):
            body = function_body(self.pipeline, function)
            self.assertIn(f"{flags_getter}(&pCreateInfos[i])", body)
            self.assertIn("if (pipeline_result != VK_SUCCESS)", body)
            self.assertIn("if (result != VK_SUCCESS) {", body)


if __name__ == "__main__":
    unittest.main()

