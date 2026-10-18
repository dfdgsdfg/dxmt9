import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "tracing_consumer", ROOT / "scripts/tools/tracing_consumer.py")
consumer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(consumer)


def varint(value):
    result = bytearray()
    while value > 0x7f:
        result.append((value & 0x7f) | 0x80)
        value >>= 7
    result.append(value)
    return bytes(result)


def field_varint(number, value):
    return varint(number << 3) + varint(value)


def field_bytes(number, value):
    return varint((number << 3) | 2) + varint(len(value)) + value


def field_string(number, value):
    return field_bytes(number, value.encode())


def message(fields):
    return b"".join(fields)


def native_trace(include_typed_relations=False):
    """Build a tiny native Trace/TracePacket/TrackEvent protobuf fixture."""
    def event(event_type, track_uuid, name_iid, annotations=(), flow_id=None,
              terminating_flow_id=None):
        fields = [field_varint(9, event_type), field_varint(11, track_uuid),
                  field_varint(10, name_iid)]
        if flow_id is not None:
            fields.append(field_varint(47, flow_id))
        if terminating_flow_id is not None:
            fields.append(field_varint(48, terminating_flow_id))
        for key, value in annotations:
            annotation = [field_string(10, key)]
            annotation.append(field_string(6, value) if isinstance(value, str)
                              else field_varint(3, value))
            fields.append(field_bytes(4, message(annotation)))
        return message(fields)

    def packet(timestamp=None, track_event=None, name=None, descriptor=None,
               sequence_flags=None):
        fields = [field_varint(10, 1)]
        if timestamp is not None:
            fields.append(field_varint(8, timestamp))
        if sequence_flags is not None:
            fields.append(field_varint(13, sequence_flags))
        if name is not None:
            interned_name = message([field_varint(1, name[0]), field_string(2, name[1])])
            fields.append(field_bytes(12, field_bytes(2, interned_name)))
        if descriptor is not None:
            track_fields = [field_varint(1, descriptor[0]), field_string(2, descriptor[1])]
            if descriptor[2] == "process":
                process = message([field_varint(1, 123), field_string(6, "native-fixture-process")])
                track_fields.append(field_bytes(3, process))
            elif descriptor[2] == "thread":
                thread = message([field_varint(1, 123), field_varint(2, 777),
                                  field_string(5, "native-fixture-thread")])
                track_fields.append(field_bytes(4, thread))
            track = message(track_fields)
            fields.append(field_bytes(60, track))
        if track_event is not None:
            fields.append(field_bytes(11, track_event))
        return field_bytes(1, message(fields))

    trace = (packet(descriptor=(2, "native-fixture-process", "process"), sequence_flags=1) +
            packet(descriptor=(1, "native-fixture-thread", "thread")) +
            packet(descriptor=(3, "native-fixture-gpu-track", "process")) +
            packet(900, event(3, 2, 2, (("owner_id", 44), ("work_id", 55),
                ("related_id", 8), ("binding_kind", "raw_publication"),
                ("source_record", 1), ("guest_process_id", 1234),
                ("guest_thread_id", 1235),
                ("process_create_time100ns", 9007199254741011),
                ("thread_create_time100ns", 9007199254741012),
                ("event_type", 5)), flow_id=500),
                name=(2, "process_marker")) +
            packet(1000, event(1, 1, 1, (("begin_source_record", 2),
                ("begin_event_name", "scope"), ("begin_owner_id", 44),
                ("begin_work_id", 55), ("begin_event_type", 6)),
                terminating_flow_id=500),
                   name=(1, "scope")) +
            packet(1600, event(2, 1, 1, (("end_owner_id", 66),
                ("end_source_record", 3), ("end_event_type", 73),
                ("end_event_name", "scope_end")))) +
            packet(1800, event(3, 3, 3, (("source_record", 4),
                ("source_clock_id", 4), ("source_timestamp", 222),
                ("calibration_id", 9))), name=(3, "gpu_marker")))
    if include_typed_relations:
        trace += (
            packet(1100, event(3, 2, 4, (("source_record", 5), ("event_type", 3),
                ("owner_id", 7), ("work_id", 8)), flow_id=501),
                   name=(4, "replay_begin")) +
            packet(1150, event(3, 2, 5, (("source_record", 6), ("event_type", 12),
                ("owner_id", 7), ("work_id", 8), ("relation_queue_identity", 44),
                ("related_id", 55))), name=(5, "replay_publish")) +
            packet(1200, event(3, 2, 6, (("source_record", 7), ("event_type", 5),
                ("owner_id", 44), ("work_id", 55)), terminating_flow_id=501),
                   name=(6, "queue_publish")) +
            packet(1300, event(3, 2, 7, (("source_record", 8), ("event_type", 26),
                ("owner_id", 44), ("work_id", 66)), flow_id=502), name=(7, "submit_begin")) +
            packet(1400, event(3, 3, 8, (("source_record", 9), ("event_type", 18),
                ("owner_id", 66), ("relation_queue_identity", 44)),
                terminating_flow_id=502), name=(8, "gpu_command_buffer_begin")))
    return trace


def normalized_bundle():
    process = {"process_id": "p1", "pid": "123", "incarnation": "p-start",
               "identity_quality": "qualified"}
    thread = {"thread_id": "t1", "process_id": "p1", "native_tid": "777",
              "incarnation": "t-start", "identity_quality": "qualified"}
    gpu_thread = {"thread_id": "gpu-3", "process_id": "p1", "native_tid": None,
                  "track_id": "3", "track_kind": "gpu", "incarnation": "gpu-start",
                  "identity_quality": "qualified"}
    return {"schema": "dxmt9-trace-bundle-v1", "run_id": "fixture",
            "inputs": [{"sha256": hashlib.sha256(b"fixture raw input").hexdigest()}],
            "processes": [process], "threads": [thread, gpu_thread],
            "relations": [
                {"relation_id": "r2", "source_event": "p", "target_event": "b",
                 "type": "consumes", "evidence": "observed_binding",
                 "source_id": "44", "target_id": "55", "flow": True}],
            "clocks": [{"clock_id": "mono", "unit": "ns", "kind": "monotonic",
                        "api": "fixture", "source": "fixture", "domain": "fixture",
                        "resolution_ns": "1", "uncertainty_ns": "0",
                        "alignment": "qualified"}],
            "window": {"start": "900", "end": "1800", "clock_id": "mono", "closed": True},
            "events": [
                {"event_id": "p", "process_id": "p1", "thread_id": "t1",
                 "source_record": "1",
                 "clock_id": "mono", "timestamp": "900", "kind": "instant",
                 "track_kind": "process", "track_id": "2",
                 "attrs": {"name": "process_marker", "owner_id": "44",
                    "work_id": "55", "related_id": "8", "binding_kind": "raw_publication",
                    "guest_process_id": "1234",
                    "guest_thread_id": "1235",
                    "process_create_time100ns": "9007199254741011",
                    "thread_create_time100ns": "9007199254741012",
                    "event_type": "5"}},
                {"event_id": "b", "process_id": "p1", "thread_id": "t1",
                 "source_record": "2",
                 "clock_id": "mono", "timestamp": "1000", "kind": "scope_begin",
                 "track_kind": "thread", "scope_id": "scope-1",
                 "attrs": {"name": "scope", "owner_id": "44", "work_id": "55",
                    "event_type": "6", "event_name": "scope"}},
                {"event_id": "e", "process_id": "p1", "thread_id": "t1",
                 "source_record": "3",
                 "clock_id": "mono", "timestamp": "1600", "kind": "scope_end",
                 "track_kind": "thread", "scope_id": "scope-1",
                 "attrs": {"name": "scope_end", "owner_id": "66",
                    "event_type": "73", "event_name": "scope_end"}},
                {"event_id": "g", "process_id": "p1", "thread_id": "gpu-3",
                 "source_record": "4", "clock_id": "mono", "timestamp": "1800",
                 "kind": "instant", "track_kind": "gpu", "track_id": "3",
                 "attrs": {"name": "gpu_marker", "source_clock_id": "4",
                    "source_timestamp": "222", "calibration_id": "9"}}]}


class TracingConsumerTests(unittest.TestCase):
    def test_simultaneous_counter_projection_order_and_source_annotations(self):
        bundle = normalized_bundle()
        base = bundle["events"][0]
        for i, value in enumerate((100, 0)):
            row = dict(base, event_id=f"counter-{i}", source_record=f"counter-{i}",
                       source_order=str(100 + i), kind="counter",
                       attrs={"counter_value": str(value), "track_name": f"wait-{i}",
                              "observation_source": "wait", "observation_record": "1",
                              "acquisition_version": "1"})
            bundle["events"].append(row)
        result = consumer.derive_expected(bundle)
        self.assertEqual([r["value"] for r in result["counter_rows"]], [0, 100])
        rows = result["observation_counter_rows"]
        self.assertEqual(len(rows), 6)
        self.assertEqual({r["value"] for r in rows if r["key"] == "debug.observation_source"}, {"wait"})

    def test_counter_csv_decimal_is_exact_and_fractional_is_rejected(self):
        self.assertEqual(consumer._counter_integer('123456789.000000'),123456789)
        for invalid in ('1.25','NaN','Infinity'):
            with self.assertRaises(consumer.ConsumerError):
                consumer._counter_integer(invalid)

    def test_native_annotation_float_formatting_tolerance_is_limited_to_calibration(self):
        row = {"ts": 1, "dur": 2, "name": "gpu", "track_kind": "process",
               "tid": None, "pid": 3, "key": "debug.begin_calibration_scale",
               "value": "1.000001839787615"}
        actual = dict(row, value="1.00000183978761")
        self.assertTrue(consumer._annotation_rows_match([row], [actual]))
        self.assertFalse(consumer._annotation_rows_match(
            [row], [dict(actual, value="1.0000018397")]))
        self.assertFalse(consumer._annotation_rows_match(
            [dict(row, key="debug.owner_id", value="4386677808")],
            [dict(row, key="debug.owner_id", value="4386677809")]))

    def test_point_bracket_resolution_annotation_preserves_decimal_precision(self):
        bundle = normalized_bundle()
        exact = "9007199254741001"
        bundle["events"][3]["attrs"]["calibration_resolution_ns"] = exact
        expected = consumer.derive_expected(bundle)
        rows = [row for row in expected["annotation_rows"]
                if row["key"] == "debug.calibration_resolution_ns"]
        self.assertEqual([row["value"] for row in rows], [exact])

    def test_pinned_native_processor_checks_protobuf_timestamps_duration_and_tracks(self):
        tp, lock = consumer.resolve_pinned_processor()
        expected_version = lock["version"]
        with tempfile.TemporaryDirectory() as temp:
            trace = Path(temp) / "fixture.pftrace"
            trace.write_bytes(native_trace())
            expected = consumer.derive_expected(normalized_bundle())
            receipt = consumer.qualify_trace(trace, tp=tp, expected=expected)
        self.assertEqual(receipt["verdict"], "qualified")
        self.assertIn(expected_version, receipt["consumer"]["version"])
        self.assertEqual(receipt["queries"]["native_timeline"]["total_duration_ns"], 600)
        self.assertEqual(receipt["queries"]["native_timeline"]["slice_track_count"], 3)
        self.assertEqual(receipt["queries"]["native_timeline"]["thread_track_count"], 1)
        self.assertEqual(receipt["queries"]["native_timeline"]["process_track_count"], 2)
        self.assertEqual(receipt["queries"]["native_timeline"]["flow_count"], 1)
        self.assertEqual(receipt["queries"]["flow_rows"], [{
            "out_ts": 900, "out_name": "process_marker", "out_tid": None,
            "out_pid": 123, "in_ts": 1000, "in_name": "scope",
            "in_tid": 777, "in_pid": 123}])

    def test_semantic_relations_must_match_native_binding_annotations(self):
        bundle = normalized_bundle()
        bundle["relations"] = [{"relation_id": "r1", "source_event": "p",
                                "target_event": "b", "type": "publishes",
                                "evidence": "observed_binding"}]
        with self.assertRaisesRegex(consumer.ConsumerError, "not preserved by native annotations"):
            consumer.derive_expected(bundle)
        bundle["events"][0]["attrs"].update({"relation_type": "publishes",
            "relation_evidence": "observed_binding", "relation_source_id": "work-1",
            "relation_target_id": "queue-1"})
        bundle["relations"][0].update({"source_id": "work-1", "target_id": "queue-1"})
        expected = consumer.derive_expected(bundle)
        self.assertIn({"ts": 900, "dur": 0, "name": "process_marker",
                       "track_kind": "process", "tid": None, "pid": 123,
                       "key": "debug.relation_type", "value": "publishes"},
                      expected["annotation_rows"])

    def test_queue_to_encode_flow_requires_exact_owner_and_sequence_binding(self):
        bundle = normalized_bundle()
        expected = consumer.derive_expected(bundle)
        self.assertEqual(expected["flow_rows"], [{
            "out_ts": 900, "out_name": "process_marker", "out_tid": None,
            "out_pid": 123, "in_ts": 1000, "in_name": "scope",
            "in_tid": 777, "in_pid": 123}])
        bundle["events"][1]["attrs"]["work_id"] = "56"
        with self.assertRaisesRegex(consumer.ConsumerError,
                                    "exact queue/sequence binding"):
            consumer.derive_expected(bundle)

    def test_native_trace_processor_verifies_all_typed_flow_endpoints(self):
        bundle = normalized_bundle()
        bundle["events"].append({"event_id": "rp", "process_id": "p1", "thread_id": "t1",
            "source_record": "5", "clock_id": "mono", "timestamp": "1100", "kind": "instant",
            "track_kind": "process", "track_id": "2",
            "attrs": {"name": "replay_begin", "event_type": "3", "owner_id": "7",
                       "work_id": "8"}})
        bundle["events"].append({"event_id": "proof", "process_id": "p1", "thread_id": "t1",
            "source_record": "6", "clock_id": "mono", "timestamp": "1150", "kind": "instant",
            "track_kind": "process", "track_id": "2",
            "attrs": {"name": "replay_publish", "event_type": "12", "owner_id": "7",
                "work_id": "8", "relation_queue_identity": "44", "related_id": "55"}})
        bundle["events"].append({"event_id": "qp", "process_id": "p1", "thread_id": "t1",
            "source_record": "7", "clock_id": "mono", "timestamp": "1200", "kind": "instant",
            "track_kind": "process", "track_id": "2",
            "attrs": {"name": "queue_publish", "event_type": "5", "owner_id": "44",
                       "work_id": "55"}})
        bundle["events"].append({"event_id": "sb", "process_id": "p1", "thread_id": "t1",
            "source_record": "8", "clock_id": "mono", "timestamp": "1300", "kind": "instant",
            "track_kind": "process", "track_id": "2",
            "attrs": {"name": "submit_begin", "event_type": "26", "owner_id": "44",
                       "work_id": "66"}})
        bundle["events"].append({"event_id": "gb", "process_id": "p1", "thread_id": "gpu-3",
            "source_record": "9", "clock_id": "mono", "timestamp": "1400", "kind": "instant",
            "track_kind": "gpu", "track_id": "3",
            "attrs": {"name": "gpu_command_buffer_begin", "event_type": "18", "owner_id": "66",
                       "relation_queue_identity": "44"}})
        bundle["relations"] += [
            {"relation_id": "r3", "source_event": "rp", "target_event": "qp",
             "evidence_event": "proof", "type": "publishes", "evidence": "observed_binding",
             "source_id": "44", "target_id": "55", "flow": True},
            {"relation_id": "r4", "source_event": "sb", "target_event": "gb",
             "type": "submitted_in", "evidence": "observed_binding",
             "source_id": "44", "target_id": "66", "flow": True}]
        expected = consumer.derive_expected(bundle)
        with tempfile.TemporaryDirectory() as temp:
            trace = Path(temp) / "typed.pftrace"
            trace.write_bytes(native_trace(include_typed_relations=True))
            receipt = consumer.qualify_trace(trace, expected=expected)
        self.assertEqual(receipt["verdict"], "qualified")
        self.assertEqual(receipt["queries"]["native_timeline"]["flow_count"], 3)
        self.assertEqual(len(receipt["queries"]["flow_rows"]), 3)
        self.assertIn({"ts": 900, "dur": 0, "name": "process_marker",
                       "track_kind": "process", "tid": None, "pid": 123,
                       "key": "debug.binding_kind", "value": "raw_publication"},
                      receipt["queries"]["annotation_rows"])

    def test_queue_publish_flow_allows_fanout_only_for_exact_binding(self):
        bundle = normalized_bundle()
        bundle["events"][0]["attrs"].update({"event_type": "5", "owner_id": "44",
                                               "work_id": "55"})
        bundle["events"][1]["attrs"].update({"event_type": "6", "owner_id": "44",
                                               "work_id": "55"})
        bundle["events"][2]["attrs"].update({"event_type": "6", "owner_id": "44",
                                               "work_id": "55"})
        bundle["relations"] = [
            {"relation_id": "r1", "source_event": "p", "target_event": "b",
             "type": "consumes", "evidence": "observed_binding", "flow": True},
            {"relation_id": "r2", "source_event": "p", "target_event": "e",
             "type": "consumes", "evidence": "observed_binding", "flow": True}]
        self.assertEqual(len(consumer.derive_expected(bundle)["flow_rows"]), 2)
        bundle["events"][2]["attrs"]["work_id"] = "56"
        with self.assertRaisesRegex(consumer.ConsumerError,
                                    "exact queue/sequence binding"):
            consumer.derive_expected(bundle)

    def test_replay_to_queue_flow_requires_explicit_publish_evidence(self):
        bundle = normalized_bundle()
        bundle["events"][0]["attrs"].update({"event_type": "3", "owner_id": "7",
                                               "work_id": "8"})
        bundle["events"][1]["attrs"].update({"event_type": "5", "owner_id": "44",
                                               "work_id": "55"})
        bundle["events"][2]["attrs"].update({"event_type": "12", "owner_id": "7",
            "work_id": "8", "relation_queue_identity": "44", "related_id": "55"})
        bundle["relations"] = [{"relation_id": "r1", "source_event": "p",
            "target_event": "b", "evidence_event": "e", "type": "publishes",
            "evidence": "observed_binding", "flow": True,
            "source_id": "44", "target_id": "55"}]
        self.assertEqual(len(consumer.derive_expected(bundle)["flow_rows"]), 1)
        for field, value in (("evidence_event", "missing"), ("source_id", "45"),
                             ("target_id", "56")):
            broken = {**bundle, "relations": [dict(bundle["relations"][0])]}
            broken["relations"][0][field] = value
            with self.assertRaises(consumer.ConsumerError):
                consumer.derive_expected(broken)
        bundle["events"][2]["attrs"]["relation_queue_identity"] = "45"
        with self.assertRaisesRegex(consumer.ConsumerError, "exact evidence binding"):
            consumer.derive_expected(bundle)

    def test_submit_to_gpu_flow_requires_exact_queue_and_command_buffer(self):
        bundle = normalized_bundle()
        bundle["events"][0]["attrs"].update({"event_type": "26", "owner_id": "44",
                                               "work_id": "55"})
        bundle["events"][3]["attrs"].update({"event_type": "18", "owner_id": "55",
                                               "relation_queue_identity": "44"})
        bundle["relations"] = [{"relation_id": "r1", "source_event": "p",
            "target_event": "g", "type": "submitted_in", "evidence": "observed_binding",
            "flow": True, "source_id": "44", "target_id": "55"}]
        self.assertEqual(len(consumer.derive_expected(bundle)["flow_rows"]), 1)
        for field, value in (("relation_queue_identity", "45"), ("owner_id", "56")):
            broken = {**bundle, "events": [dict(e, attrs=dict(e["attrs"]))
                for e in bundle["events"]]}
            broken["events"][3]["attrs"][field] = value
            with self.assertRaisesRegex(consumer.ConsumerError, "exact queue/command-buffer binding"):
                consumer.derive_expected(broken)

    def test_guest_identity_annotations_round_trip_exact_64_bit_values(self):
        bundle = normalized_bundle()
        expected = consumer.derive_expected(bundle)
        annotations = expected["annotation_rows"]
        self.assertIn({"ts": 900, "dur": 0, "name": "process_marker",
                       "track_kind": "process", "tid": None, "pid": 123,
                       "key": "debug.guest_process_id", "value": "1234"}, annotations)
        self.assertIn({"ts": 900, "dur": 0, "name": "process_marker",
                       "track_kind": "process", "tid": None, "pid": 123,
                       "key": "debug.process_create_time100ns",
                       "value": "9007199254741011"}, annotations)
        self.assertIn({"ts": 1000, "dur": 600, "name": "scope",
                       "track_kind": "thread", "tid": 777, "pid": 123,
                       "key": "debug.begin_event_type", "value": "6"}, annotations)
        self.assertIn({"ts": 1000, "dur": 600, "name": "scope",
                       "track_kind": "thread", "tid": 777, "pid": 123,
                       "key": "debug.end_event_type", "value": "73"}, annotations)
        self.assertIn({"ts": 1000, "dur": 600, "name": "scope",
                       "track_kind": "thread", "tid": 777, "pid": 123,
                       "key": "debug.end_event_name", "value": "scope_end"}, annotations)

    def test_instant_event_depth_tracks_open_scope_nesting(self):
        bundle = normalized_bundle()
        bundle["events"].append({"event_id": "nested", "process_id": "p1",
            "thread_id": "t1", "source_record": "5", "clock_id": "mono",
            "timestamp": "1200", "kind": "instant", "track_kind": "thread",
            "attrs": {"name": "nested_marker", "event_type": "11"}})
        expected = consumer.derive_expected(bundle)
        nested = next(row for row in expected["slice_rows"]
                      if row["name"] == "nested_marker")
        self.assertEqual(nested["depth"], 1)
        self.assertEqual(expected["nested_slice_count"], 1)

    def test_expectations_apply_exact_rational_clock_transform(self):
        bundle = normalized_bundle()
        bundle["clocks"].append({"clock_id": "ticks", "unit": "ticks", "kind": "monotonic",
            "api": "fixture", "source": "fixture", "domain": "counter", "resolution_ns": "1",
            "uncertainty_ns": "0", "alignment": "qualified",
            "mapping": {"host_clock_id": "mono", "scale_num": "2", "scale_den": "1",
                "offset_ns": "1", "valid_from": "100", "valid_to": "400", "error_ns": "0",
                "calibration": [{"source_ticks": "100", "host_ns": "201"},
                                {"source_ticks": "400", "host_ns": "801"}]}})
        bundle["window"].update({"start": "201", "end": "801"})
        source_ticks = [100, 150, 400]
        for event, tick in zip(bundle["events"], source_ticks):
            event["clock_id"] = "ticks"
            event["timestamp"] = str(tick)
        expected = consumer.derive_expected(bundle)
        self.assertEqual(expected["slice_rows"][0]["ts"], 201)
        self.assertEqual(expected["total_duration_ns"], 500)

    def test_rejects_empty_or_malformed_trace_instead_of_empty_success(self):
        tp, _ = consumer.resolve_pinned_processor()
        with tempfile.TemporaryDirectory() as temp:
            trace = Path(temp) / "empty.pftrace"
            trace.write_bytes(b"")
            with self.assertRaisesRegex(consumer.ConsumerError, "missing or empty"):
                consumer.qualify_trace(trace, tp=tp)
            trace.write_bytes(b"not a protobuf trace")
            with self.assertRaises(consumer.ConsumerError):
                consumer.qualify_trace(trace, tp=tp)


if __name__ == "__main__":
    unittest.main()
