"""Regression guards for the partial four-extension follow-up; not CTS."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class FourExtensionsSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pipeline = (DZN / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")
        cls.cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.image = (DZN / "dzn_image.c").read_text(encoding="utf-8")

    def test_robustness_stage_override_and_pipeline_hashes(self):
        self.assertIn("&robustness[stage], info->pNext", self.pipeline)
        self.assertIn("info->pNext, info->stage.pNext", self.pipeline)
        self.assertIn("stages[stage].info, &robustness[stage]", self.pipeline)
        self.assertIn("&info->stage, &robustness, spirv_hash", self.pipeline)
        self.assertIn(".robustness = robustness[stage]", self.pipeline)
        self.assertIn(".robustness = robustness,", self.pipeline)

    def test_per_pipeline_image_lowering_and_bounded_descriptors(self):
        self.assertIn("options->robustness.images == VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_EXT", self.pipeline)
        self.assertIn("!device->vk.enabled_features.pipelineRobustness", self.device)
        for name in ("StorageBuffers", "UniformBuffers", "VertexInputs", "Images"):
            self.assertIn(".defaultRobustness" + name, self.device)

    def test_depth_bias_only_native_format_representation(self):
        self.assertIn("dzn_CmdSetDepthBias2EXT", self.cmd)
        self.assertIn("representation->depthBiasExact", self.cmd)
        self.assertIn("bias_representation->depthBiasExact", self.pipeline)
        self.assertIn("VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT", self.cmd)
        for optional in ("floatRepresentation", "depthBiasExact", "leastRepresentableValueForceUnormRepresentation"):
            self.assertRegex(self.device, r"\." + optional + r"\s*=\s*false")

    def test_maintenance5_helpers_are_not_an_advertisement(self):
        self.assertIn("dzn_CmdBindIndexBuffer2KHR", self.cmd)
        self.assertIn("size == VK_WHOLE_SIZE ? buf->size - offset : size", self.cmd)
        self.assertIn("dzn_GetImageSubresourceLayout2KHR", self.image)
        self.assertIn("dzn_GetDeviceImageSubresourceLayoutKHR", self.image)
        self.assertIn("dzn_image_destroy(dzn_image_from_handle(image), NULL)", self.image)
        self.assertRegex(self.device, r"\.KHR_maintenance5\s*=\s*true")
        for prop in ("depthStencilSwizzleOneSupport", "polygonModePointSize", "earlyFragmentSampleMaskTestBeforeSampleCounting"):
            self.assertIn("."+prop, self.device)

    def test_uint8_is_not_falsely_advertised(self):
        for field in ("KHR_index_type_uint8", "EXT_index_type_uint8"):
            self.assertRegex(self.device, r"\." + field + r"\s*=\s*true")
        self.assertIn("dzn_cmd_buffer_expand_uint8_indices", self.cmd)
        self.assertIn("DZN_INDEX_1B_CONVERT", self.cmd)
        self.assertIn("uint8_buffer", self.cmd)


if __name__ == "__main__":
    unittest.main()
