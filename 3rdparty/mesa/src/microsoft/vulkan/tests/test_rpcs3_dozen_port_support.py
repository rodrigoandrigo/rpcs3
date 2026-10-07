# SPDX-License-Identifier: MIT
"""Source regression guards for bugs found by the isolated Windows port probe.

These are source contracts, not substitutes for executing the GPU probe or CTS.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parents[3]


class RpcS3DozenPortSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dxil = (ROOT / "src/microsoft/compiler/nir_to_dxil.c").read_text(encoding="utf-8")
        cls.bindless = (ROOT / "src/microsoft/spirv_to_dxil/dxil_spirv_nir_lower_bindless.c").read_text(encoding="utf-8")
        cls.validator = (ROOT / "src/microsoft/compiler/dxil_validator.cpp").read_text(encoding="utf-8")
        cls.pipeline = (ROOT / "src/microsoft/vulkan/dzn_pipeline.c").read_text(encoding="utf-8")
        cls.device = (ROOT / "src/microsoft/vulkan/dzn_device.c").read_text(encoding="utf-8")

    def test_descriptor_lookup_does_not_dereference_ambiguous_generic_binding(self):
        body = self.dxil.split("emit_load_vulkan_descriptor(", 1)[1].split("emit_load_sample_pos_from_id(", 1)[0]
        self.assertNotIn("nir_get_binding_variable", body)
        self.assertIn("nir_var_mem_ssbo", body)
        self.assertIn("non_writeable &=", body)
        self.assertIn("if (!found)", body)

    def test_bindless_declarations_are_removed_before_table_ssbos_are_added(self):
        body = self.bindless.split("dxil_spirv_nir_lower_bindless(", 1)[1]
        self.assertIn("can_remove_var(var, options)", body)
        self.assertLess(body.index("exec_node_remove(&var->node)"), body.index("add_bindless_data_var(nir, index)"))
        self.assertIn("case nir_intrinsic_image_deref_samples:", self.bindless)

    def test_validator_handles_failed_calls_and_missing_optional_compiler(self):
        body = self.validator.split("dxil_validate_module(", 1)[1].split("dxil_disasm_module(", 1)[0]
        self.assertIn("FAILED(hr) || !result", body)
        self.assertIn("FAILED(status_hr)", body)
        self.assertNotIn("str[blob_utf8->GetBufferSize() - 1] = 0", body)
        self.assertIn("blob->GetEncoding", body)
        self.assertIn("ralloc_strndup", body)
        self.assertIn("DXIL validation failed (HRESULT", body)

    def test_uwp_pipeline_errors_keep_hresult_and_callback_diagnostics(self):
        self.assertIn("Dozen compute PSO creation failed (HRESULT", self.pipeline)
        self.assertIn('_debug_printf("Dozen DXIL validation failed:', self.pipeline)

    def test_temporary_retail_debug_layer_activation_is_not_present(self):
        start = self.device.index("instance->factory = try_create_device_factory")
        end = self.device.index("#ifndef _XBOX_UWP", start)
        self.assertNotIn("d3d12_enable_debug_layer", self.device[start:end])
        self.assertNotIn("Temporary native-PC diagnostic", self.device)


if __name__ == "__main__":
    unittest.main()
