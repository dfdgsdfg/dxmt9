#!/usr/bin/env python3
"""Unit tests for Metal shader codegen summary helpers."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_metal_shader_codegen.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_metal_shader_codegen", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


CODEGEN = load_module()


class MetalShaderCodegenTests(unittest.TestCase):
    def test_parse_metal_size_output(self) -> None:
        parsed = CODEGEN.parse_metal_size(
            "   text    data     bss     dec     hex filename\n"
            "   7632       0       0    7632    1dd0 top.metallib\n"
        )

        self.assertEqual(parsed["metallib_text_bytes"], 7632)
        self.assertEqual(parsed["metallib_data_bytes"], 0)
        self.assertEqual(parsed["metallib_bss_bytes"], 0)
        self.assertEqual(parsed["metallib_dec_bytes"], 7632)

    def test_parse_ir_return_and_scratch_metrics(self) -> None:
        ir = """
define <{ <4 x float>, <4 x float>, float }> @dxmt9_vs(i32 %0) {
  %1 = alloca [8 x <4 x float>], align 16
  call void @llvm.lifetime.start.p0i8(i64 128, i8* %2)
  %3 = load <4 x float>, <4 x float>* %1
  %4 = call fast float @air.dot.v4f32(<4 x float> %3, <4 x float> %3)
  %5 = insertvalue <{ <4 x float>, <4 x float>, float }> undef, <4 x float> %3, 0
  br label %6
}
"""

        parsed = CODEGEN.parse_ir_metrics(ir)

        self.assertEqual(parsed["ir_return_field_count"], 3)
        self.assertEqual(parsed["ir_return_bytes"], 36)
        self.assertEqual(parsed["ir_alloca_count"], 1)
        self.assertEqual(parsed["ir_alloca_bytes"], 128)
        self.assertEqual(parsed["ir_scratch_bytes_estimate"], 128)
        self.assertEqual(parsed["ir_lifetime_start_bytes"], 128)
        self.assertEqual(parsed["ir_insertvalue_count"], 1)
        self.assertEqual(parsed["ir_air_dot_calls"], 1)

    def test_parse_ir_type_alias_alloca_and_memory_intrinsics(self) -> None:
        ir = """
%struct.StageScratch = type { <4 x float>, [3 x <4 x float>], i32 }

define %struct.StageScratch @dxmt9_vs(i32 %0) {
  %1 = alloca %struct.StageScratch, align 16
  %2 = alloca [2 x %struct.StageScratch], align 16
  call void @llvm.lifetime.start.p0i8(i64 204, i8* %3)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %4, i8* %5, i64 68, i1 false)
  call void @llvm.memset.p0i8.i64(i8* %4, i8 0, i64 68, i1 false)
  %6 = load float, float addrspace(1)* %7
  call void @llvm.lifetime.end.p0i8(i64 204, i8* %3)
}
"""

        parsed = CODEGEN.parse_ir_metrics(ir)

        self.assertEqual(parsed["ir_type_def_count"], 1)
        self.assertEqual(parsed["ir_max_type_def_bytes"], 68)
        self.assertEqual(parsed["ir_return_field_count"], 3)
        self.assertEqual(parsed["ir_return_bytes"], 68)
        self.assertEqual(parsed["ir_alloca_count"], 2)
        self.assertEqual(parsed["ir_alloca_array_count"], 1)
        self.assertEqual(parsed["ir_alloca_struct_count"], 2)
        self.assertEqual(parsed["ir_alloca_bytes"], 204)
        self.assertEqual(parsed["ir_lifetime_start_bytes"], 204)
        self.assertEqual(parsed["ir_lifetime_end_bytes"], 204)
        self.assertEqual(parsed["ir_scratch_bytes_estimate"], 204)
        self.assertEqual(parsed["ir_memcpy_count"], 1)
        self.assertEqual(parsed["ir_memset_count"], 1)
        self.assertEqual(parsed["ir_addrspace1_refs"], 1)


if __name__ == "__main__":
    unittest.main()
