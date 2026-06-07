#!/usr/bin/env python3

from __future__ import annotations

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_fragmentless_depth_route_gate.py"


def load_module():
    spec = importlib.util.spec_from_file_location("summarize_fragmentless_depth_route_gate", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    fields = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


class FragmentlessDepthRouteGateTests(unittest.TestCase):
    def test_route_reachable_blocks_xcode_when_equality_missing(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            enc = root / "enc.csv"
            summary = root / "summary.md"
            log = root / "dxmt9.log"
            write_csv(enc, [{
                "seq": "60",
                "encoder": "0",
                "indexed_draws": "42",
                "primitive_count": "97294",
                "vertex_count": "291882",
                "probe_fragmentless_depth_only_draws": "42",
                "probe_fragmentless_depth_only_primitives": "97294",
                "probe_fragmentless_depth_only_vertices": "291882",
                "vsout_layout_last": "0x0",
            }])
            summary.write_text(
                "| `present_encoded` | `1680` |\n"
                "| `draw_skipped_no_pipeline` | `0` |\n"
                "| `gpu_command_buffer_errors` | `0` |\n",
                encoding="utf-8",
            )
            log.write_text(
                "fragmentless depth-only probe accepted\n"
                "fragmentless depth-only probe accepted\n",
                encoding="utf-8",
            )
            args = module.parse_args.__globals__["argparse"].Namespace(
                encoder_csv=enc,
                target_row="60/0",
                perf_summary=summary,
                log=[log],
                equality_csv=None,
                xcode_baseline_csv=None,
                xcode_treatment_csv=None,
                min_route_coverage_pct=99.0,
                max_equality_changed_pct=0.0,
                max_equality_delta=0,
                min_vs_bytes_drop_pct=5.0,
                output=root / "out.md",
                csv_output=root / "out.csv",
            )
            row = module.build_gate_row(args)
            self.assertEqual(row["route_status"], "passed-route")
            self.assertEqual(row["overall_verdict"], "route-reachable-needs-equality")
            self.assertEqual(row["gputrace_readiness"], "blocked-needs-equality")

    def test_equality_pass_makes_xcode_counter_candidate(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            enc = root / "enc.csv"
            summary = root / "summary.md"
            equality = root / "equality.csv"
            write_csv(enc, [{
                "seq": "60",
                "encoder": "0",
                "indexed_draws": "1",
                "primitive_count": "10",
                "vertex_count": "30",
                "probe_fragmentless_depth_only_draws": "1",
                "probe_fragmentless_depth_only_primitives": "10",
                "probe_fragmentless_depth_only_vertices": "30",
                "vsout_layout_last": "0x0",
            }])
            summary.write_text(
                "| `present_encoded` | `1` |\n"
                "| `draw_skipped_no_pipeline` | `0` |\n"
                "| `gpu_command_buffer_errors` | `0` |\n",
                encoding="utf-8",
            )
            write_csv(equality, [{
                "area": "full",
                "changed_pct": "0.000000",
                "max_delta": "0",
            }])
            args = module.parse_args.__globals__["argparse"].Namespace(
                encoder_csv=enc,
                target_row="60/0",
                perf_summary=summary,
                log=[],
                equality_csv=equality,
                xcode_baseline_csv=None,
                xcode_treatment_csv=None,
                min_route_coverage_pct=99.0,
                max_equality_changed_pct=0.0,
                max_equality_delta=0,
                min_vs_bytes_drop_pct=5.0,
                output=root / "out.md",
                csv_output=root / "out.csv",
            )
            row = module.build_gate_row(args)
            self.assertEqual(row["overall_verdict"], "route-equal-ready-for-xcode")
            self.assertEqual(row["gputrace_readiness"], "ready-for-xcode-counters")

    def test_flat_xcode_counters_reject_route(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            enc = root / "enc.csv"
            summary = root / "summary.md"
            equality = root / "equality.csv"
            baseline = root / "base.csv"
            treatment = root / "treat.csv"
            write_csv(enc, [{
                "seq": "60",
                "encoder": "0",
                "indexed_draws": "1",
                "primitive_count": "10",
                "vertex_count": "30",
                "probe_fragmentless_depth_only_draws": "1",
                "probe_fragmentless_depth_only_primitives": "10",
                "probe_fragmentless_depth_only_vertices": "30",
                "vsout_layout_last": "0x0",
            }])
            summary.write_text(
                "| `present_encoded` | `1` |\n"
                "| `draw_skipped_no_pipeline` | `0` |\n"
                "| `gpu_command_buffer_errors` | `0` |\n",
                encoding="utf-8",
            )
            write_csv(equality, [{"area": "full", "changed_pct": "0", "max_delta": "0"}])
            write_csv(baseline, [{
                "seq": "60",
                "enc": "0",
                "vs_buffer_write_mib": "100",
                "vs_buffer_bytes_per_vs_invocation": "1000",
                "gpu_ms": "5",
            }])
            write_csv(treatment, [{
                "seq": "60",
                "enc": "0",
                "vs_buffer_write_mib": "99",
                "vs_buffer_bytes_per_vs_invocation": "995",
                "gpu_ms": "4.9",
            }])
            args = module.parse_args.__globals__["argparse"].Namespace(
                encoder_csv=enc,
                target_row="60/0",
                perf_summary=summary,
                log=[],
                equality_csv=equality,
                xcode_baseline_csv=baseline,
                xcode_treatment_csv=treatment,
                min_route_coverage_pct=99.0,
                max_equality_changed_pct=0.0,
                max_equality_delta=0,
                min_vs_bytes_drop_pct=5.0,
                output=root / "out.md",
                csv_output=root / "out.csv",
            )
            row = module.build_gate_row(args)
            self.assertEqual(row["counter_status"], "failed-counters")
            self.assertEqual(row["overall_verdict"], "rejected-counter-no-vswrite-move")

    def test_cli_writes_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            enc = root / "enc.csv"
            summary = root / "summary.md"
            out = root / "gate.md"
            out_csv = root / "gate.csv"
            write_csv(enc, [{
                "seq": "60",
                "encoder": "0",
                "indexed_draws": "1",
                "primitive_count": "3",
                "vertex_count": "9",
                "probe_fragmentless_depth_only_draws": "1",
                "probe_fragmentless_depth_only_primitives": "3",
                "probe_fragmentless_depth_only_vertices": "9",
                "vsout_layout_last": "0x0",
            }])
            summary.write_text(
                "| `present_encoded` | `1` |\n"
                "| `draw_skipped_no_pipeline` | `0` |\n"
                "| `gpu_command_buffer_errors` | `0` |\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--encoder-csv",
                    str(enc),
                    "--perf-summary",
                    str(summary),
                    "--output",
                    str(out),
                    "--csv-output",
                    str(out_csv),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn(str(out), result.stdout)
            self.assertTrue(out.exists())
            self.assertTrue(out_csv.exists())


if __name__ == "__main__":
    unittest.main()
