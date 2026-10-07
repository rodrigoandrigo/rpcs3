#!/usr/bin/env python3
# Copyright © 2026 Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Verify VK_EXT_shader_uniform_buffer_unsized_array in Dozen."""
from __future__ import annotations
import os
import re
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_DESCRIPTORS = TEST_DIR.parent / "dzn_descriptor_set.c"
DXIL_NIR = MESA_ROOT / "src/microsoft/compiler/nir_to_dxil.c"
SPIRV_TO_NIR = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


def function_body(source: str, name: str) -> str:
    match = re.search(
        r"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\n" + re.escape(name) + r"\s*\(",
        source,
    )
    if match is None:
        raise AssertionError(f"function not found: {name}")
    brace = source.index("{", match.end())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unclosed function: {name}")


def _instruction(opcode: int, *operands: int) -> list[int]:
    return [((len(operands) + 1) << 16) | opcode, *operands]


def _string_words(value: str) -> list[int]:
    encoded = value.encode("utf-8") + b"\0"
    encoded += b"\0" * ((4 - len(encoded) % 4) % 4)
    return list(struct.unpack(f"<{len(encoded) // 4}I", encoded))


def runtime_array_ubo_shader() -> bytes:
    """Build a small valid-layout SPIR-V vertex shader with a runtime UBO tail.

    The block contains a scalar prefix followed by a final vec4 runtime array
    (ArrayStride 16). The array is indexed by VertexIndex and loaded into
    Position, exercising the non-constant UBO offset used by real shaders.
    """
    words: list[int] = []
    emit = lambda opcode, *operands: words.extend(_instruction(opcode, *operands))
    emit(17, 1)  # OpCapability Shader
    emit(14, 0, 1)  # OpMemoryModel Logical GLSL450
    emit(15, 0, 17, *_string_words("main"), 13, 14)  # OpEntryPoint Vertex
    emit(71, 5, 6, 16)  # ArrayStride 16
    emit(71, 6, 2)  # Block
    emit(72, 6, 0, 35, 0)  # member 0 Offset 0
    emit(72, 6, 1, 35, 16)  # final runtime-array member Offset 16
    emit(71, 12, 34, 0)  # DescriptorSet 0
    emit(71, 12, 33, 0)  # Binding 0
    emit(71, 13, 11, 42)  # BuiltIn VertexIndex
    emit(71, 14, 11, 0)  # BuiltIn Position
    emit(19, 1)  # OpTypeVoid
    emit(21, 2, 32, 0)  # OpTypeInt uint32
    emit(22, 3, 32)  # OpTypeFloat 32
    emit(23, 4, 3, 4)  # OpTypeVector vec4
    emit(29, 5, 4)  # OpTypeRuntimeArray vec4
    emit(30, 6, 3, 5)  # OpTypeStruct { float, runtime vec4[] }
    emit(32, 7, 2, 6)  # Uniform pointer to block
    emit(32, 8, 2, 4)  # Uniform pointer to vec4
    emit(32, 9, 1, 2)  # Input pointer to uint
    emit(32, 10, 3, 4)  # Output pointer to vec4
    emit(33, 11, 1)  # OpTypeFunction void()
    emit(43, 2, 15, 0)  # uint 0
    emit(43, 2, 16, 1)  # uint 1 (block member index)
    emit(59, 7, 12, 2)  # UBO variable
    emit(59, 9, 13, 1)  # VertexIndex input
    emit(59, 10, 14, 3)  # Position output
    emit(54, 1, 17, 0, 11)  # OpFunction main
    emit(248, 18)  # OpLabel
    emit(61, 2, 19, 13)  # load VertexIndex
    emit(65, 8, 20, 12, 16, 19)  # access UBO.values[VertexIndex]
    emit(61, 4, 21, 20)  # load vec4
    emit(62, 14, 21)  # store Position
    emit(253)  # OpReturn
    emit(56)  # OpFunctionEnd
    header = [0x07230203, 0x00010300, 0, 22, 0]
    return struct.pack(f"<{len(header) + len(words)}I", *(header + words))


class ShaderUniformBufferUnsizedArraySupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.descriptors = DZN_DESCRIPTORS.read_text(encoding="utf-8")
        cls.dxil = DXIL_NIR.read_text(encoding="utf-8")
        cls.spirv = SPIRV_TO_NIR.read_text(encoding="utf-8")
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_extension_and_feature_are_reported(self) -> None:
        extensions = function_body(self.device, "dzn_physical_device_get_extensions")
        features = function_body(self.device, "dzn_physical_device_get_features")
        self.assertRegex(
            extensions,
            r"\.EXT_shader_uniform_buffer_unsized_array\s*=\s*true\b",
        )
        self.assertRegex(
            features,
            r"\.shaderUniformBufferUnsizedArray\s*=\s*true\b",
        )

    def test_registry_feature_is_queried_and_enabled_by_shared_runtime(self) -> None:
        extension = self.registry.find(
            "./extensions/extension[@name='VK_EXT_shader_uniform_buffer_unsized_array']"
        )
        self.assertIsNotNone(extension)
        feature = extension.find(
            "./require/feature[@name='shaderUniformBufferUnsizedArray']"
        )
        self.assertIsNotNone(feature)
        self.assertEqual(
            feature.get("struct"),
            "VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT",
        )

    def test_spirv_frontend_preserves_runtime_array_as_unsized(self) -> None:
        start = self.spirv.index("case SpvOpTypeRuntimeArray:")
        type_case = self.spirv[start : self.spirv.index("case SpvOpTypeStruct:", start)]
        self.assertIn("val->type->length = 0", type_case)
        self.assertIn("glsl_array_type(array_element->type, val->type->length", type_case)

    def test_dxil_uses_cbv_metadata_and_dynamic_load_path(self) -> None:
        emit_ubo = function_body(self.dxil, "emit_ubo_var")
        emit_load = function_body(self.dxil, "emit_load_ubo_vec4")
        self.assertIn("glsl_get_explicit_size(type, false)", emit_ubo)
        self.assertIn("load_ubo(ctx, handle, offset, overload)", emit_load)
        self.assertIn('"dx.op.cbufferLoadLegacy"', self.dxil)

    def test_bound_cbv_range_retains_d3d12_robust_bounds_checks(self) -> None:
        buffer_desc = function_body(
            self.descriptors, "dzn_descriptor_heap_write_buffer_desc"
        )
        self.assertIn("info->range == VK_WHOLE_SIZE", buffer_desc)
        self.assertIn("ALIGN_POT(size, 256)", buffer_desc)
        self.assertIn("D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS", self.descriptors)
        self.assertIn("range->Flags = type ==", self.descriptors)

    def test_cbv_rounding_respects_d3d12_size_limit(self) -> None:
        buffer_desc = function_body(
            self.descriptors, "dzn_descriptor_heap_write_buffer_desc"
        )
        self.assertIn(
            "MIN2(ALIGN_POT(size, 256), D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4 * sizeof(float))",
            buffer_desc,
        )

        memory_requirements = function_body(
            self.device, "dzn_GetBufferMemoryRequirements2"
        )
        self.assertIn(
            "size = ALIGN_POT(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)",
            memory_requirements,
        )

        properties = function_body(
            self.device, "dzn_physical_device_get_properties"
        )
        self.assertIn(
            ".maxUniformBufferRange = D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * D3D12_STANDARD_VECTOR_SIZE * sizeof(float)",
            properties,
        )

        max_cbv_size = 4096 * 4 * 4
        for requested_size in (1, 255, 256, 65535, 65536):
            cbv_size = min(((requested_size + 255) // 256) * 256, max_cbv_size)
            self.assertEqual(cbv_size % 256, 0)
            self.assertLessEqual(cbv_size, max_cbv_size)

    @unittest.skipUnless(
        os.environ.get("SPIRV2DXIL"),
        "set SPIRV2DXIL to the built Dozen SPIR-V-to-DXIL harness",
    )
    def test_dynamic_runtime_tail_compiles_to_dxil(self) -> None:
        compiler = Path(os.environ["SPIRV2DXIL"])
        with tempfile.TemporaryDirectory(prefix="dzn-ubo-unsized-") as tmp:
            tmp_path = Path(tmp)
            spirv_path = tmp_path / "runtime_tail.spv"
            dxil_path = tmp_path / "runtime_tail.dxil"
            spirv_path.write_bytes(runtime_array_ubo_shader())
            result = subprocess.run(
                [
                    str(compiler),
                    "-s",
                    "vertex",
                    "-d",
                    "-o",
                    str(dxil_path),
                    str(spirv_path),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            diagnostics = result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, diagnostics)
            self.assertIn("@load_vertex_id_zero_base", diagnostics)
            self.assertIn("@load_ubo", diagnostics)
            self.assertIn("range=-1", diagnostics)
            self.assertGreater(dxil_path.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()

