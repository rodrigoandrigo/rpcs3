"""Source regression guards. Native probes and CTS are distinct evidence."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class CompletionSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.nir = (DZN / "dzn_nir.c").read_text(encoding="utf-8")
        cls.pipeline = (DZN / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")
        cls.cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.meta = (DZN / "dzn_meta.c").read_text(encoding="utf-8")

    def test_original_position_is_saved_before_clip_and_viewport_conversion(self):
        preserve = self.pipeline.index("dzn_nir_preserve_xfb_position(*nir)")
        self.assertLess(preserve, self.pipeline.index("NIR_PASS(_, *nir, nir_lower_clip_halfz)"))
        self.assertIn('"dzn_xfb_original_position"', self.nir)
        self.assertIn("nir_intrinsic_write_mask(intr)", self.nir)
        self.assertIn("nir_intrinsic_get_var(intr, 0) != position", self.nir)
        self.assertIn("nir->xfb_info->outputs[i].location = location", self.nir)

    def test_point_geometry_carries_metadata_and_does_not_cull_capture(self):
        self.assertIn("memcpy(nir->xfb_info, previous_shader->xfb_info, size)", self.nir)
        self.assertIn("captures_xfb ? nir_imm_true(b) : cull_pass", self.nir)
        self.assertIn("nir_bcsel(b, cull_pass, position, clipped)", self.nir)
        # Native point SO still needs primitive-atomic overflow handling.
        self.assertRegex(self.device, r"\.transformFeedback\s*=\s*false")
        self.assertNotRegex(self.device, r"\.EXT_transform_feedback\s*=")

    def test_uint8_widening_handles_unaligned_words_and_restart(self):
        self.assertIn("old_index_size == 1", self.nir)
        self.assertIn("DZN_INDEX_1B_CONVERT", self.meta)
        self.assertIn("uint8_buffer", self.cmd)
        self.assertGreaterEqual(self.cmd.count("dzn_cmd_buffer_expand_uint8_indices(cmdbuf)"), 2)
        self.assertIn("source_address & ~3ull", self.cmd)
        self.assertIn("buf->usage = vk_buffer_usage_flags(pCreateInfo)", self.device)

    def test_maintenance5_formats_do_not_claim_native_render_target_swizzle(self):
        self.assertIn("format == VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR", self.device)
        mask = self.device.split("if (format == VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR)", 1)[1].split("/* Native A8", 1)[0]
        self.assertNotIn("VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT", mask)
        self.assertIn("base_props->bufferFeatures = 0", mask)


if __name__ == "__main__":
    unittest.main()
