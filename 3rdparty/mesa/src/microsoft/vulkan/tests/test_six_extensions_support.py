"""Implementation guards, not conformance tests; two extensions stay pending."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class SixExtensionsSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")
        cls.cmd = (DZN / "dzn_cmd_buffer.c").read_text(encoding="utf-8")
        cls.pipeline = (DZN / "dzn_pipeline.c").read_text(encoding="utf-8")
        cls.nir = (DZN.parent / "spirv_to_dxil/dxil_spirv_nir.c").read_text(encoding="utf-8")
        cls.wsi = (DZN.parents[1] / "vulkan/wsi/wsi_common_win32.cpp").read_text(encoding="utf-8")

    def test_int8_alu_does_not_require_native_storage(self):
        self.assertRegex(self.device, r"\.shaderInt8\s*=\s*true")
        self.assertRegex(self.device, r"\.shaderFloat16\s*=\s*pdev->options4.Native16BitShaderOpsSupported")
        self.assertRegex(self.device, r"\.storageBuffer8BitAccess\s*=\s*support_8bit")

    def test_explicit_shared_layout_capabilities(self):
        for cap in ("WorkgroupMemoryExplicitLayoutKHR", "WorkgroupMemoryExplicitLayout8BitAccessKHR", "WorkgroupMemoryExplicitLayout16BitAccessKHR"):
            self.assertIn("." + cap + " = true", self.nir)
        for feature in ("workgroupMemoryExplicitLayout", "workgroupMemoryExplicitLayoutScalarBlockLayout", "workgroupMemoryExplicitLayout8BitAccess", "workgroupMemoryExplicitLayout16BitAccess"):
            self.assertRegex(self.device, r"\." + feature + r"\s*=\s*true")

    def test_provoking_vertex_first_and_last_are_exposed(self):
        self.assertRegex(self.device, r"\.provokingVertexLast\s*=\s*true")
        self.assertIn("VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT", self.pipeline)
        self.assertIn("VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT", self.pipeline)
        self.assertIn("dzn_nir_lower_last_provoking_vertex", self.pipeline)

    def test_conditional_native_commands_restore_predication(self):
        self.assertIn("enable ? cmdbuf->state.conditional.op : D3D12_PREDICATION_OP_EQUAL_ZERO", self.cmd)
        clear = self.cmd.split("dzn_CmdClearAttachments(", 1)[1].split("static D3D12_RESOLVE_MODE", 1)[0]
        self.assertIn("dzn_cmd_buffer_set_conditional(cmdbuf, true)", clear)
        self.assertIn("dzn_cmd_buffer_set_conditional(cmdbuf, false)", clear)
        self.assertRegex(self.device, r"\.inheritedConditionalRendering\s*=\s*false")

    def test_incomplete_extensions_are_not_advertised(self):
        self.assertNotRegex(self.device, r"\.EXT_transform_feedback\s*=")
        self.assertRegex(self.device, r"\.presentWait\s*=\s*true")
        self.assertRegex(self.device, r"\.transformFeedback\s*=\s*false")

    def test_experimental_present_completion_is_not_queue_completion(self):
        self.assertIn("GetFrameStatistics(&stats)", self.wsi)
        self.assertIn("GetLastPresentCount(&record->sequence)", self.wsi)
        self.assertIn("stats.PresentCount - chain->present_head->sequence", self.wsi)
        self.assertIn("chain->completed_present_id >= present_id", self.wsi)
        self.assertIn("return VK_TIMEOUT", self.wsi)
        self.assertIn("DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT", self.wsi)
        self.assertIn("SetMaximumFrameLatency(1)", self.wsi)
        self.assertIn("GetFrameLatencyWaitableObject()", self.wsi)
        self.assertIn("assert(!chain->present_head)", self.wsi)

    def test_experimental_stream_output_handles_dynamic_discard(self):
        self.assertIn("pipeline->templates.desc_offsets.so", self.pipeline)
        self.assertIn("so->RasterizedStream = extended->discard ? D3D12_SO_NO_RASTERIZED_STREAM : 0", self.pipeline)
        for command in ("dzn_CmdBindTransformFeedbackBuffersEXT", "dzn_CmdBeginTransformFeedbackEXT", "dzn_CmdEndTransformFeedbackEXT"):
            self.assertIn(command, self.cmd)


if __name__ == "__main__":
    unittest.main()
