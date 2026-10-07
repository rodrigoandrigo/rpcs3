#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify conservative VK_EXT_pipeline_creation_feedback support in Dozen."""
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


class PipelineCreationFeedbackSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_is_advertised_for_the_vulkan_12_device(self):
        self.assertRegex(
            self.device,
            r"\.EXT_pipeline_creation_feedback\s*=\s*true\b",
        )
        self.assertRegex(self.device, r"#define DZN_API_VERSION VK_MAKE_VERSION\(1, 2,")

    def test_registry_promotion_and_no_extra_commands_or_features(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_pipeline_creation_feedback']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("promotedto"), "VK_VERSION_1_3")
        self.assertEqual(extension.findall("./require/command"), [])
        self.assertEqual(extension.findall("./require/feature"), [])
        self.assertIn(
            "VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT",
            {
                item.get("name")
                for item in extension.findall("./require/enum")
            },
        )

    def test_outputs_are_cleared_before_creation_including_optional_stage_array(self):
        body = function_body(self.pipeline, "dzn_pipeline_creation_feedback_init")
        self.assertIn("*feedback_info->pPipelineCreationFeedback =", body)
        self.assertIn("pipelineStageCreationFeedbackCount", body)
        self.assertIn("pPipelineStageCreationFeedbacks[i]", body)
        self.assertIn("(VkPipelineCreationFeedback) { 0 }", body)

    def test_pipeline_feedback_is_valid_and_measures_the_complete_PSO_call(self):
        body = function_body(self.pipeline, "dzn_pipeline_creation_feedback_finish")
        self.assertIn("VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT", body)
        self.assertIn("os_time_get_nano() - pipeline_start", body)
        self.assertNotIn(
            "VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT",
            body,
        )
        self.assertIn("D3D12 PSOs", body)

    def test_graphics_and_compute_wrappers_write_feedback_only_on_success(self):
        for function, create_call in (
            ("dzn_CreateGraphicsPipelines", "dzn_graphics_pipeline_create("),
            ("dzn_CreateComputePipelines", "dzn_compute_pipeline_create("),
        ):
            with self.subTest(function=function):
                body = function_body(self.pipeline, function)
                self.assertIn("PIPELINE_CREATION_FEEDBACK_CREATE_INFO", body)
                self.assertIn("dzn_pipeline_creation_feedback_init(feedback_info)", body)
                self.assertIn("dzn_pipeline_creation_feedback_finish(feedback_info, pipeline_start)", body)
                self.assertLess(
                    body.index("dzn_pipeline_creation_feedback_init(feedback_info)"),
                    body.index(create_call),
                )
                self.assertLess(body.index(create_call), body.index("pipeline_result == VK_SUCCESS"))
                self.assertLess(
                    body.index("pipeline_result == VK_SUCCESS"),
                    body.index("dzn_pipeline_creation_feedback_finish"),
                )

    def test_dxil_cache_hits_do_not_claim_a_reusable_pipeline_hit(self):
        finish = function_body(self.pipeline, "dzn_pipeline_creation_feedback_finish")
        self.assertIn("not reusable D3D12 PSOs", finish)
        self.assertIn("stage feedback is optional", finish)
        self.assertIn("invalid", finish)


if __name__ == "__main__":
    unittest.main()

