#!/usr/bin/env python3

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_xctrace_cpu_threads.py"


class SummarizeXctraceCpuThreadsTests(unittest.TestCase):
    def test_summarizes_keywords_and_thread_states(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            time_sample = root / "time-sample.xml"
            output_csv = root / "cpu.csv"
            output_md = root / "cpu.md"
            output_json = root / "verdict.json"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <sample-time id="t1" fmt="00:00.001">1000</sample-time>
                          <thread id="th1" fmt="3DMark05.exe (0xaaa) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="s1" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="2.00 ms">2000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="OnMainThread">
                                <binary id="b1" name="winemac.so"/>
                              </frame>
                              <frame id="f2" name="macdrv_clip_cursor">
                                <binary ref="b1"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                        <row>
                          <sample-time id="t2" fmt="00:00.002">2000</sample-time>
                          <thread ref="th1"/>
                          <process ref="p1"/>
                          <thread-state ref="s1"/>
                          <weight id="w2" fmt="1.00 ms">1000000</weight>
                          <tagged-backtrace id="bt2">
                            <backtrace>
                              <frame id="f3" name="dxmt9p_device_commit_chunk">
                                <binary id="b2" name="winemetal.so"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                        <row>
                          <sample-time id="t3" fmt="00:00.003">3000</sample-time>
                          <thread id="th2" fmt="Other.exe (0xbbb) (Other.exe, pid: 7)"/>
                          <process id="p2" fmt="Other.exe (7)"/>
                          <thread-state ref="s1"/>
                          <weight id="w3" fmt="9.00 ms">9000000</weight>
                          <tagged-backtrace id="bt3">
                            <backtrace>
                              <frame id="f4" name="OnMainThread">
                                <binary ref="b1"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            time_sample.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <sample-time id="ts1" fmt="00:00.001">1000</sample-time>
                          <thread id="sth1" fmt="3DMark05.exe (0xaaa) (3DMark05.exe, pid: 42)"/>
                          <thread-state id="waiting" fmt="Waiting">Waiting</thread-state>
                        </row>
                        <row>
                          <sample-time id="ts2" fmt="00:00.002">2000</sample-time>
                          <thread ref="sth1"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--time-sample",
                    str(time_sample),
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                    "--output-verdict-json",
                    str(output_json),
                    "--run-label",
                    "unit",
                    "--trace",
                    "unit.trace",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with output_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            row = rows[0]
            self.assertEqual(row["time_profile_rows"], "2")
            self.assertEqual(row["time_profile_weight_ms"], "3.000")
            self.assertIn("OnMainThread=1", row["keyword_hits"])
            self.assertIn("macdrv_clip_cursor=1", row["keyword_hits"])
            self.assertEqual(row["p4_wait_keyword_hits"], "2")
            self.assertEqual(row["p4_holder_keyword_hits"], "0")
            self.assertEqual(row["time_sample_running_rows"], "1")
            self.assertEqual(row["time_sample_blocked_rows"], "1")
            self.assertIn("Waiting=1", row["time_sample_states"])
            self.assertIn("Running=1", row["time_sample_states"])
            md = output_md.read_text(encoding="utf-8")
            self.assertIn("producer-wait-stack-positive", md)
            self.assertIn("OnMainThread", md)
            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["status"], "producer-wait-stack-positive")
            self.assertEqual(verdict["producer_wait_keyword_hits"], "2")
            self.assertEqual(verdict["producer_sample_running_rows"], "1")
            self.assertEqual(verdict["producer_sample_blocked_rows"], "1")
            self.assertEqual(verdict["wait_keyword_thread_count"], "1")
            self.assertEqual(verdict["nonproducer_wait_keyword_hits"], "0")
            self.assertEqual(verdict["holder_status"], "holder-not-sampled")
            self.assertEqual(verdict["holder_keyword_hits"], "0")

    def test_summarizes_main_thread_holder_keywords(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            thread_info = root / "thread-info.xml"
            output_csv = root / "cpu.csv"
            output_md = root / "cpu.md"
            output_json = root / "verdict.json"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="producer" fmt="3DMark05.exe (0xproducer) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="5.00 ms">5000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="d3d9_frame_dispatch">
                                <binary id="b1" name="3DMark05.exe"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                        <row>
                          <thread id="main" fmt="3DMark05.exe (0xmain) (3DMark05.exe, pid: 42)"/>
                          <process ref="p1"/>
                          <thread-state ref="running"/>
                          <weight id="w2" fmt="2.00 ms">2000000</weight>
                          <tagged-backtrace id="bt2">
                            <backtrace>
                              <frame id="f2" name="CA::Transaction::commit">
                                <binary id="b2" name="QuartzCore"/>
                              </frame>
                              <frame id="f3" name="-[CAMetalLayer nextDrawable]">
                                <binary ref="b2"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            thread_info.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="ti1" fmt="3DMark05.exe (0xmain) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <pid id="pid1" fmt="42">42</pid>
                          <tid id="tid1" fmt="0xmain">0xmain</tid>
                          <thread-name id="name1" fmt="Main Thread"/>
                          <boolean id="main1" fmt="Yes">true</boolean>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--thread-info",
                    str(thread_info),
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                    "--output-verdict-json",
                    str(output_json),
                    "--run-label",
                    "holder",
                    "--trace",
                    "holder.trace",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with output_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 2)
            main_row = next(row for row in rows if "0xmain" in row["thread"])
            self.assertEqual(main_row["is_main_thread"], "Yes")
            self.assertEqual(main_row["p4_holder_keyword_hits"], "3")
            self.assertIn("CA::Transaction=1", main_row["keyword_hits"])
            self.assertIn("CAMetalLayer=1", main_row["keyword_hits"])
            self.assertIn("nextDrawable=1", main_row["keyword_hits"])

            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["holder_status"], "main-thread-holder-positive")
            self.assertEqual(verdict["holder_keyword_thread_count"], "1")
            self.assertEqual(verdict["holder_keyword_hits"], "3")
            self.assertEqual(verdict["main_thread_holder_keyword_thread_count"], "1")
            self.assertEqual(verdict["main_thread_holder_keyword_hits"], "3")
            self.assertEqual(verdict["producer_holder_keyword_hits"], "0")
            self.assertEqual(verdict["nonproducer_holder_keyword_hits"], "3")

            md = output_md.read_text(encoding="utf-8")
            self.assertIn("Holder status | `main-thread-holder-positive`", md)
            self.assertIn("Main-thread holder keyword hits | `3`", md)

            stdout_result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--thread-info",
                    str(thread_info),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(stdout_result.returncode, 0, stdout_result.stderr)
            main_line = next(line for line in stdout_result.stdout.splitlines() if "0xmain" in line)
            self.assertIn("p4_holder=3", main_line)
            self.assertIn("running=0", main_line)
            self.assertIn("blocked=0", main_line)

    def test_producer_thread_regex_overrides_highest_weight_thread(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            time_sample = root / "time-sample.xml"
            auto_json = root / "auto.json"
            selected_json = root / "selected.json"
            selected_md = root / "selected.md"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="th1" fmt="3DMark05.exe (0xrunner) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="10.00 ms">10000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="d3d9_frame_dispatch">
                                <binary id="b1" name="3DMark05.exe"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                        <row>
                          <thread id="th2" fmt="3DMark05.exe (0xwaiter) (3DMark05.exe, pid: 42)"/>
                          <process ref="p1"/>
                          <thread-state ref="running"/>
                          <weight id="w2" fmt="1.00 ms">1000000</weight>
                          <tagged-backtrace id="bt2">
                            <backtrace>
                              <frame id="f2" name="OnMainThread">
                                <binary id="b2" name="winemac.so"/>
                              </frame>
                              <frame id="f3" name="dispatch_semaphore_wait">
                                <binary id="b3" name="libdispatch.dylib"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            time_sample.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="sth1" fmt="3DMark05.exe (0xrunner) (3DMark05.exe, pid: 42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                        </row>
                        <row>
                          <thread ref="sth1"/>
                          <thread-state ref="running"/>
                        </row>
                        <row>
                          <thread id="sth2" fmt="3DMark05.exe (0xwaiter) (3DMark05.exe, pid: 42)"/>
                          <thread-state id="waiting" fmt="Waiting">Waiting</thread-state>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )

            auto = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--time-sample",
                    str(time_sample),
                    "--output-verdict-json",
                    str(auto_json),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(auto.returncode, 0, auto.stderr)
            auto_verdict = json.loads(auto_json.read_text(encoding="utf-8"))
            self.assertEqual(auto_verdict["status"], "producer-running-negative-scout")
            self.assertEqual(auto_verdict["producer_selection"], "auto-highest-weight")
            self.assertIn("0xrunner", auto_verdict["producer_thread"])

            selected = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--time-sample",
                    str(time_sample),
                    "--output-md",
                    str(selected_md),
                    "--output-verdict-json",
                    str(selected_json),
                    "--producer-thread-regex",
                    "0xwaiter",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(selected.returncode, 0, selected.stderr)
            selected_verdict = json.loads(selected_json.read_text(encoding="utf-8"))
            self.assertEqual(selected_verdict["status"], "producer-wait-stack-positive")
            self.assertEqual(selected_verdict["producer_selection"], "0xwaiter")
            self.assertEqual(selected_verdict["producer_selection_source"], "explicit-regex")
            self.assertIn("0xwaiter", selected_verdict["producer_thread"])
            self.assertIn("Producer selector: `0xwaiter`", selected_md.read_text(encoding="utf-8"))

    def test_producer_thread_regex_not_found_is_inconclusive_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            output_json = root / "verdict.json"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="th1" fmt="3DMark05.exe (0xaaa) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="1.00 ms">1000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="d3d9_frame_dispatch">
                                <binary id="b1" name="3DMark05.exe"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--output-verdict-json",
                    str(output_json),
                    "--producer-thread-regex",
                    "0xmissing",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["status"], "producer-thread-not-found")
            self.assertEqual(verdict["producer_selection"], "0xmissing")
            self.assertEqual(verdict["wait_keyword_thread_count"], "0")

    def test_producer_thread_regex_from_pe_log_prefers_clear_return(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            pe_log = root / "wrapper.log"
            output_json = root / "verdict.json"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="th1" fmt="3DMark05.exe (0xaaa) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="10.00 ms">10000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="d3d9_frame_dispatch">
                                <binary id="b1" name="3DMark05.exe"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                        <row>
                          <thread id="th2" fmt="3DMark05.exe (0xc1ea) (3DMark05.exe, pid: 42)"/>
                          <process ref="p1"/>
                          <thread-state ref="running"/>
                          <weight id="w2" fmt="1.00 ms">1000000</weight>
                          <tagged-backtrace id="bt2">
                            <backtrace>
                              <frame id="f2" name="OnMainThread">
                                <binary id="b2" name="winemac.so"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            pe_log.write_text(
                "\n".join(
                    [
                        "[dxmt9-device] pe_present_timing device=0x1 thread_id=0x111 hr=0x00000000",
                        "[dxmt9-device] pe_present_call_return device=0x1 ordinal=1 milestone=4 call=SetRenderTarget thread_id=0x222 hr=0x00000000",
                        "[dxmt9-device] pe_present_call_return device=0x1 ordinal=1 milestone=5 call=Clear thread_id=0xc1ea hr=0x00000000",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--output-verdict-json",
                    str(output_json),
                    "--producer-thread-regex-from-pe-log",
                    str(pe_log),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["status"], "producer-wait-stack-positive")
            self.assertEqual(verdict["producer_selection"], "0xc1ea")
            self.assertEqual(verdict["producer_selection_source"], "pe-log-clear-return")
            self.assertIn("0xc1ea", verdict["producer_thread"])

    def test_producer_thread_regex_from_log_prefers_native_commit_thread(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            direct_log = root / "direct.log"
            output_json = root / "verdict.json"
            output_md = root / "summary.md"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="th1" fmt="3DMark05.exe (0x5be7f0) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="1.00 ms">1000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="OnMainThread">
                                <binary id="b1" name="winemac.so"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            direct_log.write_text(
                "\n".join(
                    [
                        "[dxmt9-device] pe_present_call_return call=Clear thread_id=0xd0 hr=0x00000000",
                        "[dxmt9-device] unix_commit_chunk_entry device=0x1 native_tid=0x0 recordCount=1 recordBytes=32 handleCount=0",
                        "[dxmt9-device] unix_commit_chunk_entry device=0x1 native_tid=0x5be7f0 pthread_self=0xabcd recordCount=9 recordBytes=640 handleCount=3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--output-md",
                    str(output_md),
                    "--output-verdict-json",
                    str(output_json),
                    "--producer-thread-regex-from-pe-log",
                    str(direct_log),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["status"], "producer-wait-stack-positive")
            self.assertEqual(verdict["producer_selection"], "0x5be7f0")
            self.assertEqual(
                verdict["producer_selection_source"], "native-log-commit-chunk-entry"
            )
            self.assertIn("0x5be7f0", verdict["producer_thread"])
            md = output_md.read_text(encoding="utf-8")
            self.assertIn("unix_commit_chunk_entry", md)

    def test_pe_log_selector_not_found_reports_thread_id_domain_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            pe_log = root / "direct.log"
            output_json = root / "verdict.json"
            output_md = root / "summary.md"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="th1" fmt="3DMark05.exe (0xaaa) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="10.00 ms">10000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="d3d9_frame_dispatch">
                                <binary id="b1" name="3DMark05.exe"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            pe_log.write_text(
                "[dxmt9-device] pe_present_call_return call=Clear thread_id=0xd0 hr=0x00000000\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--output-md",
                    str(output_md),
                    "--output-verdict-json",
                    str(output_json),
                    "--producer-thread-regex-from-pe-log",
                    str(pe_log),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["status"], "producer-thread-not-found")
            self.assertEqual(verdict["producer_selection"], "0xd0")
            self.assertEqual(verdict["producer_selection_source"], "pe-log-clear-return")
            self.assertIn("Win32 thread id", verdict["reason"])
            self.assertIn("Mach/pthread id", verdict["reason"])
            md = output_md.read_text(encoding="utf-8")
            self.assertIn("Win32 thread-id namespace", md)

    def test_producer_thread_regex_from_pe_log_missing_id_does_not_auto_select(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            pe_log = root / "wrapper.log"
            output_json = root / "verdict.json"
            output_md = root / "summary.md"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="th1" fmt="3DMark05.exe (0xaaa) (3DMark05.exe, pid: 42)"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="10.00 ms">10000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="OnMainThread">
                                <binary id="b1" name="winemac.so"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            pe_log.write_text(
                "[dxmt9-device] pe_present_call_return call=Clear hr=0x00000000\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--output-md",
                    str(output_md),
                    "--output-verdict-json",
                    str(output_json),
                    "--producer-thread-regex-from-pe-log",
                    str(pe_log),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["status"], "producer-thread-selector-missing")
            self.assertEqual(verdict["producer_selection"], "")
            self.assertEqual(verdict["producer_selection_source"], "pe-log-no-thread-id")
            self.assertEqual(verdict["producer_selection_required"], "1")
            self.assertEqual(verdict["producer_thread"], "")
            self.assertEqual(verdict["wait_keyword_thread_count"], "1")
            self.assertEqual(verdict["nonproducer_wait_keyword_hits"], "1")
            md = output_md.read_text(encoding="utf-8")
            self.assertIn("producer-thread-selector-missing", md)
            self.assertIn("did not contain a usable", md)

    def test_thread_info_tid_can_select_producer(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            time_profile = root / "time-profile.xml"
            thread_info = root / "thread-info.xml"
            output_csv = root / "summary.csv"
            output_json = root / "verdict.json"

            time_profile.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <thread id="th1" fmt="Render Worker"/>
                          <process id="p1" fmt="3DMark05.exe (42)"/>
                          <thread-state id="running" fmt="Running">Running</thread-state>
                          <weight id="w1" fmt="1.00 ms">1000000</weight>
                          <tagged-backtrace id="bt1">
                            <backtrace>
                              <frame id="f1" name="OnMainThread">
                                <binary id="b1" name="winemac.so"/>
                              </frame>
                            </backtrace>
                          </tagged-backtrace>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )
            thread_info.write_text(
                textwrap.dedent(
                    """\
                    <trace-query-result>
                      <node>
                        <row>
                          <pid id="pid1" fmt="42">42</pid>
                          <tid id="tid1" fmt="0xfeed">65261</tid>
                          <process id="ip1" fmt="3DMark05.exe (42)"/>
                          <thread id="ith1" fmt="Render Worker"/>
                          <thread-name id="tn1" fmt="Render Worker  0xfeed">Render Worker  0xfeed</thread-name>
                          <boolean id="main" fmt="No">0</boolean>
                        </row>
                      </node>
                    </trace-query-result>
                    """
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--time-profile",
                    str(time_profile),
                    "--thread-info",
                    str(thread_info),
                    "--output-csv",
                    str(output_csv),
                    "--output-verdict-json",
                    str(output_json),
                    "--producer-thread-regex",
                    "0xfeed",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with output_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["tid"], "0xfeed")
            self.assertEqual(rows[0]["is_main_thread"], "No")
            verdict = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual(verdict["status"], "producer-wait-stack-positive")
            self.assertEqual(verdict["producer_tid"], "0xfeed")
            self.assertEqual(verdict["producer_is_main_thread"], "No")


if __name__ == "__main__":
    unittest.main()
