"""LAST provoking vertex compiler/state regression guards, not Vulkan CTS."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class ProvokingVertexSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pipeline = (DZN / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.nir = (DZN / "dzn_nir.c").read_text(encoding="utf-8")
        cls.cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.meta = (DZN / "dzn_meta.c").read_text(encoding="utf-8")
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")

    def test_feature_and_pipeline_accept_last(self):
        self.assertRegex(self.device, r"\.provokingVertexLast\s*=\s*true")
        self.assertIn("provoking->provokingVertexMode != VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT", self.pipeline)
        self.assertRegex(self.device, r"\.transformFeedbackPreservesProvokingVertex\s*=\s*false")

    def test_cache_keys_separate_modes_and_emulation(self):
        self.assertGreaterEqual(self.pipeline.count("&provoking_compile_key"), 2)
        self.assertIn("device->vk.enabled_features.provokingVertexLast << 25", self.pipeline)

    def test_geometry_retiming_preserves_output_limit_and_flushes_tails(self):
        function = self.nir.split("dzn_nir_lower_last_provoking_vertex(nir_shader *nir)", 1)[1].split("dzn_nir_polygon_point_mode_gs", 1)[0]
        self.assertIn("MESA_PRIM_LINE_STRIP ? 1 : 2", function)
        self.assertIn("nir_after_impl(impl)", function)
        self.assertIn("for (unsigned tail = delay; tail > 0; tail--)", function)
        self.assertNotIn("vertices_out =", function)
        self.assertIn("nir_lower_io_vars_to_temporaries, impl, nir_var_shader_out", function)
        self.assertIn("nir_lower_global_vars_to_local", function)

    def test_fans_use_consistent_emulation_and_strips_handle_parity(self):
        self.assertIn("!device->vk.enabled_features.provokingVertexLast", self.meta)
        self.assertIn("provoking_strip", self.nir)
        self.assertIn("nir_load_primitive_id", self.nir)
        self.assertIn("topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ? 1", self.cmd)
        self.assertIn("cmdbuf->vk.base.device->enabled_features.provokingVertexLast", self.cmd)

    def test_restart_rewrites_strip_and_uses_full_gpu_address(self):
        self.assertIn("DZN_INDEX_2B_STRIP_RESTART", self.cmd)
        self.assertIn("DZN_INDEX_4B_STRIP_RESTART", self.meta)
        self.assertIn("draw_type.triangle_fan_primitive_restart ?\n      sizeof(struct dzn_indirect_triangle_fan_prim_restart_rewrite_index_exec_params)", self.cmd)
        self.assertIn("nir_iadd(&b, nir_channel(&b, exec_buf_start, 1)", self.nir)
        self.assertIn("nir_iadd(&b, nir_channel(&b, draw_info1, 2), nir_channel(&b, draw_info1, 0))", self.nir)
        self.assertIn("D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST", self.cmd)


if __name__ == "__main__":
    unittest.main()
