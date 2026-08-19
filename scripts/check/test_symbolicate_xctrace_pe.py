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
        [dxmt9-pe-module-map] Info: module=winemetal.dll base=0x10200000 size=0x10000
        [dxmt9-pe-module-map] Info: probe=dxmt9PeModuleMapProbeMarker addr={probe_addr} contained={contained}
        """
    )


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
