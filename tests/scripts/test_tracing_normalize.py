import importlib.util
import hashlib
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "tracing_normalize", ROOT / "scripts/tools/tracing_normalize.py")
tracing = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tracing)


def complete_bundle():
    return {
        "schema": tracing.SCHEMA,
        "producer": "fixture",
        "run_id": "run-1",
        "inputs": [{"path": "fixture.raw", "sha256": hashlib.sha256(b"fixture").hexdigest()}],
        "processes": [{"process_id": "p1", "pid": "99", "incarnation": "start-1",
                       "identity_quality": "qualified"}],
        "threads": [{"thread_id": "t1", "process_id": "p1", "native_tid": "7",
                      "incarnation": "thread-1", "identity_quality": "qualified"}],
        "clocks": [{"clock_id": "mono", "unit": "ns", "kind": "monotonic",
                    "alignment": "qualified", "api": "fixture clock", "source": "fixture",
                    "domain": "fixture-local", "resolution_ns": "1", "uncertainty_ns": "0"}],
        "events": [
            {"event_id": "b", "source_record": "r1", "process_id": "p1", "thread_id": "t1",
             "clock_id": "mono", "timestamp": "9007199254741000", "kind": "scope_begin",
             "source_order": "1", "collector": "fixture",
             "scope_id": "scope-1", "attrs": {}},
            {"event_id": "e", "source_record": "r2", "process_id": "p1", "thread_id": "t1",
             "clock_id": "mono", "timestamp": "9007199254741123", "kind": "scope_end",
             "source_order": "2", "collector": "fixture",
             "scope_id": "scope-1", "attrs": {}}],
        "relations": [], "relation_requirements": [],
        "relation_diagnostics": {
            "unmatched_replay_publish": "0", "ambiguous_replay_publish": "0",
            "unmatched_queue_publish": "0", "ambiguous_queue_publish": "0",
            "unmatched_encode_begin": "0", "unmatched_submit_begin": "0",
            "ambiguous_submit_begin": "0", "unmatched_gpu_command_buffer": "0",
            "ambiguous_gpu_command_buffer": "0", "duplicate_source_records": "0"},
        "window": {"start": "9007199254741000", "end": "9007199254741124",
                                     "clock_id": "mono",
                                     "closed": True},
        "loss": {"total": "0"}, "coverage": {"events": {"observed": "2", "population": "2"}},
        "qualification_policy": {"max_clock_error_ns": "0"}}


class TracingNormalizeTests(unittest.TestCase):
    def calibrated_bundle(self):
        bundle = complete_bundle()
        start = int(bundle["events"][0]["timestamp"])
        bundle["events"][0]["attrs"].update({"source_timestamp": "200",
            "source_clock_id": "gpu-clock", "calibration_id": "1",
            "source_timestamp_unit": "ticks", "uncertainty_ns": "0"})
        bundle["clock_calibrations"] = [{"kind": "gpu", "mapping_kind": "two_anchor",
            "calibration_id": "1", "source_order": "1", "source_clock_id": "gpu-clock",
            "source_ticks": "200", "source_before": "100", "source_after": "300",
            "host_ns": str(start), "host_clock_id": "mono",
            "host_before_ns": str(start-100), "host_after_ns": str(start+100),
            "scale": "1", "offset_ns": str(start-200), "valid_begin": "100",
            "valid_end": "300", "error_ns": "0"}]
        return bundle

    def test_preserves_64_bit_strings_and_qualifies_scopes(self):
        bundle = complete_bundle()
        verdict = tracing.validate_bundle(bundle, require_complete=True)
        self.assertEqual(verdict["verdict"], "qualified")
        self.assertEqual(bundle["events"][0]["timestamp"], "9007199254741000")

    def test_complete_window_requires_clock_and_contains_every_event(self):
        bundle = complete_bundle()
        del bundle["window"]["clock_id"]
        self.assertEqual(tracing.validate_bundle(bundle)["verdict"], "unqualified")
        with self.assertRaisesRegex(tracing.TraceError, "capture_window_bounds_or_clock_missing"):
            tracing.validate_bundle(bundle, require_complete=True)
        bundle = complete_bundle()
        bundle["events"][0]["timestamp"] = "9007199254740999"
        with self.assertRaisesRegex(tracing.TraceError, "outside declared capture window"):
            tracing.validate_bundle(bundle)

    def test_zero_population_optional_source_is_complete_when_no_records_exist(self):
        bundle = complete_bundle()
        bundle["coverage"]["pe"] = {"observed": "0", "population": "0"}
        self.assertEqual(tracing.validate_bundle(bundle, require_complete=True)["verdict"],
                         "qualified")

    def test_complete_capture_fails_for_unmatched_encode_or_duplicate_relation_source(self):
        bundle = complete_bundle()
        bundle["relation_requirements"] = ["queue_encode"]
        bundle["relation_diagnostics"]["unmatched_encode_begin"] = "1"
        with self.assertRaisesRegex(tracing.TraceError,
                                    "relation_diagnostic_nonzero:unmatched_encode_begin"):
            tracing.validate_bundle(bundle, require_complete=True)

        bundle = complete_bundle()
        bundle["relation_requirements"] = ["queue_encode"]
        bundle["relation_diagnostics"]["duplicate_source_records"] = "1"
        with self.assertRaisesRegex(tracing.TraceError,
                                    "relation_diagnostic_nonzero:duplicate_source_records"):
            tracing.validate_bundle(bundle, require_complete=True)

    def test_cpu_only_capture_does_not_require_missing_gpu_relation(self):
        bundle = complete_bundle()
        bundle["relation_requirements"] = ["queue_encode"]
        bundle["relation_diagnostics"]["unmatched_submit_begin"] = "1"
        bundle["relation_diagnostics"]["unmatched_gpu_command_buffer"] = "1"
        self.assertEqual(tracing.validate_bundle(bundle, require_complete=True)["verdict"],
                         "qualified")

    def test_complete_capture_requires_relation_applicability_and_counters(self):
        bundle = complete_bundle()
        del bundle["relation_requirements"]
        with self.assertRaisesRegex(tracing.TraceError, "relation_requirements_missing"):
            tracing.validate_bundle(bundle, require_complete=True)

    def test_gpu_source_clock_requires_valid_calibration_anchors_range_and_error(self):
        bundle = self.calibrated_bundle()
        self.assertEqual(tracing.validate_bundle(bundle, require_complete=True)["verdict"],
                         "qualified")

        missing = self.calibrated_bundle()
        missing["clock_calibrations"] = []
        with self.assertRaisesRegex(tracing.TraceError, "event_calibration_missing"):
            tracing.validate_bundle(missing, require_complete=True)

        malformed = self.calibrated_bundle()
        malformed["clock_calibrations"][0]["scale"] = "-1"
        with self.assertRaisesRegex(tracing.TraceError, "scale must be positive"):
            tracing.validate_bundle(malformed, require_complete=True)

        outside = self.calibrated_bundle()
        outside["events"][0]["attrs"]["source_timestamp"] = "301"
        with self.assertRaisesRegex(tracing.TraceError,
                                    "event_calibration_out_of_range"):
            tracing.validate_bundle(outside, require_complete=True)

        high_error = self.calibrated_bundle()
        high_error["events"][0]["attrs"]["uncertainty_ns"] = "1001"
        high_error["qualification_policy"]["max_clock_error_ns"] = "1000"
        with self.assertRaisesRegex(tracing.TraceError,
                                    "event_uncertainty_exceeds_policy"):
            tracing.validate_bundle(high_error, require_complete=True)

        high_calibration_error = self.calibrated_bundle()
        high_calibration_error["clock_calibrations"][0]["error_ns"] = "1001"
        with self.assertRaisesRegex(tracing.TraceError,
                                    "calibration_error_exceeds_policy"):
            tracing.validate_bundle(high_calibration_error, require_complete=True)
        bundle = complete_bundle()
        bundle["relation_requirements"] = ["submit_gpu"]
        del bundle["relation_diagnostics"]["unmatched_gpu_command_buffer"]
        with self.assertRaisesRegex(tracing.TraceError,
                                    "relation_diagnostic_missing:unmatched_gpu_command_buffer"):
            tracing.validate_bundle(bundle, require_complete=True)

    def test_gpu_process_track_has_stable_identity_without_fabricated_tid(self):
        bundle = complete_bundle()
        gpu_track = {"thread_id": "gpu-track-1", "track_id": "gpu-track-1",
                     "track_kind": "gpu", "process_id": "p1", "native_tid": None,
                     "incarnation": "gpu-start-1", "identity_quality": "qualified"}
        bundle["threads"].append(gpu_track)
        bundle["events"].append({"event_id": "gpu-event", "source_record": "r3",
            "source_order": "3", "collector": "fixture", "process_id": "p1",
            "thread_id": "gpu-track-1", "clock_id": "mono", "timestamp": "9007199254741120",
            "kind": "instant", "track_kind": "gpu", "track_id": "gpu-track-1",
            "attrs": {"name": "gpu_event"}})
        bundle["coverage"]["events"] = {"observed": "3", "population": "3"}
        self.assertEqual(tracing.validate_bundle(bundle, require_complete=True)["verdict"],
                         "qualified")

    def test_rejects_numeric_timestamp_that_could_be_rounded_by_json_consumers(self):
        bundle = complete_bundle()
        bundle["events"][0]["timestamp"] = 9007199254741000
        with self.assertRaisesRegex(tracing.TraceError, "decimal string"):
            tracing.validate_bundle(bundle)

    def test_rejects_duplicate_event_ids_and_scope_clock_mismatch(self):
        bundle = complete_bundle()
        bundle["events"][1]["event_id"] = "b"
        with self.assertRaisesRegex(tracing.TraceError, "duplicate event_id"):
            tracing.validate_bundle(bundle)
        bundle = complete_bundle()
        bundle["clocks"].append({"clock_id": "other", "unit": "ns", "kind": "monotonic",
                                 "alignment": "qualified", "api": "other", "source": "other",
                                 "domain": "other", "resolution_ns": "1", "uncertainty_ns": "0"})
        bundle["events"][1]["clock_id"] = "other"
        with self.assertRaisesRegex(tracing.TraceError, "invalid scope endpoints"):
            tracing.validate_bundle(bundle)

    def test_unqualified_clock_identity_loss_and_coverage_do_not_pass(self):
        bundle = complete_bundle()
        bundle["loss"]["total"] = "1"
        bundle["coverage"]["events"]["observed"] = "1"
        with self.assertRaisesRegex(tracing.TraceError, "event_loss"):
            tracing.validate_bundle(bundle, require_complete=True)
        bundle = complete_bundle()
        bundle["threads"][0]["incarnation"] = "unknown"
        with self.assertRaisesRegex(tracing.TraceError, "identity_incarnation_unqualified"):
            tracing.validate_bundle(bundle, require_complete=True)
        bundle = complete_bundle()
        bundle["clocks"][0]["alignment"] = "unqualified"
        with self.assertRaisesRegex(tracing.TraceError, "clock_mapping_unqualified"):
            tracing.validate_bundle(bundle, require_complete=True)

    def test_scope_timestamp_order_is_numeric_and_rational_clock_mapping_is_bounded(self):
        bundle = complete_bundle()
        bundle["events"][0]["timestamp"] = "9"
        bundle["events"][1]["timestamp"] = "10"
        bundle["window"] = {"start": "9", "end": "11", "clock_id": "mono", "closed": True}
        self.assertEqual(tracing.validate_bundle(bundle, require_complete=True)["verdict"],
                         "qualified")
        bundle = complete_bundle()
        host_clock = bundle["clocks"][0]
        bundle["window"]["clock_id"] = "mono"
        source_clock = {"clock_id": "ticks", "unit": "ticks", "kind": "monotonic",
                        "api": "fixture ticks", "source": "fixture counter",
                        "domain": "source-clock", "resolution_ns": "unknown", "uncertainty_ns": "0",
                        "alignment": "qualified",
                        "mapping": {"host_clock_id": "mono", "scale_num": "2",
                                    "scale_den": "1", "offset_ns": "1",
                                    "valid_from": "0", "valid_to": "100", "error_ns": "0",
                                    "calibration": [{"source_ticks": "0", "host_ns": "1"},
                                                    {"source_ticks": "10", "host_ns": "21"}]}}
        bundle["clocks"].append(source_clock)
        bundle["events"][0]["clock_id"] = "ticks"
        self.assertEqual(tracing.map_clock_timestamp(source_clock, "10", 0), (21, 0))
        with self.assertRaisesRegex(tracing.TraceError, "outside clock calibration"):
            tracing.map_clock_timestamp(source_clock, "101", 0)

    def test_runtime_jsonl_preserves_exact_timestamp_and_scope(self):
        raw_process = dict(complete_bundle()["processes"][0], pid=99)
        raw_thread = dict(complete_bundle()["threads"][0], native_tid=7)
        gpu_thread = {"thread_id": "gpu-track", "track_id": "gpu-track", "track_kind": "gpu",
                      "process_id": "p1", "native_tid": None, "incarnation": "gpu-1",
                      "identity_quality": "qualified"}
        raw_clock = dict(complete_bundle()["clocks"][0], resolution_ns=1)
        metadata = {"schema": "dxmt9.trace.raw.v1", "run_id": "run-1",
                    "processes": [raw_process], "threads": [raw_thread, gpu_thread],
                    "clocks": [raw_clock],
                    "window": {"start_ns": "9007199254741000", "end_ns": "9007199254741124",
                               "clock_id": "mono",
                               "closed": True},
                    "loss": {"total": "0"},
                    "relation_requirements": ["replay_queue", "queue_encode", "submit_gpu"],
                    "relation_diagnostics": {
                        "unmatched_replay_publish": "0", "ambiguous_replay_publish": "0",
                        "unmatched_queue_publish": "0", "ambiguous_queue_publish": "0",
                        "unmatched_encode_begin": "0", "unmatched_submit_begin": "0",
                        "ambiguous_submit_begin": "0", "unmatched_gpu_command_buffer": "0",
                        "ambiguous_gpu_command_buffer": "0", "duplicate_source_records": "0"},
                    "relations": [
                        {"relation_id": 1, "source_event": "1", "target_event": "2",
                         "type": "consumes", "evidence": "observed_binding", "flow": True,
                         "source_id": 44, "target_id": 55},
                        {"relation_id": 2, "source_event": "1", "target_event": "2",
                         "evidence_event": "3", "type": "publishes",
                         "evidence": "observed_binding", "flow": True},
                        {"relation_id": 3, "source_event": "2", "target_event": "3",
                         "type": "submitted_in", "evidence": "observed_binding", "flow": True}],
                    "coverage": {"events": {"observed": "3", "population": "3"}},
                    "qualification_policy": {"max_clock_error_ns": "0"}}
        # Window conversion belongs to the adapter and is tested as the metadata contract evolves.
        metadata["window"] = {"start": "9007199254741000", "end": "9007199254741124",
                              "clock_id": "mono", "closed": True}
        records = [
            {"event_id": "b", "source_record": "1", "process_id": "p1", "thread_id": "t1",
             "clock_id": "mono", "timestamp_ns": "9007199254741000", "phase": "begin", "source_order": "1",
             "event_type": 12, "arg0": "44", "arg1": "66", "work_ids": ["55"],
             "scope_id": "scope-1", "fields": {"binding_kind": "raw_publication"}},
            {"event_id": "e", "source_record": "2", "process_id": "p1", "thread_id": "t1",
             "clock_id": "mono", "timestamp_ns": "9007199254741123", "phase": "end", "source_order": "2",
             "scope_id": "scope-1", "fields": {}},
            {"event_id": "g", "source_record": "3", "process_id": "p1", "thread_id": "gpu-track",
             "track_id": "gpu-track", "track_kind": "gpu", "clock_id": "mono",
             "timestamp_ns": "9007199254741100", "phase": "instant", "source_order": "3",
             "name": "gpu_event", "fields": {}}]
        text = "\n".join(json.dumps(row) for row in [metadata, *records])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "capture.raw.jsonl"
            path.write_text(text, encoding="utf-8")
            bundle = tracing.adapt(path, "auto", run_id="ignored", process_incarnation="unknown")
            self.assertEqual(bundle["inputs"][0]["sha256"], tracing._digest(path))
        self.assertEqual(bundle["events"][1]["timestamp"], "9007199254741123")
        self.assertEqual(bundle["events"][0]["attrs"]["owner_id"], "44")
        self.assertEqual(bundle["events"][0]["attrs"]["work_id"], "55")
        self.assertEqual(bundle["events"][0]["attrs"]["binding_kind"], "raw_publication")
        self.assertNotIn("relation_type", bundle["events"][0]["attrs"])
        self.assertEqual(bundle["processes"][0]["pid"], "99")
        self.assertEqual(bundle["threads"][0]["native_tid"], "7")
        self.assertEqual(bundle["clocks"][0]["resolution_ns"], "1")
        self.assertIsNone(bundle["threads"][1]["native_tid"])
        self.assertEqual(bundle["events"][2]["track_kind"], "gpu")
        self.assertEqual(bundle["relations"], [
            {"relation_id": "1", "source_event": "b", "target_event": "e",
             "type": "consumes", "evidence": "observed_binding", "flow": True,
             "source_id": "44", "target_id": "55"},
            {"relation_id": "2", "source_event": "b", "target_event": "e",
             "evidence_event": "g", "type": "publishes",
             "evidence": "observed_binding", "flow": True},
            {"relation_id": "3", "source_event": "e", "target_event": "g",
             "type": "submitted_in", "evidence": "observed_binding", "flow": True}])
        self.assertEqual(bundle["relation_requirements"], ["replay_queue", "queue_encode", "submit_gpu"])
        self.assertEqual(bundle["relation_diagnostics"]["unmatched_encode_begin"], "0")
        self.assertEqual(tracing.validate_bundle(bundle, require_complete=True)["verdict"], "qualified")

    def test_runtime_relations_with_unknown_or_ambiguous_endpoint_fail_closed(self):
        metadata = {"schema": "dxmt9.trace.raw.v1", "run_id": "r",
                    "relations": [{"relation_id": "r1", "source_event": "missing",
                                   "target_event": "2", "type": "consumes",
                                   "evidence": "observed_binding"}]}
        # The common fixture setup exercises the adapter's event-reference table.
        process = {"process_id": "p", "pid": 1, "incarnation": "p"}
        thread = {"thread_id": "t", "process_id": "p", "native_tid": 1,
                  "incarnation": "t"}
        clock = {"clock_id": "c", "unit": "ns", "kind": "monotonic",
                 "api": "test", "source": "test", "domain": "test",
                 "resolution_ns": 1, "uncertainty_ns": 0, "alignment": "qualified"}
        metadata.update({"processes": [process], "threads": [thread], "clocks": [clock],
                         "window": {"start": "1", "end": "3", "clock_id": "c",
                                    "closed": True}, "loss": {"total": "0"},
                         "coverage": {"events": {"observed": "2", "population": "2"}},
                         "qualification_policy": {"max_clock_error_ns": "0"}})
        records = [{"event_id": str(i), "source_record": str(i), "process_id": "p",
                    "thread_id": "t", "clock_id": "c", "timestamp_ns": str(i),
                    "phase": "instant", "source_order": str(i)} for i in (1, 2)]
        with self.assertRaisesRegex(tracing.TraceError, "unknown or ambiguous source_event"):
            tracing.normalize_bundle({"schema": "dxmt9.trace.raw.v1", **metadata,
                                       "events": records})

        metadata["relations"][0]["source_event"] = "1"
        records[1]["source_record"] = "1"
        with self.assertRaisesRegex(tracing.TraceError, "unknown or ambiguous source_event"):
            tracing.normalize_bundle({"schema": "dxmt9.trace.raw.v1", **metadata,
                                       "events": records})

    def test_runtime_jsonl_preserves_guest_identity_and_unaligned_pe_clock(self):
        metadata = {
            "schema": "dxmt9.trace.raw.v1", "run_id": "guest-run",
            "processes": [
                {"process_id": "500", "pid": 500, "incarnation": "host-start",
                 "identity_quality": "qualified"},
                {"process_id": "9223372036854775809", "pid": 1234,
                 "incarnation": "guest-start", "identity_quality": "qualified",
                 "pid_namespace": "guest", "guest_process_id": 1234,
                 "creation_time100ns": 9007199254741011},
            ],
            "threads": [
                {"thread_id": "700", "process_id": "500", "native_tid": 700,
                 "incarnation": "host-thread", "identity_quality": "qualified"},
                {"thread_id": "9223372036854775810", "track_id": "9223372036854775810",
                 "process_id": "9223372036854775809", "native_tid": None,
                 "guest_tid": 1235, "incarnation": "guest-thread",
                 "identity_quality": "qualified", "tid_namespace": "guest"},
            ],
            "clocks": [
                {"clock_id": "3", "unit": "ns", "kind": "monotonic",
                 "api": "mach", "source": "mach", "domain": "host",
                 "resolution_ns": 1, "uncertainty_ns": 1, "alignment": "qualified"},
                {"clock_id": "4", "unit": "ticks", "kind": "external_counter",
                 "api": "qpc", "source": "qpc", "domain": "guest-qpc",
                 "resolution_ns": "unknown", "uncertainty_ns": 10,
                 "alignment": "unqualified", "frequency": 10000000},
            ],
            "window": {"start_ns": "1000", "end_ns": "2000", "clock_id": "3",
                       "closed": True},
            "loss": {"total": "0"},
            "coverage": {"pe": {"observed": "1", "population": "1"}},
            "qualification_policy": {"max_clock_error_ns": "1"},
        }
        event = {
            "event_id": "1", "collector": "engine-perfetto", "source_record": 1,
            "source_order": 1, "process_id": "500", "thread_id": "700",
            "native_tid": 700, "guest_process_id": 1234, "guest_thread_id": 1235,
            "process_create_time100ns": 9007199254741011,
            "thread_create_time100ns": 9007199254741012,
            "clock_id": 4, "timestamp_ns": 12345678901234567,
            "source_timestamp": 12345678901234567, "source_clock_id": 4,
            "calibration_id": 1,
            "native_observation": {"source_order": "1", "host_thread_id": "700"},
            "phase": "instant", "event_type": 82, "fields": {},
        }
        bundle = tracing.normalize_bundle(dict(metadata, events=[event]))
        self.assertEqual(bundle["processes"][1]["pid"], "1234")
        self.assertEqual(bundle["processes"][1]["guest_process_id"], "1234")
        self.assertEqual(bundle["threads"][1]["guest_tid"], "1235")
        normalized = bundle["events"][0]
        self.assertEqual(normalized["clock_id"], "4")
        self.assertEqual(normalized["timestamp"], "12345678901234567")
        self.assertEqual(normalized["attrs"]["guest_process_id"], "1234")
        self.assertEqual(normalized["attrs"]["guest_thread_id"], "1235")
        self.assertEqual(normalized["attrs"]["source_timestamp"], "12345678901234567")
        self.assertEqual(normalized["attrs"]["native_observation"]["host_thread_id"], "700")
        self.assertEqual(bundle["qualification"]["verdict"], "unqualified")
        self.assertIn("event_clock_not_aligned_to_window", bundle["qualification"]["reasons"])

    def test_cpu_supply_adapter_keeps_cpu_counter_separate(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "cpu.csv"
            path.write_text("# cpu-supply-v2 first=1 last=3 closed=1 overflow=0 incomplete=0 cpu_clock=1 pid=44\n"
                            "wall_ns,cpu_ns,tid,owner,id,value,frame,event\n"
                            "100,200,7,1,5,0,2,6\n"
                            "150,225,7,1,5,0,2,7\n", encoding="utf-8")
            bundle = tracing.adapt_cpu_supply(path.read_text(), path, run_id="run")
        self.assertEqual(bundle["events"][0]["kind"], "scope_begin")
        self.assertEqual(bundle["events"][0]["attrs"]["thread_cpu_counter_ns"], "200")
        self.assertEqual(bundle["clocks"][0]["source"], "CLOCK_MONOTONIC")
        self.assertEqual(bundle["qualification"]["verdict"], "unqualified")

    def test_wait_adapter_retains_elapsed_milestones_and_not_cpu(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "wait.csv"
            fields = "pid=55 first=1 last=5 closed=1 overflow=0 incomplete=0 unknown=0 owners=1 capacity=32768 mutex_min_ns=50000 storage=1 cv_entries=1"
            path.write_text(f"# wait-trace-v1 {fields}\n"
                            "site,owner,target,query,queue,sequence,initializer,tid,first_frame,last_frame,begin_ns,acquired_ns,end_ns,waits,raw_before,queued_before,completed_before,state_before,raw_after,queued_after,completed_after,state_after,requested_queue\n"
                            "2,1,4,0,0,0,0,7,2,4,10,20,40,1,5,3,2,0,5,3,3,1,0\n"
                            "# end rows=1\n", encoding="utf-8")
            bundle = tracing.adapt_wait_trace(path.read_text(), path, run_id="run")
        self.assertEqual([e["kind"] for e in bundle["events"]],
                         ["scope_begin", "scope_end", "instant"])
        self.assertEqual(bundle["events"][2]["attrs"]["elapsed_ns"], "10")
        self.assertIn("not_cpu", bundle["events"][0]["attrs"]["interpretation"])

    def test_pe_snapshot_adapter_does_not_create_elapsed_slices(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "pe.log"
            path.write_text("[dxmt9-perf-frame frame=10]\n"
                            "[dxmt9-pe-sampler] presents=10 samples=12\n"
                            "[dxmt9-pe-sampler] module=xrEngine.exe samples=8\n"
                            "[dxmt9-pe-sampler] selfpc_module=d3d9.dll samples=2\n"
                            "[dxmt9-pe-sampler] selfpc_summary mode=full rows=0 used=0\n",
                            encoding="utf-8")
            bundle = tracing.adapt_pe_sampler(path.read_text(), path, run_id="run")
        self.assertEqual(bundle["events"][0]["kind"], "counter")
        self.assertEqual(bundle["events"][0]["attrs"]["sample_population"], "12")
        self.assertEqual(bundle["events"][0]["attrs"]["qualification"],
                         "snapshot_counter_not_elapsed_or_thread_cpu")

    def test_chrome_adapter_rejects_independent_clock_domains(self):
        bundle = complete_bundle()
        bundle["clocks"].append({"clock_id": "second", "unit": "ns", "kind": "monotonic",
                                 "alignment": "qualified", "api": "second", "source": "second",
                                 "domain": "second", "resolution_ns": "1", "uncertainty_ns": "0"})
        with self.assertRaisesRegex(tracing.TraceError, "exactly one qualified monotonic clock"):
            tracing.chrome_trace(bundle)


if __name__ == "__main__":
    unittest.main()
