"""Backend guards for storage-only EXT_image_2d_view_of_3d."""
from pathlib import Path
import unittest

DZN = Path(__file__).resolve().parent.parent


class Image2DViewOf3DSupport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (DZN / "dzn_device.c").read_text(encoding="utf-8")
        cls.image = (DZN / "dzn_image.c").read_text(encoding="utf-8")

    def test_storage_feature_only_is_advertised(self):
        self.assertRegex(self.device, r"\.EXT_image_2d_view_of_3d\s*=\s*true")
        self.assertRegex(self.device, r"\.image2DViewOf3D\s*=\s*true")
        self.assertRegex(self.device, r"\.sampler2DViewOf3D\s*=\s*false")

    def test_2d_storage_view_selects_3d_uav_slice(self):
        self.assertIn("from_3d_image", self.image)
        self.assertIn("D3D12_UAV_DIMENSION_TEXTURE3D", self.image)
        self.assertIn("Texture3D.FirstWSlice = iview->vk.base_array_layer", self.image)
        self.assertIn("Texture3D.WSize = iview->vk.layer_count", self.image)


if __name__ == "__main__":
    unittest.main()
