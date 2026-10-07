#!/usr/bin/env python3
"""Compile SPIR-V vertex outputs ViewportIndex and Layer through Dozen's DXIL frontend.

This validates SPIR-V -> NIR -> DXIL emission and signature semantics. It does
not validate the DXIL with the Windows validator or execute on D3D12 hardware.
"""
from __future__ import annotations
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

SPV_MAGIC = 0x07230203


def inst(opcode: int, *args: int) -> list[int]:
    return [((len(args) + 1) << 16) | opcode, *args]


def string_words(text: str) -> list[int]:
    raw = text.encode("utf-8") + b"\0"
    raw += b"\0" * ((-len(raw)) % 4)
    return list(struct.unpack("<" + "I" * (len(raw) // 4), raw))


def module(builtin: int, capability: int, extension_capability: bool = False) -> bytes:
    # SPIR-V ids: main=1, void=2, float=3, vec4=4, uint=5, pointers=6..7,
    # function type=8, constants=9..12, outputs=13..14, label=15.
    words = [SPV_MAGIC, 0x00010300, 0, 16, 0]
    words += inst(17, 1)  # OpCapability Shader
    words += inst(17, capability)  # ShaderViewportIndex or ShaderLayer
    if extension_capability:
        words += inst(10, *string_words("SPV_EXT_shader_viewport_index_layer"))
    words += inst(14, 0, 1)  # Logical, GLSL450
    words += inst(15, 0, 1, *string_words("main"), 13, 14)  # Vertex entry point
    words += inst(71, 13, 11, 0)  # BuiltIn Position
    words += inst(71, 14, 11, builtin)  # BuiltIn ViewportIndex or Layer
    words += inst(19, 2)  # OpTypeVoid
    words += inst(22, 3, 32)  # OpTypeFloat 32
    words += inst(23, 4, 3, 4)  # OpTypeVector float4
    words += inst(21, 5, 32, 0)  # OpTypeInt uint32
    words += inst(32, 6, 3, 4)  # Output pointer to vec4
    words += inst(32, 7, 3, 5)  # Output pointer to uint
    words += inst(33, 8, 2)  # OpTypeFunction void()
    words += inst(43, 3, 9, 0x00000000)  # float 0.0
    words += inst(43, 3, 10, 0x3F800000)  # float 1.0
    words += inst(44, 4, 11, 9, 9, 9, 10)  # Position = (0, 0, 0, 1)
    words += inst(43, 5, 12, 1)  # Index = 1, retained as a real output
    words += inst(59, 6, 13, 3)  # Position output
    words += inst(59, 7, 14, 3)  # Viewport/layer output
    words += inst(54, 2, 1, 0, 8)  # OpFunction
    words += inst(248, 15)  # OpLabel
    words += inst(62, 13, 11)  # Store position
    words += inst(62, 14, 12)  # Store index
    words += inst(253)  # OpReturn
    words += inst(56)  # OpFunctionEnd
    return struct.pack("<" + "I" * len(words), *words)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        parser.error(f"spirv2dxil not found: {compiler}")
    if args.output_dir:
        outdir = args.output_dir.resolve()
        outdir.mkdir(parents=True, exist_ok=True)
        temp = None
    else:
        temp = tempfile.TemporaryDirectory(prefix="viewport-layer-dxil-")
        outdir = Path(temp.name)

    failures: list[tuple[str, str]] = []
    for name, builtin, capability, semantic, extension_capability in (
        ("ViewportIndex", 10, 70, b"SV_ViewportArrayIndex", False),
        ("Layer", 9, 69, b"SV_RenderTargetArrayIndex", False),
        ("ViewportIndex_EXT", 10, 5254, b"SV_ViewportArrayIndex", True),
        ("Layer_EXT", 9, 5254, b"SV_RenderTargetArrayIndex", True),
    ):
        source = outdir / f"{name}.spv"
        output = outdir / f"{name}.dxil"
        source.write_bytes(module(builtin, capability, extension_capability))
        output.unlink(missing_ok=True)
        result = subprocess.run(
            [str(compiler), "--stage", "vertex", "--output", str(output), str(source)],
            capture_output=True,
            text=True,
        )
        detail = result.stderr.strip() or result.stdout.strip()
        if result.returncode:
            failures.append((name, detail or f"spirv2dxil exited {result.returncode}"))
        elif not output.is_file() or output.stat().st_size == 0:
            failures.append((name, "compiler returned success without a DXIL output"))
        elif semantic not in output.read_bytes():
            failures.append((name, f"DXIL output omitted signature semantic {semantic.decode()}"))
        else:
            print(f"PASS {name}: emitted {semantic.decode()}")
    for name, detail in failures:
        print(f"FAIL {name}: {detail}")
    print(f"\nResult: {4 - len(failures)}/4 compiled with expected system-value signature")
    if temp:
        temp.cleanup()
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

