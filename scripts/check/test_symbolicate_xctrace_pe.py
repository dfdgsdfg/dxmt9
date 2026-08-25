#!/usr/bin/env python3
"""Tests for scripts/tools/symbolicate_xctrace_pe.py.

Uses small, hand-written synthetic fixtures: a time-profile XML with interned
thread/weight/backtrace defs (mirroring what xctrace actually emits, per
scripts/tools/summarize_xctrace_cpu_threads.py's parsing approach) and a
synthetic dxmt9-pe-module-map log line set.
"""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "symbolicate_xctrace_pe.py"


TIME_PROFILE_XML = textwrap.dedent(
    """\
    <trace-query-result>
      <node>
        <row>
          <sample-time id="t1" fmt="00:00.001">1000</sample-time>
          <thread id="thA" fmt="game.exe (0xaaa) (game.exe, pid: 42)"/>
          <process id="p1" fmt="game.exe (42)"/>
          <weight id="w1" fmt="2.00 ms">2000000</weight>
          <tagged-backtrace id="bt1">
            <backtrace>
              <frame id="f1" name="0x10000500"/>
            </backtrace>
          </tagged-backtrace>
        </row>
        <row>
          <sample-time id="t2" fmt="00:00.002">2000</sample-time>
          <thread ref="thA"/>
          <process ref="p1"/>
          <weight id="w2" fmt="1.00 ms">1000000</weight>
          <tagged-backtrace id="bt2">
            <backtrace>
              <frame id="f2" name="0x10000600"/>
            </backtrace>
          </tagged-backtrace>
        </row>
        <row>
          <sample-time id="t3" fmt="00:00.003">3000</sample-time>
          <thread ref="thA"/>
          <process ref="p1"/>
          <weight id="w3" fmt="1.00 ms">1000000</weight>
          <tagged-backtrace id="bt3">
            <backtrace>
              <frame id="f3" name="0x20000000"/>
            </backtrace>
          </tagged-backtrace>
        </row>
        <row>
          <sample-time id="t4" fmt="00:00.004">4000</sample-time>
          <thread ref="thA"/>
          <process ref="p1"/>
          <weight id="w4" fmt="1.00 ms">1000000</weight>
          <tagged-backtrace id="bt4">
            <backtrace>
              <frame id="f4" name="0x123456789"/>
            </backtrace>
          </tagged-backtrace>
        </row>
        <row>
          <sample-time id="t5" fmt="00:00.005">5000</sample-time>
          <thread ref="thA"/>
          <process ref="p1"/>
          <weight id="w5" fmt="1.00 ms">1000000</weight>
          <tagged-backtrace id="bt5">
            <backtrace>
              <frame id="f5" name="dyld_stub_binder">
                <binary id="b1" name="libdyld.dylib"/>
              </frame>
            </backtrace>
          </tagged-backtrace>
        </row>
        <row>
          <sample-time id="t6" fmt="00:00.006">6000</sample-time>
          <thread ref="thA"/>
          <process ref="p1"/>
          <weight id="w6" fmt="1.00 ms">1000000</weight>
        </row>
        <row>
          <sample-time id="t7" fmt="00:00.007">7000</sample-time>
          <thread id="thB" fmt="game.exe (0xbbb) (game.exe, pid: 42)"/>
          <process ref="p1"/>
          <weight id="w7" fmt="0.50 ms">500000</weight>
          <tagged-backtrace id="bt7">
            <backtrace>
              <frame id="f7" name="0x10000500"/>
            </backtrace>
          </tagged-backtrace>
        </row>
      </node>
    </trace-query-result>
    """
)


def module_map_log(probe_addr: str, contained: str) -> str:
    return textwrap.dedent(
        f"""\
        [dxmt9-pe-module-map] Info: module=d3d9.dll base=0x10000000 size=0x100000
        [dxmt9-pe-module-map] Info: module=winemetal_dxmt9.dll base=0x10200000 size=0x10000
        [dxmt9-pe-module-map] Info: probe=dxmt9PeModuleMapProbeMarker addr={probe_addr} contained={contained}
        """
    )


# Two cumulative [dxmt9-pe-sampler] groups from one run. The counters never
# reset, so the second group supersedes the first entirely and the tool must
# read only it. The module map from the same run supplies the RVA base.
SAMPLER_LOG = textwrap.dedent(
    """\
    [dxmt9-pe-module-map] info: module=game.exe base=0x00400000 size=0x50000
    [dxmt9-pe-module-map] info: module=d3d9.dll base=0x10000000 size=0x100000
    [dxmt9-pe-module-map] info: probe=dxmt9PeModuleMapProbeMarker addr=0x10000500 contained=1
    [dxmt9-pe-sampler] info: started thread_id=0xaaa hz=250 interval_ms=4
    [dxmt9-pe-sampler] info: presents=60 samples=100 suspend_failures=0 ctx_failures=0 resume_failures=0 hz=250 module_table=1
    [dxmt9-pe-sampler] info: module=game.exe samples=60
    [dxmt9-pe-sampler] info: module=d3d9.dll samples=30
    [dxmt9-pe-sampler] info: module=unknown samples=10
    [dxmt9-pe-sampler] info: selfpc_module=d3d9.dll
    [dxmt9-pe-sampler] info: selfpc bucket=0x10000440 samples=20
    [dxmt9-pe-sampler] info: selfpc bucket=0x10000480 samples=10
    [dxmt9-pe-sampler] info: selfpc_overflow=0
    [dxmt9-pe-sampler] info: presents=120 samples=200 suspend_failures=1 ctx_failures=2 resume_failures=0 hz=250 module_table=1
    [dxmt9-pe-sampler] info: module=game.exe samples=120
    [dxmt9-pe-sampler] info: module=d3d9.dll samples=60
    [dxmt9-pe-sampler] info: module=unknown samples=20
    [dxmt9-pe-sampler] info: selfpc_module=d3d9.dll
    [dxmt9-pe-sampler] info: selfpc bucket=0x10000440 samples=40
    [dxmt9-pe-sampler] info: selfpc bucket=0x10000480 samples=20
    [dxmt9-pe-sampler] info: selfpc_overflow=3
    """
)

# Equal-count module and bucket rows, to pin the secondary sort keys.
SAMPLER_TIE_LOG = textwrap.dedent(
    """\
    [dxmt9-pe-module-map] info: module=d3d9.dll base=0x10000000 size=0x100000
    [dxmt9-pe-module-map] info: probe=dxmt9PeModuleMapProbeMarker addr=0x10000500 contained=1
    [dxmt9-pe-sampler] info: presents=60 samples=30 suspend_failures=0 ctx_failures=0 resume_failures=0 hz=100 module_table=1
    [dxmt9-pe-sampler] info: module=zlib.dll samples=10
    [dxmt9-pe-sampler] info: module=d3d9.dll samples=10
    [dxmt9-pe-sampler] info: module=advapi32.dll samples=10
    [dxmt9-pe-sampler] info: selfpc_module=d3d9.dll
    [dxmt9-pe-sampler] info: selfpc bucket=0x10000880 samples=5
    [dxmt9-pe-sampler] info: selfpc bucket=0x10000040 samples=5
    [dxmt9-pe-sampler] info: selfpc_overflow=0
    """
)


class SamplerLogModeTests(unittest.TestCase):
    def _run(self, td: Path, log_text: str, *extra: str) -> tuple[subprocess.CompletedProcess, Path, Path, Path]:
        sampler_log = td / "run.log"
        sampler_log.write_text(log_text, encoding="utf-8")
        output_csv = td / "out.csv"
        output_md = td / "out.md"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--sampler-log",
                str(sampler_log),
                "--output-csv",
                str(output_csv),
                "--output-md",
                str(output_md),
                *extra,
            ],
            capture_output=True,
            text=True,
        )
        return result, output_csv, output_md, td / "out-selfpc.csv"

    def test_last_emission_group_wins(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            result, output_csv, output_md, _ = self._run(td, SAMPLER_LOG)
            self.assertEqual(result.returncode, 0, result.stderr)

            with output_csv.open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            by_module = {row["module"]: row for row in rows}
            # The first group's 60/30/10 must be invisible: counters are
            # cumulative, so only the 120/60/20 group counts.
            self.assertEqual(int(by_module["game.exe"]["samples"]), 120)
            self.assertEqual(int(by_module["d3d9.dll"]["samples"]), 60)
            self.assertEqual(int(by_module["unknown"]["samples"]), 20)
            # share_of_thread uses the header's own total (200), not the sum of
            # the printed top-N rows.
            self.assertAlmostEqual(float(by_module["game.exe"]["share_of_thread"]), 0.6)
            # weight_ms is derived from hz: 120 samples at 250 Hz = 480 ms.
            self.assertAlmostEqual(float(by_module["game.exe"]["weight_ms"]), 480.0)

            md_text = output_md.read_text(encoding="utf-8")
            self.assertIn("PASS", md_text)
            self.assertIn("250 Hz", md_text)
            self.assertIn("Samples: 200", md_text)
            # Failure counts come from the last group too.
            self.assertIn("Suspend failures: 1", md_text)

    def test_selfpc_rva_is_relative_to_the_named_module(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            result, _, output_md, selfpc_csv = self._run(td, SAMPLER_LOG)
            self.assertEqual(result.returncode, 0, result.stderr)

            with selfpc_csv.open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            self.assertEqual(len(rows), 2)
            # d3d9.dll base is 0x10000000, so 0x10000440 -> RVA 0x440.
            self.assertEqual(rows[0]["bucket"], "0x10000440")
            self.assertEqual(rows[0]["rva"], "0x440")
            self.assertEqual(int(rows[0]["samples"]), 40)
            self.assertEqual(rows[1]["bucket"], "0x10000480")
            self.assertEqual(rows[1]["rva"], "0x480")
            self.assertEqual(int(rows[1]["samples"]), 20)

            md_text = output_md.read_text(encoding="utf-8")
            self.assertIn("base=0x10000000", md_text)
            self.assertIn("Buckets dropped to overflow: 3", md_text)

    def test_missing_module_map_leaves_rva_empty_but_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            # Same sampler groups, module-map lines stripped.
            stripped = "\n".join(
                line
                for line in SAMPLER_LOG.splitlines()
                if "[dxmt9-pe-module-map]" not in line
            ) + "\n"
            result, _, output_md, selfpc_csv = self._run(td, stripped)
            self.assertEqual(result.returncode, 0, result.stderr)
            with selfpc_csv.open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            self.assertEqual([row["rva"] for row in rows], ["", ""])
            self.assertIn("RVA unresolved", output_md.read_text(encoding="utf-8"))
            self.assertIn("unvalidated", result.stderr)

    def test_deterministic_output_ordering_and_tie_breaks(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            outputs = []
            for _ in range(3):
                result, output_csv, _, selfpc_csv = self._run(td, SAMPLER_TIE_LOG)
                self.assertEqual(result.returncode, 0, result.stderr)
                outputs.append(
                    (
                        output_csv.read_text(encoding="utf-8"),
                        selfpc_csv.read_text(encoding="utf-8"),
                    )
                )
            self.assertEqual(outputs[0], outputs[1])
            self.assertEqual(outputs[1], outputs[2])

            with (td / "out.csv").open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            # Equal sample counts order by module name ascending.
            self.assertEqual(
                [row["module"] for row in rows],
                ["advapi32.dll", "d3d9.dll", "zlib.dll"],
            )
            with (td / "out-selfpc.csv").open(newline="", encoding="utf-8") as fh:
                self_rows = list(csv.DictReader(fh))
            # Equal sample counts order by bucket address ascending.
            self.assertEqual(
                [row["bucket"] for row in self_rows],
                ["0x10000040", "0x10000880"],
            )

    def test_no_sampler_group_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            result, _, _, _ = self._run(
                td,
                "[dxmt9-pe-module-map] info: module=d3d9.dll base=0x10000000 size=0x100000\n",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no [dxmt9-pe-sampler] emission group", result.stderr)

    def test_modes_are_mutually_exclusive(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            time_profile = td / "time-profile.xml"
            time_profile.write_text(TIME_PROFILE_XML, encoding="utf-8")
            sampler_log = td / "run.log"
            sampler_log.write_text(SAMPLER_LOG, encoding="utf-8")

            both = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--sampler-log",
                    str(sampler_log),
                    "--module-map-log",
                    str(sampler_log),
                    "--output-csv",
                    str(td / "out.csv"),
                    "--output-md",
                    str(td / "out.md"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(both.returncode, 2, both.stderr)
            self.assertIn("exactly one of", both.stderr)

            neither = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--output-csv",
                    str(td / "out.csv"),
                    "--output-md",
                    str(td / "out.md"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(neither.returncode, 2, neither.stderr)

    def test_time_profile_mode_still_requires_a_module_map(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            time_profile = td / "time-profile.xml"
            time_profile.write_text(TIME_PROFILE_XML, encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--output-csv",
                    str(td / "out.csv"),
                    "--output-md",
                    str(td / "out.md"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("--module-map-log is required", result.stderr)


class SymbolicateXctracePeTests(unittest.TestCase):
    def _write_fixtures(self, td: Path, probe_addr: str, contained: str) -> tuple[Path, Path]:
        time_profile = td / "time-profile.xml"
        time_profile.write_text(TIME_PROFILE_XML, encoding="utf-8")
        module_map = td / "module-map.log"
        module_map.write_text(module_map_log(probe_addr, contained), encoding="utf-8")
        return time_profile, module_map

    def test_module_classification_and_no_backtrace_bucketing(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            time_profile, module_map = self._write_fixtures(
                td, probe_addr="0x10000500", contained="1"
            )
            output_csv = td / "out.csv"
            output_md = td / "out.md"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--module-map-log",
                    str(module_map),
                    "--process-regex",
                    r"game\.exe",
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("selected_thread=", result.stdout)
            # Thread A carries the higher total weight (6ms vs 0.5ms), so
            # auto-selection must pick it over thread B.
            self.assertIn("0xaaa", result.stdout.split("selected_thread=", 1)[1].splitlines()[0])

            with output_csv.open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            by_module = {row["module"]: row for row in rows}

            self.assertIn("d3d9.dll", by_module)
            self.assertEqual(int(by_module["d3d9.dll"]["samples"]), 2)  # 0x10000500, 0x10000600
            self.assertIn("unknown_32bit", by_module)
            self.assertEqual(int(by_module["unknown_32bit"]["samples"]), 1)  # 0x20000000
            self.assertIn("unknown_64bit", by_module)
            self.assertEqual(int(by_module["unknown_64bit"]["samples"]), 1)  # 0x123456789
            self.assertIn("host:libdyld.dylib", by_module)
            self.assertEqual(int(by_module["host:libdyld.dylib"]["samples"]), 1)
            self.assertIn("no_backtrace", by_module)
            self.assertEqual(int(by_module["no_backtrace"]["samples"]), 1)

            # 6 samples total on the selected thread (thread A only).
            total_samples = sum(int(row["samples"]) for row in rows)
            self.assertEqual(total_samples, 6)

            md_text = output_md.read_text(encoding="utf-8")
            self.assertIn("PASS", md_text)
            self.assertIn("d3d9.dll", md_text)

    def test_explicit_thread_tid_selection(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            time_profile, module_map = self._write_fixtures(
                td, probe_addr="0x10000500", contained="1"
            )
            output_csv = td / "out.csv"
            output_md = td / "out.md"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--module-map-log",
                    str(module_map),
                    "--process-regex",
                    r"game\.exe",
                    "--thread-tid",
                    "0xbbb",
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with output_csv.open(newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
            # Thread B has exactly one sample.
            total_samples = sum(int(row["samples"]) for row in rows)
            self.assertEqual(total_samples, 1)

    def test_probe_failure_exits_nonzero_and_can_be_overridden(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            # Probe address 0x99999999 is not inside any logged module range.
            time_profile, module_map = self._write_fixtures(
                td, probe_addr="0x99999999", contained="0"
            )
            output_csv = td / "out.csv"
            output_md = td / "out.md"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--module-map-log",
                    str(module_map),
                    "--process-regex",
                    r"game\.exe",
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FAIL", result.stderr + result.stdout + output_md.read_text(encoding="utf-8"))

            result_allowed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--module-map-log",
                    str(module_map),
                    "--process-regex",
                    r"game\.exe",
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                    "--allow-probe-failure",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result_allowed.returncode, 0, result_allowed.stderr)

    def test_deterministic_output_ordering(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            time_profile, module_map = self._write_fixtures(
                td, probe_addr="0x10000500", contained="1"
            )
            output_csv = td / "out.csv"
            output_md = td / "out.md"

            outputs = []
            for _ in range(3):
                result = subprocess.run(
                    [
                        sys.executable,
                        str(SCRIPT),
                        "--time-profile",
                        str(time_profile),
                        "--module-map-log",
                        str(module_map),
                        "--process-regex",
                        r"game\.exe",
                        "--output-csv",
                        str(output_csv),
                        "--output-md",
                        str(output_md),
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                outputs.append(output_csv.read_text(encoding="utf-8"))
            self.assertEqual(outputs[0], outputs[1])
            self.assertEqual(outputs[1], outputs[2])

    def test_no_probe_line_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            time_profile = td / "time-profile.xml"
            time_profile.write_text(TIME_PROFILE_XML, encoding="utf-8")
            module_map = td / "module-map.log"
            module_map.write_text(
                "[dxmt9-pe-module-map] Info: module=d3d9.dll base=0x10000000 size=0x100000\n",
                encoding="utf-8",
            )
            output_csv = td / "out.csv"
            output_md = td / "out.md"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--module-map-log",
                    str(module_map),
                    "--process-regex",
                    r"game\.exe",
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
