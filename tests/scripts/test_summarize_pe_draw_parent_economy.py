"""Fail-closed parser tests for the public-COM draw-parent matrix."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/tools/summarize_pe_draw_parent_economy.py"
spec = importlib.util.spec_from_file_location("pe_draw_parent_economy", SCRIPT)
module = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(module)


def row(case: str, path: str, groups: int = 2) -> str:
    calls = groups * 32 * (module.SETTERS[case] + (path != "setters_only"))
    return (f"draw_parent_row case={case} path={path} groups={groups} iterations=32 "
            f"qpc_ticks=100 emptyqpc_ticks=3 calls={calls} hresult_failures=0 "
            "status=0x00000000 initial=pass final=pass inputs=pass "
            "pixel_center=0xff00ff00 pixel_corner=0xff000000 cpu_scope=process "
            "cpu_includes=setup_warm_drain_oracle cpu_user_start_100ns=100 "
            "cpu_user_end_100ns=200 cpu_kernel_start_100ns=10 cpu_kernel_end_100ns=20 "
            "pid=123 tid=456")


def independent_completion_boundaries(warm_groups: int, groups: int,
                                      window: int) -> set[int]:
    """Synthetic producer schedule, intentionally independent of parser logic."""
    boundaries = set()
    start = 0
    for phase_groups in (warm_groups, groups):
        remaining = phase_groups
        while remaining:
            step = min(window, remaining)
            boundaries.add(start + step - 1)
            start += step
            remaining -= step
    return boundaries


def log(trace: bool = False) -> str:
    lines = ["draw_parent_meta schema=1 arch=x86 qpc_frequency=1000000 groups=2 warm_groups=1 iterations=32 trace=" + ("1" if trace else "0") + " pid=123 tid=456",
             "draw_parent_binding module=C:\\windows\\system32\\d3d9.dll machine=0x14c primitive_rva=0x101b0 indexed_rva=0x10200"]
    lines.extend(row(case, path) for case in module.CASES for path in module.PATHS)
    if trace:
        lines.append("pe_recorder_chunk seq=1 records=1")
        for case in module.CASES:
            for path in module.PATHS:
                for group in range(3):
                    for phase in ("begin", "timed_begin", "timed_end", "end"):
                        lines.append(f"draw_parent_trace phase={phase} case={case} path={path} group={group}")
    lines.extend(("draw_parent_release refs=0", "draw_parent_complete schema=1 rows=27 status=pass"))
    return "\n".join(lines)


def detail_log(warm_groups: int = 1, trace: bool = False,
               tail_detail: bool = False, groups: int = 2,
               completion_window: int | None = None) -> str:
    tail_detail = tail_detail or completion_window is not None
    schema = 4 if completion_window is not None else (3 if tail_detail else 2)
    meta = (f"draw_parent_meta schema={schema} arch=x86 qpc_frequency=1000000 groups={groups} "
            f"warm_groups={warm_groups} iterations=32 trace={'1' if trace else '0'} "
            + "group_detail=1 " + ("tail_detail=1 " if tail_detail else "")
            + (f"completion_window={completion_window} " if completion_window is not None else "")
            + "pid=123 tid=456")
    lines = [meta,
             "draw_parent_binding module=C:\\windows\\system32\\d3d9.dll machine=0x14c primitive_rva=0x101b0 indexed_rva=0x10200"]
    # The fixture captures all group samples before emitting the buffered row
    # records, so parser acceptance does not depend on per-row logging timing.
    sample_lines = []
    for case in module.CASES:
        for path in module.PATHS:
            for ordinal in range(warm_groups + groups):
                measured_ordinal = ordinal - warm_groups
                if ordinal < warm_groups:
                    qpc = 0
                elif groups == 2:
                    qpc = 40 if measured_ordinal == 0 else 60
                else:
                    qpc = 100 if measured_ordinal == 0 else 0
                polls = ordinal + 1
                if completion_window is not None:
                    polls = int(ordinal in independent_completion_boundaries(
                        warm_groups, groups, completion_window))
                sample_lines.append(f"draw_parent_group case={case} path={path} group={ordinal} "
                                    f"measured={int(ordinal >= warm_groups)} qpc_ticks={qpc} "
                                    f"tail_qpc_ticks={ordinal + 2} tail_getdata_calls={polls}")
                if tail_detail:
                    end_scene = ordinal
                    getdata = int(ordinal != 0)
                    if completion_window is not None and not polls:
                        getdata = 0
                    other = ordinal + 2 - end_scene - getdata
                    sample_lines.append(
                        f"draw_parent_tail case={case} path={path} group={ordinal} "
                        f"end_scene_qpc_ticks={end_scene} issue_qpc_ticks=0 "
                        f"getdata_qpc_ticks={getdata} other_qpc_ticks={other}")
    if trace:
        # Recorder evidence and the complete trace loop precede buffered detail
        # samples, matching the fixture's no-in-timed-pair publication order.
        trace_lines = ["pe_recorder_chunk seq=1 records=1"]
        for case in module.CASES:
            for path in module.PATHS:
                for group in range(warm_groups + groups):
                    for phase in ("begin", "timed_begin", "timed_end", "end"):
                        trace_lines.append(
                            f"draw_parent_trace phase={phase} case={case} path={path} group={group}")
        lines.extend(trace_lines)
    lines.extend(sample_lines)
    lines.extend(row(case, path, groups) for case in module.CASES for path in module.PATHS)
    lines.extend(("draw_parent_release refs=0",
                  f"draw_parent_complete schema={schema} rows=27 status=pass"))
    return "\n".join(lines)


class DrawParentEconomyParserTests(unittest.TestCase):
    def test_default_conformance_excludes_economy_unless_explicitly_selected(self) -> None:
        sys.path.insert(0, str(SCRIPT.parents[2]))
        from scripts.tools.run_d3d9_conformance import load_manifest_cases
        with tempfile.TemporaryDirectory() as folder:
            manifest = Path(folder) / "cases.toml"
            manifest.write_text('[[case]]\nexecutable="ordinary.exe"\nfunction="ordinary"\n'
                                '[[case]]\nexecutable="economy.exe"\nfunction="economy"\non_demand=true\n')
            self.assertEqual(load_manifest_cases(manifest, include_on_demand=False),
                             {"ordinary.exe": ["ordinary"]})
            self.assertEqual(load_manifest_cases(manifest, include_on_demand=True),
                             {"ordinary.exe": ["ordinary"], "economy.exe": ["economy"]})
        actual = SCRIPT.parents[2] / "tests/conformance/d3d9/MANIFEST.toml"
        for arch in ("x86", "x64"):
            name = f"d3d9_draw_parent_economy_{arch}.exe"
            self.assertNotIn(name, load_manifest_cases(actual, include_on_demand=False))
            self.assertEqual(load_manifest_cases(actual, include_on_demand=True)[name],
                             ["runDrawParentEconomy"])

    def test_driver_cli_imports_from_canonical_script_path(self) -> None:
        result = subprocess.run([sys.executable, str(SCRIPT.with_name("run_pe_draw_parent_economy.py")),
                                 "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--mode", result.stdout)
        self.assertIn("--group-detail", result.stdout)
        self.assertIn("--tail-detail", result.stdout)
        self.assertIn("--completion-window", result.stdout)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "invalid-output"
            invalid = subprocess.run(
                [sys.executable, str(SCRIPT.with_name("run_pe_draw_parent_economy.py")),
                 "--arch", "x86", "--completion-window", "0", "--output", str(output)],
                capture_output=True, text=True)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertFalse(output.exists())

    def test_runner_filenames_are_not_owned_records(self) -> None:
        text = ("[runner] exe=/tmp/d3d9_draw_parent_economy_x86.exe\n\n" + log() +
                "\n[runner] aux d3d9_draw_parent_economy_x86.exe pass=1 fail=0\n")
        self.assertEqual(len(module.parse_text(text)["rows"]), 27)

    def test_each_state_flag_and_trailing_failure_is_rejected(self) -> None:
        for field in ("initial", "final", "inputs"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                module.parse_text(log().replace(f"{field}=pass", f"{field}=fail", 1))
        for line in ("FAIL: teardown", "SKIP: unsupported", "[d3d9-pe] error: bridge failed",
                     "wine: Unhandled exception", "draw_parent_unknown value=1"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                module.parse_text(log() + "\n" + line)

    def test_meta_binding_and_foreign_identity_reject(self) -> None:
        for old, new in (("qpc_frequency=1000000", "qpc_frequency=0"),
                         ("schema=1", "schema=2"),
                         ("machine=0x14c", "machine=0x8664"),
                         ("primitive_rva=0x101b0", "primitive_rva=0x0"),
                         ("qpc_ticks=100", "qpc_ticks=18446744073709551616"),
                         ("cpu_scope=process", "cpu_scope=thread")):
            with self.subTest(new=new), self.assertRaises(ValueError):
                module.parse_text(log().replace(old, new, 1))
        for record in (log().splitlines()[0], log().splitlines()[1]):
            with self.subTest(record=record), self.assertRaises(ValueError):
                module.parse_text(record + "\n" + log())

    def test_prefixed_timed_flush_and_multiple_markers_reject(self) -> None:
        marker = "draw_parent_trace phase=timed_end case=steady path=setters_only group=0"
        bad = log(True).replace(marker, "[d3d9-pe] info: pe_recorder_chunk seq=99 records=1\n" + marker, 1)
        with self.assertRaises(ValueError):
            module.parse_text(bad)
        with self.assertRaises(ValueError):
            module.parse_text(log() + "\ndraw_parent_release refs=0 draw_parent_release refs=0")

    def test_complete_timing_matrix_and_raw_rows(self) -> None:
        report = module.parse_text(log(), expected_arch="x86", expected_trace=False, expected_groups=2)
        self.assertEqual(len(report["rows"]), 27)
        self.assertEqual(report["rows"][0]["qpc_ticks"], 100)
        self.assertEqual(report["rows"][0]["calls"], 320)
        self.assertFalse(report["meta"]["trace"])
        self.assertNotIn("group_detail", report["meta"])

    def test_trace_markers_are_balanced_and_outside_timed_pairs(self) -> None:
        report = module.parse_text(log(True), expected_arch="x86", expected_trace=True, expected_groups=2)
        self.assertTrue(report["meta"]["trace"])

    def test_logger_prefixes_are_extracted_and_trace_requires_chunk_evidence(self) -> None:
        prefixed = "\n".join("[dxmt9] " + line for line in log(True).splitlines())
        self.assertEqual(len(module.parse_text(prefixed, expected_arch="x86",
                                                expected_trace=True, expected_groups=2)["rows"]), 27)
        without_chunk = log(True).replace("pe_recorder_chunk seq=1 records=1\n", "")
        with self.assertRaises(ValueError):
            module.parse_text(without_chunk, expected_arch="x86", expected_trace=True, expected_groups=2)

    def test_duplicate_and_missing_rows_reject(self) -> None:
        text = log()
        first = row(module.CASES[0], module.PATHS[0])
        with self.assertRaises(ValueError):
            module.parse_text(text + "\n" + first)
        with self.assertRaises(ValueError):
            module.parse_text(text.replace(first + "\n", "", 1))

    def test_unexpected_row_and_invalid_fields_reject(self) -> None:
        text = log()
        mutations = (
            ("case=steady path=setters_only", "case=unknown path=setters_only"),
            ("qpc_ticks=100", "qpc_ticks=0"),
            ("calls=0", "calls=1"),
            ("hresult_failures=0", "hresult_failures=1"),
            ("status=0x00000000", "status=0x00000001"),
            ("pixel_center=0xff00ff00", "pixel_center=0xff000000"),
            ("cpu_user_end_100ns=200", "cpu_user_end_100ns=99"),
            ("pid=123 tid=456", "pid=124 tid=456"),
        )
        for old, new in mutations:
            with self.subTest(new=new), self.assertRaises(ValueError):
                module.parse_text(text.replace(old, new, 1))

    def test_missing_completion_release_and_trace_marker_reject(self) -> None:
        text = log()
        for marker in ("draw_parent_release refs=0", "draw_parent_complete schema=1 rows=27 status=pass"):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                module.parse_text(text.replace(marker, "", 1))
        traced = log(True)
        marker = "draw_parent_trace phase=timed_end case=steady path=setters_only group=0"
        with self.assertRaises(ValueError):
            module.parse_text(traced.replace(marker, "", 1))

    def test_chunk_flush_inside_timed_pair_and_unknown_record_reject(self) -> None:
        traced = log(True)
        marker = "draw_parent_trace phase=timed_end case=steady path=setters_only group=0"
        bad = traced.replace(marker, "pe_recorder_chunk seq=99 records=1\n" + marker, 1)
        with self.assertRaises(ValueError):
            module.parse_text(bad)
        with self.assertRaises(ValueError):
            module.parse_text(log() + "\ndraw_parent_unknown field=1")

    def test_schema2_group_detail_preserves_samples_and_distributions(self) -> None:
        report = module.parse_text(detail_log(), expected_group_detail=True)
        self.assertEqual(report["schema"], "dxmt9-pe-draw-parent-economy-v2")
        self.assertEqual(len(report["group_rows"]), 81)
        measured = report["group_distributions"]["steady/setters_only"]["measured"]["qpc_ticks"]
        self.assertEqual(measured, {"min": 40, "p50": 40, "p90": 60, "p99": 60,
                                    "max": 60, "sum": 100, "count": 2})
        warm = report["group_distributions"]["steady/setters_only"]["warm"]["qpc_ticks"]
        self.assertEqual(warm["sum"], 0)
        self.assertEqual(warm["count"], 1)

    def test_schema2_group_detail_fail_closed_mutations(self) -> None:
        text = detail_log()
        mutations = (
            ("schema=2", "schema=1"),
            ("group_detail=1", "group_detail=0"),
            ("draw_parent_complete schema=2", "draw_parent_complete schema=1"),
            ("measured=1 qpc_ticks=40", "measured=0 qpc_ticks=40"),
            ("tail_getdata_calls=1", "tail_getdata_calls=0"),
            ("qpc_ticks=40", "qpc_ticks=18446744073709551616"),
        )
        for old, new in mutations:
            with self.subTest(new=new), self.assertRaises(ValueError):
                module.parse_text(text.replace(old, new, 1))
        first = "draw_parent_group case=steady path=setters_only group=0 measured=0 qpc_ticks=0 tail_qpc_ticks=2 tail_getdata_calls=1"
        with self.assertRaises(ValueError):
            module.parse_text(text.replace(first + "\n", "", 1))
        with self.assertRaises(ValueError):
            module.parse_text(text.replace(first, first + "\n" + first, 1))
        row_text = row("steady", "setters_only")
        with self.assertRaises(ValueError):
            module.parse_text(text.replace(row_text, row_text + "\n" + first, 1))
        traced = detail_log(trace=True)
        moved = traced.replace(first + "\n", "", 1)
        timed_begin = "draw_parent_trace phase=timed_begin case=steady path=setters_only group=0"
        moved = moved.replace(timed_begin, timed_begin + "\n" + first, 1)
        with self.assertRaises(ValueError):
            module.parse_text(moved)

    def test_schema2_measured_sum_and_zero_warm_groups(self) -> None:
        report = module.parse_text(detail_log(warm_groups=0), expected_group_detail=True)
        self.assertEqual(report["group_distributions"]["steady/setters_only"]["warm"]["qpc_ticks"]["count"], 0)
        bad = detail_log().replace("qpc_ticks=40 tail_qpc_ticks=3", "qpc_ticks=41 tail_qpc_ticks=3", 1)
        with self.assertRaises(ValueError):
            module.parse_text(bad)
        overflow = detail_log().replace(
            "qpc_ticks=40 tail_qpc_ticks=3", "qpc_ticks=18446744073709551615 tail_qpc_ticks=3", 1)
        with self.assertRaises(ValueError):
            module.parse_text(overflow)
        traced = module.parse_text(detail_log(trace=True), expected_group_detail=True,
                                   expected_trace=True)
        self.assertTrue(traced["meta"]["trace"])

    def test_schema3_tail_detail_preserves_schema2_and_distributions(self) -> None:
        schema2 = module.parse_text(detail_log(), expected_group_detail=True)
        report = module.parse_text(detail_log(tail_detail=True), expected_group_detail=True,
                                   expected_tail_detail=True)
        self.assertEqual(report["schema"], "dxmt9-pe-draw-parent-economy-v3")
        self.assertEqual(report["rows"], schema2["rows"])
        self.assertEqual(report["group_rows"], schema2["group_rows"])
        self.assertTrue(report["meta"]["group_detail"])
        self.assertTrue(report["meta"]["tail_detail"])
        self.assertNotIn("completion_window", report["meta"])
        self.assertEqual(len(report["tail_rows"]), 81)
        self.assertEqual(report["tail_rows"][0], {
            "case": "steady", "path": "setters_only", "group": 0,
            "end_scene_qpc_ticks": 0, "issue_qpc_ticks": 0,
            "getdata_qpc_ticks": 0, "other_qpc_ticks": 2})
        self.assertEqual(report["tail_distributions"]["steady/setters_only"]["measured"]["end_scene_qpc_ticks"],
                         {"min": 1, "p50": 1, "p90": 2, "p99": 2, "max": 2,
                          "sum": 3, "count": 2})
        traced = module.parse_text(detail_log(trace=True, tail_detail=True),
                                   expected_group_detail=True, expected_tail_detail=True,
                                   expected_trace=True)
        self.assertEqual(len(traced["tail_rows"]), 81)

    def test_schema3_tail_detail_fail_closed_mutations(self) -> None:
        text = detail_log(tail_detail=True)
        tail = ("draw_parent_tail case=steady path=setters_only group=0 "
                "end_scene_qpc_ticks=0 issue_qpc_ticks=0 getdata_qpc_ticks=0 "
                "other_qpc_ticks=2")
        for old, new in (
                ("schema=3", "schema=2"),
                ("tail_detail=1", "tail_detail=0"),
                ("draw_parent_complete schema=3", "draw_parent_complete schema=2"),
                ("case=steady path=setters_only group=0", "case=unknown path=setters_only group=0"),
                ("other_qpc_ticks=2", "other_qpc_ticks=1"),
                ("end_scene_qpc_ticks=0", "end_scene_qpc_ticks=18446744073709551616"),
        ):
            with self.subTest(new=new), self.assertRaises(ValueError):
                module.parse_text(text.replace(old, new, 1))
        with self.assertRaises(ValueError):
            module.parse_text(text.replace(tail + "\n", "", 1))
        with self.assertRaises(ValueError):
            module.parse_text(text.replace(tail, tail + "\n" + tail, 1))
        lines = text.splitlines()
        tail_index = next(index for index, line in enumerate(lines) if line == tail)
        lines[tail_index], lines[tail_index + 2] = lines[tail_index + 2], lines[tail_index]
        with self.assertRaises(ValueError):
            module.parse_text("\n".join(lines))
        with self.assertRaises(ValueError):
            module.parse_text(detail_log(), expected_tail_detail=True)
        with self.assertRaises(ValueError):
            module.parse_text(log() + "\n" + tail)

    def test_schema3_zero_tail_components_and_tail_failure_markers(self) -> None:
        zero = detail_log(tail_detail=True)
        report = module.parse_text(zero, expected_tail_detail=True)
        self.assertEqual(report["tail_rows"][0]["end_scene_qpc_ticks"], 0)
        self.assertEqual(report["tail_rows"][0]["issue_qpc_ticks"], 0)
        for marker in ("FAIL: tail", "SKIP: tail", "[d3d9-pe] error: tail failure"):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                module.parse_text(zero + "\n" + marker)

    def test_tail_warm_phase_is_separate_and_empty_warm_is_bounded(self) -> None:
        text = detail_log(tail_detail=True)
        text = text.replace("tail_qpc_ticks=2 tail_getdata_calls=1",
                            "tail_qpc_ticks=25 tail_getdata_calls=1", 1)
        text = text.replace("end_scene_qpc_ticks=0 issue_qpc_ticks=0",
                            "end_scene_qpc_ticks=23 issue_qpc_ticks=0", 1)
        report = module.parse_text(text, expected_tail_detail=True)
        phases = report["tail_distributions"]["steady/setters_only"]
        self.assertEqual(phases["warm"]["end_scene_qpc_ticks"]["sum"], 23)
        self.assertEqual(phases["measured"]["end_scene_qpc_ticks"]["sum"], 3)
        empty = module.parse_text(detail_log(warm_groups=0, tail_detail=True))
        self.assertEqual(empty["tail_distributions"]["steady/setters_only"]["warm"]["getdata_qpc_ticks"],
                         {"count": 0, "sum": 0, "min": None, "p50": None,
                          "p90": None, "p99": None, "max": None})

    def test_tail_only_identity_mismatch_and_component_sum_overflow_reject(self) -> None:
        text = detail_log(tail_detail=True)
        bad = text.replace("draw_parent_tail case=steady path=setters_only group=0",
                           "draw_parent_tail case=steady path=primitive group=0", 1)
        with self.assertRaises(ValueError):
            module.parse_text(bad)
        with self.assertRaises(ValueError):
            module.parse_text(text.replace("end_scene_qpc_ticks=0 issue_qpc_ticks=0",
                                          "end_scene_qpc_ticks=18446744073709551615 issue_qpc_ticks=1", 1))
        with self.assertRaises(ValueError):
            module.parse_text(text, expected_tail_detail=False)

    def test_schema4_completion_windows_boundary_truth_and_summary(self) -> None:
        for window in (2, 8, 64):
            for warm_groups, groups in ((0, 1), (3, 5), (17, 73)):
                with self.subTest(window=window, warm_groups=warm_groups, groups=groups):
                    report = module.parse_text(
                        detail_log(warm_groups=warm_groups, groups=groups,
                                   completion_window=window),
                        expected_group_detail=True, expected_tail_detail=True,
                        expected_completion_window=window)
                    self.assertEqual(report["schema"], "dxmt9-pe-draw-parent-economy-v4")
                    self.assertEqual(report["meta"]["completion_window"], window)
                    summary = report["completion_summary"]["steady/setters_only"]
                    expected = independent_completion_boundaries(warm_groups, groups, window)
                    expected_warm = sum(ordinal in expected for ordinal in range(warm_groups))
                    expected_measured = sum(ordinal in expected
                                            for ordinal in range(warm_groups, warm_groups + groups))
                    self.assertEqual(summary["warm"]["count"], expected_warm)
                    self.assertEqual(summary["measured"]["count"], expected_measured)
                    self.assertEqual(len(report["group_rows"]), 27 * (warm_groups + groups))
                    if window in (8, 64) and warm_groups == 17 and groups == 73:
                        self.assertGreater(expected_measured, 1)
        warm_nonzero = detail_log(warm_groups=3, groups=5, completion_window=8)
        warm_nonzero = warm_nonzero.replace(
            "group=0 measured=0 qpc_ticks=0 tail_qpc_ticks=2",
            "group=0 measured=0 qpc_ticks=7 tail_qpc_ticks=2", 1)
        report = module.parse_text(warm_nonzero, expected_completion_window=8)
        self.assertEqual(report["group_distributions"]["steady/setters_only"]["warm"]["qpc_ticks"]["sum"], 7)

    def test_schema4_rejects_interior_or_missing_phase_completion(self) -> None:
        text = detail_log(warm_groups=3, groups=5, completion_window=2)
        # Group 3 is the first, non-boundary measured group; Issue/GetData must
        # remain absent there.
        interior = text.replace(
            "draw_parent_group case=steady path=setters_only group=3 measured=1 "
            "qpc_ticks=100 tail_qpc_ticks=5 tail_getdata_calls=0",
            "draw_parent_group case=steady path=setters_only group=3 measured=1 "
            "qpc_ticks=100 tail_qpc_ticks=5 tail_getdata_calls=1", 1)
        with self.assertRaises(ValueError):
            module.parse_text(interior)
        interior_tail = text.replace(
            "end_scene_qpc_ticks=3 issue_qpc_ticks=0 getdata_qpc_ticks=0 other_qpc_ticks=2",
            "end_scene_qpc_ticks=3 issue_qpc_ticks=1 getdata_qpc_ticks=0 other_qpc_ticks=1", 1)
        with self.assertRaises(ValueError):
            module.parse_text(interior_tail)
        interior_getdata = text.replace(
            "end_scene_qpc_ticks=3 issue_qpc_ticks=0 getdata_qpc_ticks=0 other_qpc_ticks=2",
            "end_scene_qpc_ticks=3 issue_qpc_ticks=0 getdata_qpc_ticks=1 other_qpc_ticks=1", 1)
        with self.assertRaises(ValueError):
            module.parse_text(interior_getdata)
        drain = detail_log(warm_groups=3, groups=5, completion_window=8)
        missing_warm = drain
        group = 2
        old = f"group={group} measured="
        start = missing_warm.index("draw_parent_group case=steady path=setters_only " + old)
        end = missing_warm.index("\n", start)
        line = missing_warm[start:end]
        self.assertIn("tail_getdata_calls=1", line)
        missing_warm = (missing_warm[:start]
                        + line.replace("tail_getdata_calls=1", "tail_getdata_calls=0")
                        + missing_warm[end:])
        with self.assertRaises(ValueError):
            module.parse_text(missing_warm)
        missing_measured = drain
        group = 7
        old = f"group={group} measured="
        start = missing_measured.index("draw_parent_group case=steady path=setters_only " + old)
        end = missing_measured.index("\n", start)
        line = missing_measured[start:end]
        self.assertIn("tail_getdata_calls=1", line)
        missing_measured = (missing_measured[:start]
                            + line.replace("tail_getdata_calls=1", "tail_getdata_calls=0")
                            + missing_measured[end:])
        with self.assertRaises(ValueError):
            module.parse_text(missing_measured)

    def test_schema4_bounds_versions_and_legacy_window(self) -> None:
        self.assertNotIn("completion_window",
                         module.parse_text(log(), expected_completion_window=1)["meta"])
        base = detail_log(completion_window=2)
        for window in (1, 0, 65, 18446744073709551616):
            with self.subTest(window=window), self.assertRaises(ValueError):
                module.parse_text(base.replace("completion_window=2", f"completion_window={window}", 1))
        text = detail_log(warm_groups=3, groups=5, completion_window=8)
        for old, new in (
                ("completion_window=8", "completion_window=2"),
                ("draw_parent_complete schema=4", "draw_parent_complete schema=3"),
                ("tail_getdata_calls=0", "tail_getdata_calls=18446744073709551616"),
                ("end_scene_qpc_ticks=0 issue_qpc_ticks=0 getdata_qpc_ticks=0",
                 "end_scene_qpc_ticks=18446744073709551615 issue_qpc_ticks=1 getdata_qpc_ticks=1"),
        ):
            with self.subTest(new=new), self.assertRaises(ValueError):
                module.parse_text(text.replace(old, new, 1))
        with self.assertRaises(ValueError):
            module.parse_text(text, expected_completion_window=2)

    def test_detail_warm_is_not_measured_and_full_sequence_is_required(self) -> None:
        text = detail_log().replace("measured=0 qpc_ticks=0", "measured=0 qpc_ticks=97")
        report = module.parse_text(text, expected_group_detail=True)
        self.assertEqual(report["group_distributions"]["steady/setters_only"]["warm"]["qpc_ticks"]["sum"], 97)
        self.assertEqual(report["group_distributions"]["steady/setters_only"]["measured"]["qpc_ticks"]["sum"], 100)
        lines = text.splitlines()
        samples = [index for index, line in enumerate(lines) if line.startswith("draw_parent_group ")]
        lines[samples[0]], lines[samples[1]] = lines[samples[1]], lines[samples[0]]
        with self.assertRaises(ValueError):
            module.parse_text("\n".join(lines))
        with self.assertRaises(ValueError):
            module.parse_text(log().replace("draw_parent_release refs=0", lines[samples[0]] + "\ndraw_parent_release refs=0"))
        with self.assertRaises(ValueError):
            module.parse_text(text, expected_group_detail=False)
        with self.assertRaises(ValueError):
            module.parse_text(log(), expected_group_detail=True)

    def test_exact_nearest_rank_distribution_and_aggregate_overflow(self) -> None:
        report = module.distribution(list(range(1, 101)))
        self.assertEqual(report, {"count": 100, "sum": 5050, "min": 1, "p50": 50,
                                  "p90": 90, "p99": 99, "max": 100})
        self.assertEqual(module.distribution([])["p99"], None)
        with self.assertRaises(ValueError):
            module.distribution([module.UINT64_MAX, 1])
        for field in ("tail_qpc_ticks", "tail_getdata_calls"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                module.parse_text(detail_log().replace(f"{field}=2", f"{field}=18446744073709551616", 1))


if __name__ == "__main__":
    unittest.main()
