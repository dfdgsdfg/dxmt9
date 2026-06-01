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
        self.assertEqual(parsed["ir_lifetime_start_bytes"], 128)
        self.assertEqual(parsed["ir_insertvalue_count"], 1)
        self.assertEqual(parsed["ir_air_dot_calls"], 1)


if __name__ == "__main__":
    unittest.main()
