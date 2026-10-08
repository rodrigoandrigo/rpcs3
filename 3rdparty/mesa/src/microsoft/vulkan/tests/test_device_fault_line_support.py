"""Dozen guards for EXT_device_fault and native EXT_line_rasterization."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class DeviceFaultLineSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")
        cls.pipeline = (DZN / "dzn_pipeline.c").read_text(encoding="utf-8")

    def test_extensions_and_features_are_reported(self):
        for field in ("EXT_device_fault", "EXT_line_rasterization"):
            self.assertRegex(self.device, rf"\.{field}\s*=\s*true")
        for feature in ("deviceFaultEXT", "rectangularLines", "bresenhamLines", "smoothLines"):
            self.assertRegex(self.device, rf"\.{feature}\s*=")
        for unsupported in ("deviceFaultVendorBinaryEXT", "stippledRectangularLines",
                            "stippledBresenhamLines", "stippledSmoothLines"):
            self.assertRegex(self.device, rf"\.{unsupported}\s*=\s*false")

    def test_fault_query_is_stable_and_does_not_claim_vendor_payloads(self):
        self.assertIn("dzn_GetDeviceFaultInfoEXT", self.device)
        self.assertIn("ID3D12Device_GetDeviceRemovedReason", self.device)
        self.assertIn("pFaultCounts->addressInfoCount = 0", self.device)
        self.assertIn("pFaultCounts->vendorInfoCount = 0", self.device)
        self.assertIn("pFaultCounts->vendorBinarySize = 0", self.device)

    def test_all_non_stippled_line_modes_map_to_rasterizer2(self):
        self.assertIn("PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT", self.pipeline)
        mappings = {
            "RECTANGULAR_EXT": "QUADRILATERAL_NARROW",
            "BRESENHAM_EXT": "ALIASED",
            "RECTANGULAR_SMOOTH_EXT": "ALPHA_ANTIALIASED",
        }
        for vk_mode, d3d_mode in mappings.items():
            self.assertIn(f"VK_LINE_RASTERIZATION_MODE_{vk_mode}", self.pipeline)
            self.assertIn(f"D3D12_LINE_RASTERIZATION_MODE_{d3d_mode}", self.pipeline)


if __name__ == "__main__":
    unittest.main()
