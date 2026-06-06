#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_alpha_backend_candidates.py"


class AnalyzeAlphaBackendCandidatesTests(unittest.TestCase):
    def test_large_alpha_groups_reject_static_blend_off(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            probe_csv = root / "probe.csv"
            msl_dir = root / "msl"
            msl_dir.mkdir()
            output = root / "report.md"
            csv_output = root / "report.csv"

            with probe_csv.open("w", newline="", encoding="utf-8") as f:
                writer = csv.DictWriter(
                    f,
                    fieldnames=[
                        "seq",
                        "encoder",
                        "encoder_draw_index",
                        "primitive_count",
                        "vertex_count",
                        "texture_mask",
                        "color_write",
                        "alpha_blend",
                        "src_blend",
                        "dst_blend",
                        "blend_op",
                        "separate_alpha",
                        "depth_enabled",
                        "depth_write",
                        "scissor",
                        "pso",
                        "vs",
                        "ps",
                        "uniform_payload_hash",
                    ],
                )
                writer.writeheader()
                writer.writerow(
                    {
                        "seq": "60",
                        "encoder": "2",
                        "encoder_draw_index": "10",
                        "primitive_count": "5000",
                        "vertex_count": "15000",
                        "texture_mask": "0x7f",
                        "color_write": "0xf",
                        "alpha_blend": "1",
                        "src_blend": "10",
                        "dst_blend": "2",
                        "blend_op": "1",
                        "separate_alpha": "0",
                        "depth_enabled": "1",
                        "depth_write": "0",
                        "scissor": "1",
                        "pso": "0x1",
                        "vs": "0xaa",
                        "ps": "0x10",
                        "uniform_payload_hash": "0xa",
                    }
                )
                writer.writerow(
                    {
                        "seq": "60",
                        "encoder": "2",
                        "encoder_draw_index": "11",
                        "primitive_count": "6000",
                        "vertex_count": "18000",
                        "texture_mask": "0x7f",
                        "color_write": "0xf",
                        "alpha_blend": "1",
                        "src_blend": "5",
                        "dst_blend": "6",
                        "blend_op": "1",
                        "separate_alpha": "0",
                        "depth_enabled": "1",
                        "depth_write": "0",
                        "scissor": "0",
                        "pso": "0x2",
                        "vs": "0xbb",
                        "ps": "0x20",
                        "uniform_payload_hash": "0xb",
                    }
                )

            (msl_dir / "translated-fs-shader-16-source-1.metal").write_text(
                "fragment float4 main0() { float4 outColor[1]; "
                "outColor[0] = (r[0] * r[1] + r[2]); return outColor[0]; }\n",
                encoding="utf-8",
            )
            (msl_dir / "translated-fs-shader-32-source-2.metal").write_text(
                "fragment float4 main0() { float4 outColor[1]; "
                "outColor[0] = dxmt9_merge(outColor[0], "
                "float4(in.texcoord2.x), 8u); return outColor[0]; }\n",
                encoding="utf-8",
            )

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(probe_csv),
                    "--seq",
                    "60",
                    "--row",
                    "60/2",
                    "--msl-dir",
                    str(msl_dir),
                    "--output",
                    str(output),
                    "--csv-output",
                    str(csv_output),
                ],
                check=True,
                cwd=REPO_ROOT,
            )

            text = output.read_text(encoding="utf-8")
            self.assertIn("reject-screen-non-noop", text)
            self.assertIn("reject-alpha-not-static-one", text)
            self.assertIn("dynamic-expression", text)
            self.assertIn("varying-alpha", text)

            with csv_output.open(newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 2)
            self.assertEqual(
                {row["static_blend_off_verdict"] for row in rows},
                {"reject-screen-non-noop", "reject-alpha-not-static-one"},
            )


if __name__ == "__main__":
    unittest.main()
