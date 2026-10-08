"""Backend guards for list restart and the supported EDS3 subset; not CTS."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class TopologyState3Support(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")
        cls.cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.pipeline = (DZN / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.nir = (DZN / "dzn_nir.c").read_text(encoding="utf-8")
        cls.meta = (DZN / "dzn_meta.c").read_text(encoding="utf-8")

    def test_only_implemented_state3_features_are_enabled(self):
        for name in ("DepthClampEnable", "DepthClipEnable", "SampleMask", "AlphaToCoverageEnable",
                     "ColorBlendEnable", "ColorBlendEquation", "ColorWriteMask", "RasterizationSamples"):
            self.assertRegex(self.device, rf"\.extendedDynamicState3{name}\s*=\s*true")
        for name in ("PolygonMode", "ProvokingVertexMode"):
            self.assertNotRegex(self.device, rf"\.extendedDynamicState3{name}\s*=\s*true")

    def test_state3_affects_masked_variant_and_native_state(self):
        for member in ("depth_clamp", "depth_clip", "sample_mask", "alpha_to_coverage"):
            self.assertIn(f", {member});", self.pipeline)
        self.assertIn("rast->DepthClipEnable = extended->depth_clip", self.pipeline)
        self.assertIn("!pipeline->explicit_depth_clip", self.pipeline)
        self.assertIn("*mask = extended->sample_mask", self.pipeline)
        self.assertIn("blend->AlphaToCoverageEnable = extended->alpha_to_coverage", self.pipeline)

    def test_list_restart_discards_incomplete_primitives_and_handles_all_widths(self):
        self.assertIn("(unsigned[]){1, 2, 3, 4, 6}", self.meta)
        self.assertIn("Restart discards an incomplete list primitive", self.nir)
        self.assertIn("nir_store_var(&b, pending, nir_imm_int(&b, 0), 1)", self.nir)
        self.assertIn("dzn_cmd_buffer_list_restart_width", self.cmd)
        self.assertIn("return max_indices", self.cmd)
        self.assertRegex(self.device, r"\.primitiveTopologyPatchListRestart\s*=\s*true")
        self.assertRegex(self.device, r"\.tessellationShader\s*=\s*true")
        self.assertIn("DZN_INDEX_2B_PATCH_RESTART", self.meta)
        self.assertIn("dzn_nir_lower_patch_vertices", self.pipeline)
        self.assertIn("expand_polygon_points", self.pipeline)

    def test_unfinished_contracts_are_not_advertised(self):
        for field in ("KHR_maintenance8", "EXT_depth_range_unrestricted"):
            self.assertNotRegex(self.device, rf"\.{field}\s*=\s*true")


if __name__ == "__main__":
    unittest.main()
