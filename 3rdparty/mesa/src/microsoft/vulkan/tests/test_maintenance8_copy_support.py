"""Source guards for partial maintenance8 work, not Vulkan conformance."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class Maintenance8CopySupport(unittest.TestCase):
    def test_shader_copy_is_distinct_from_numerical_blit(self):
        nir = (DZN / "dzn_nir.c").read_text(encoding="utf-8")
        cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        for mode in ("D16_TO_COLOR", "COLOR_TO_D16", "D24_TO_COLOR", "COLOR_TO_D24"):
            self.assertIn("DZN_BLIT_COPY_" + mode, nir)
        self.assertIn("dst_range = {\n      .aspectMask = dst_aspect", cmd)
        self.assertIn("dzn_copy_uint_format", cmd)
        self.assertIn("src->vk.samples > 1 ? NULL : &src_box", cmd)

    def test_raw_d32_scratch_is_owned_by_command_buffer(self):
        cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        self.assertIn("dzn_cmd_buffer_alloc_raw_d32_scratch", cmd)
        self.assertIn("list_addtail(&entry->link, &cmdbuf->internal_bufs[DZN_INTERNAL_BUF_DEFAULT])", cmd)
        self.assertIn("raw_d32_src ? dzn_image_to_handle(&src_scratch)", cmd)
        self.assertIn("raw_d32_dst ? dzn_image_to_handle(&scratch)", cmd)

    def test_integer_msaa_clear_and_shader_cache_key(self):
        cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        meta = (DZN / "dzn_meta.c").read_text(encoding="utf-8")
        self.assertIn("dzn_cmd_buffer_clear_msaa_integer", cmd)
        self.assertIn("DZN_BLIT_CLEAR_INTEGER", meta)
        self.assertIn("_mesa_hash_table_insert(meta->fs, (void *)(uintptr_t)info->hash_key, out)", meta)

    def test_signed_modulo_is_not_unsigned_remainder(self):
        compiler = (DZN.parent / "compiler" / "nir_to_dxil.c").read_text(encoding="utf-8")
        body = compiler.split("case nir_op_imod:", 1)[1].split("case nir_op_umod:", 1)[0]
        self.assertIn("DXIL_BINOP_SREM", body)
        self.assertIn("DXIL_ICMP_SLT", body)
        self.assertIn("DXIL_ICMP_NE", body)
        self.assertNotIn("DXIL_BINOP_UREM", body)

    def test_d32_diagnostic_has_independent_controls(self):
        probe = (DZN / "tests" / "dozen_copy_contracts.h").read_text(encoding="utf-8")
        for option in ("INTEGER_CONTROL", "FLOAT_CONTROL", "DEPTH_ATTACHMENT"):
            self.assertIn("DZN_TEST_" + option, probe)
        self.assertIn("float_control && (!special_depth || !full_depth)", probe)
        self.assertIn("0x00000100u", probe)
        self.assertIn("actual!=expected", probe)


if __name__ == "__main__":
    unittest.main()
