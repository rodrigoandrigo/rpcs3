"""Source regressions for the seven local extensions; not Vulkan CTS."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent
WSI = DZN.parents[1] / "vulkan" / "wsi"


class SevenExtensionsSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")
        cls.pipeline = (DZN / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.private = (DZN / "dzn_private.h").read_text(encoding="utf-8")
        cls.wsi = (WSI / "wsi_common_win32.cpp").read_text(encoding="utf-8")

    def test_all_seven_extension_fields_have_implementation(self):
        for field in ("EXT_4444_formats", "KHR_present_id", "KHR_swapchain_mutable_format",
                      "EXT_conservative_rasterization", "EXT_extended_dynamic_state",
                      "EXT_extended_dynamic_state2", "EXT_vertex_input_dynamic_state"):
            self.assertRegex(self.device, r"\." + field + r"\s*=")

    def test_4444_has_distinct_exact_native_mappings(self):
        util = (DZN / "dzn_util.c").read_text(encoding="utf-8")
        self.assertIn("[PIPE_FORMAT_B4G4R4A4_UNORM] = DXGI_FORMAT_B4G4R4A4_UNORM", util)
        self.assertIn("[PIPE_FORMAT_R4G4B4A4_UNORM] = DXGI_FORMAT_A4B4G4R4_UNORM", util)
        self.assertIn(".formatA4B4G4R4 = pdev->support_a4b4g4r4", self.device)
        self.assertIn("a4b4g4r4_support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D", self.device)

    def test_conservative_state_has_hardware_gate_and_pipeline_consumer(self):
        self.assertIn("D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED", self.device)
        self.assertIn(".primitiveUnderestimation = false", self.device)
        self.assertIn("PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT", self.pipeline)
        self.assertEqual(self.pipeline.count("desc->ConservativeRaster = conservative_mode"), 2)

    def test_extended_states_are_gated_and_optional_features_stay_off(self):
        self.assertIn(".extendedDynamicState = pdev->options14.IndependentFrontAndBackStencilRefMaskSupported", self.device)
        self.assertIn(".extendedDynamicState2LogicOp = false", self.device)
        self.assertIn(".extendedDynamicState2PatchControlPoints = false", self.device)
        for state in ("CULL_MODE", "FRONT_FACE", "PRIMITIVE_TOPOLOGY", "DEPTH_TEST_ENABLE",
                      "DEPTH_WRITE_ENABLE", "DEPTH_COMPARE_OP", "DEPTH_BOUNDS_TEST_ENABLE",
                      "STENCIL_TEST_ENABLE", "STENCIL_OP", "RASTERIZER_DISCARD_ENABLE",
                      "DEPTH_BIAS_ENABLE", "PRIMITIVE_RESTART_ENABLE", "VERTEX_INPUT_EXT",
                      "VERTEX_INPUT_BINDING_STRIDE", "VIEWPORT_WITH_COUNT", "SCISSOR_WITH_COUNT"):
            self.assertIn("case VK_DYNAMIC_STATE_" + state + ":", self.pipeline)

    def test_late_vertex_variants_own_nir_options_and_serialize_cache(self):
        self.assertIn("pipeline->dynamic_vs_options = *pipeline->dynamic_vs->options", self.pipeline)
        self.assertIn("pipeline->dynamic_vs->options = &pipeline->dynamic_vs_options", self.pipeline)
        self.assertIn("NIR_PASS(_, nir, dxil_nir_lower_vs_vertex_conversion, conversions)", self.pipeline)
        self.assertIn("mtx_lock(&pipeline->variants_lock)", self.pipeline)
        self.assertIn("mtx_unlock(&pipeline->variants_lock)", self.pipeline)
        self.assertNotIn("pipeline->base.state = variant->state", self.pipeline)
        self.assertIn("cmdbuf->state.pipeline_state = new_pipeline_state", self.cmd)

    def test_common_vertex_input_storage_is_initialized_and_reset(self):
        self.assertIn("struct vk_vertex_input_state dynamic_vi", self.private)
        self.assertEqual(self.cmd.count("cmdbuf->vk.dynamic_graphics_state.vi = &cmdbuf->dynamic_vi"), 2)
        self.assertIn("vk_common_CmdSetVertexInputEXT", self.cmd)
        self.assertIn("DZN_DYNAMIC_VERTEX_INPUT | DZN_DYNAMIC_VERTEX_STRIDE", self.cmd)

    def test_mutable_swapchain_is_not_bound_to_a_typed_dxgi_image(self):
        self.assertIn("params->storage_image || params->mutable_format", self.wsi)
        self.assertIn("VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR", self.wsi)
        self.assertIn("create.flags &= ~(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT", self.wsi)

    def test_present_ids_track_accepted_presents_not_completion(self):
        self.assertIn("result == VK_SUCCESS && present_id", self.wsi)
        self.assertIn("chain->last_submitted_present_id = present_id", self.wsi)
        self.assertRegex(self.device, r"\.KHR_present_wait\s*=\s*true")
        self.assertIn("chain->completed_present_id >= present_id", self.wsi)

    def test_failed_variant_creation_returns_recording_error(self):
        self.assertIn("dynamic graphics PSO failed HRESULT", self.pipeline)
        self.assertIn("vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_UNKNOWN)", self.cmd)

    def test_implicit_point_shader_consumes_dynamic_raster_state(self):
        nir = (DZN / "dzn_nir.c").read_text(encoding="utf-8")
        for field in ("point_cull_mode", "point_front_ccw", "point_depth_bias_enable"):
            self.assertIn(field, nir)
            self.assertIn("cmdbuf->state.sysvals.gfx." + field, self.cmd)
        self.assertIn("info->cull_dynamic || info->front_face_dynamic", nir)
        self.assertIn("if (info->depth_bias_enable_dynamic)", nir)
        self.assertIn("var->data.how_declared = nir_var_hidden", nir)

    def test_wsi_blit_uses_matching_format_and_dedicated_image(self):
        self.assertRegex(self.wsi, r"memory_dedicated_info\s*=\s*\{\s*"
                         r"VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,\s*nullptr,\s*image->image,")
        self.assertIn("DXGI_FORMAT_R8G8B8A8_UNORM", self.wsi)
        self.assertIn("VK_FORMAT_R8G8B8A8_SRGB", self.wsi)

    def test_reused_images_keep_legal_enhanced_barrier_access(self):
        self.assertIn("layout_only_destination", self.cmd)
        self.assertIn("D3D12_TEXTURE_BARRIER_FLAG_DISCARD", self.cmd)
        self.assertIn("VK_PIPELINE_STAGE_2_HOST_BIT", self.cmd)


if __name__ == "__main__":
    unittest.main()
