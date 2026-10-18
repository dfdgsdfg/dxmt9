#!/usr/bin/env python3
"""Validate and normalize dxmt9 tracing inputs without inventing evidence.

The normalized bundle is an offline interchange format. It does not assert that
different clocks, reused OS IDs, or neighboring work records are related.
"""
from __future__ import annotations

import argparse
import csv
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import hashlib
import io
import json
import re
import sys
from pathlib import Path
from typing import Any

SCHEMA = "dxmt9-trace-bundle-v1"
EVENTS = ("present import_begin import_end raw_ready mutation_ready "
          "stateblock_ready replay_begin replay_end queue_empty queue_reserved "
          "queue_ready pop_attempt pop_wait pop_wake publish encode_begin "
          "encode_end submit replay_ledger writer_wait_begin writer_wait_end "
          "commit_wait_begin commit_wait_end publish_reason publish_commands "
          "scope_boundary query_issue completion_dequeue completion_observed "
          "retire encode_prefetch_begin encode_prefetch_end "
          "encode_backend_begin encode_backend_end framegraph_encode_begin "
          "framegraph_encode_end framegraph_pool_binding chunk_body_begin "
          "chunk_body_end chunk_command_begin chunk_command_end "
          "chunk_split_begin chunk_split_end encode_draw_begin encode_draw_end "
          "parallel_observe_begin parallel_observe_counts parallel_observe_draws "
          "parallel_observe_children parallel_observe_reject parallel_observe_end "
          "parallel_observe_status parallel_observe_imbalance "
          "parallel_observe_resources parallel_access_range "
          "parallel_access_counts parallel_access_touches parallel_access_reuse "
          "parallel_access_flags prepared_begin prepared_allocated "
          "prepared_appended prepared_sealed prepared_status prepared_identities "
          "prepared_accesses prepared_ranges prepared_unresolved prepared_end "
          "prepared_source_index prepared_source_generation "
          "prepared_storage_generation prepared_storage_pages prepared_screen "
          "prepared_storage_capacity prepared_cardinality_begin "
          "prepared_cardinality_status prepared_cardinality_commands "
          "prepared_cardinality_unresolved prepared_cardinality_accesses "
          "prepared_cardinality_identities prepared_cardinality_end "
          "framegraph_planning_phase stateblock_wait_begin stateblock_wait_end "
          "stateblock_wait_capacity stateblock_wait_adopted mark_ticket_wait_begin "
          "mark_ticket_wait_end mark_ticket_hold_end queue_mutex_hold_begin "
          "queue_mutex_hold_end queue_submit_phase queue_submit_validation_end "
          "queue_submit_summary_family session_release_post session_release_raw "
          "session_release_policy session_release_queue session_release_action_begin "
          "session_release_ack session_release_return session_release_close "
          "session_release_reuse draw_run_phase prepared_replay_phase_begin "
          "prepared_replay_phase_end provider_replay_phase slot_validation_extent "
          "slot_validation_begin slot_validation_end").split()
SUPPLY_EVENTS = {name: i for i, name in enumerate(EVENTS)}
WAIT_SITES = {0: "query_receipt", 1: "resource_replay", 2: "buffer_lock",
              3: "buffer_unlock", 4: "query_queue_acquire"}
INT = re.compile(r"(?:0|[1-9][0-9]*)\Z")
SIGNED_INT = re.compile(r"(?:0|-?[1-9][0-9]*)\Z")
VALID_RELATIONS = {"contains", "publishes", "consumes", "waits_for",
                   "submitted_in", "observed_complete", "caller_context",
                   "annotates", "flow"}
RELATION_REQUIREMENTS = {"replay_queue", "queue_encode", "submit_gpu"}
RELATION_DIAGNOSTICS = {
    "replay_queue": ("unmatched_replay_publish", "ambiguous_replay_publish"),
    "queue_encode": ("unmatched_queue_publish", "ambiguous_queue_publish",
                     "unmatched_encode_begin"),
    "submit_gpu": ("unmatched_submit_begin", "ambiguous_submit_begin",
                   "unmatched_gpu_command_buffer", "ambiguous_gpu_command_buffer"),
}
ALL_RELATION_DIAGNOSTICS = {
    key for keys in RELATION_DIAGNOSTICS.values() for key in keys
} | {"duplicate_source_records"}


class TraceError(ValueError):
    pass


def _mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise TraceError(f"{name} must be an object")
    return value


def _array(value: Any, name: str) -> list[Any]:
    if not isinstance(value, list):
        raise TraceError(f"{name} must be an array")
    return value


def _id(value: Any, name: str) -> str:
    # JSON integers above 2**53 may already have been rounded by callers. The
    # wire contract therefore requires decimal strings for identity and time.
    if not isinstance(value, str) or not value or value.strip() != value:
        raise TraceError(f"{name} must be a non-empty string")
    return value


def _uint(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise TraceError(f"{name} must be an unsigned decimal integer")
    text = str(value)
    if not INT.fullmatch(text):
        raise TraceError(f"{name} must be an unsigned decimal integer")
    return int(text)


def _timestamp(value: Any, name: str) -> int:
    if not isinstance(value, str):
        raise TraceError(f"{name} must be a decimal string to preserve 64-bit precision")
    return _uint(value, name)


def _signed(value: Any, name: str) -> int:
    if not isinstance(value, str) or not SIGNED_INT.fullmatch(value):
        raise TraceError(f"{name} must be a signed decimal string")
    return int(value)


def map_clock_timestamp(clock: dict[str, Any], timestamp: str,
                        max_error_ns: int) -> tuple[int, int]:
    """Map source ticks with exact integer rational arithmetic and bounded error."""
    raw = _timestamp(timestamp, "timestamp")
    mapping = _mapping(clock.get("mapping"), "clock.mapping")
    numerator = _uint(mapping.get("scale_num"), "mapping.scale_num")
    denominator = _uint(mapping.get("scale_den"), "mapping.scale_den")
    if not numerator or not denominator:
        raise TraceError("clock scale numerator/denominator must be positive")
    offset = _signed(mapping.get("offset_ns"), "mapping.offset_ns")
    valid_from = _timestamp(mapping.get("valid_from"), "mapping.valid_from")
    valid_to = _timestamp(mapping.get("valid_to"), "mapping.valid_to")
    error = _uint(mapping.get("error_ns"), "mapping.error_ns")
    if valid_from >= valid_to or not valid_from <= raw <= valid_to:
        raise TraceError("timestamp is outside clock calibration validity range")
    if error > max_error_ns:
        raise TraceError("clock mapping uncertainty exceeds declared bound")
    scaled = raw * numerator
    # Nearest-integer rounding; all quantities are integers, including the
    # rounding error. Do not convert timestamps through binary floating point.
    mapped = (scaled + denominator // 2) // denominator + offset
    return mapped, error


def _decimal(value: Any, name: str) -> Decimal:
    if not isinstance(value, (str, int)) or isinstance(value, bool):
        raise TraceError(f"{name} must be a decimal string")
    try:
        result = Decimal(str(value))
    except InvalidOperation as exc:
        raise TraceError(f"{name} must be a finite decimal") from exc
    if not result.is_finite():
        raise TraceError(f"{name} must be a finite decimal")
    return result


def _map_calibrated_timestamp(source_ticks: int, scale: Decimal,
                              offset_ns: Decimal) -> int:
    return int((Decimal(source_ticks) * scale + offset_ns).to_integral_value(
        rounding=ROUND_HALF_UP))


def _digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_bundle(bundle: dict[str, Any], *, require_complete: bool = False) -> dict[str, Any]:
    """Validate identities, timestamps, endpoint structure, relations and gates."""
    _mapping(bundle, "bundle")
    if bundle.get("schema") != SCHEMA:
        raise TraceError(f"unsupported schema: {bundle.get('schema')!r}")
    for key in ("producer", "run_id", "inputs", "processes", "threads", "clocks",
                "events", "relations", "window", "loss", "coverage"):
        if key not in bundle:
            raise TraceError(f"missing {key}")
    ids: dict[str, set[str]] = {}
    if not isinstance(bundle["inputs"], list):
        raise TraceError("inputs must be an array")
    for i, ref in enumerate(bundle["inputs"]):
        ref = _mapping(ref, f"inputs[{i}]")
        digest = ref.get("sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise TraceError(f"inputs[{i}].sha256 must be a lowercase SHA-256 digest")
    for table in ("processes", "threads", "clocks", "events", "relations"):
        rows = _array(bundle[table], table)
        seen: set[str] = set()
        for i, raw in enumerate(rows):
            row = _mapping(raw, f"{table}[{i}]")
            key = {"processes": "process_id", "threads": "thread_id", "clocks": "clock_id",
                   "events": "event_id", "relations": "relation_id"}[table]
            value = _id(row.get(key), f"{table}[{i}].{key}")
            if value in seen:
                raise TraceError(f"duplicate {key}: {value}")
            seen.add(value)
        ids[table] = seen
    processes = {p["process_id"]: p for p in bundle["processes"]}
    threads = {t["thread_id"]: t for t in bundle["threads"]}
    clocks = {c["clock_id"]: c for c in bundle["clocks"]}
    for p in processes.values():
        _id(p.get("pid"), "process.pid")
        _id(p.get("incarnation"), "process.incarnation")
        if p.get("pid_namespace") == "guest":
            _id(p.get("guest_process_id"), "guest_process.guest_process_id")
            _id(p.get("creation_time100ns"), "guest_process.creation_time100ns")
    for t in threads.values():
        track_kind = t.get("track_kind", "thread")
        if track_kind == "gpu":
            if t.get("native_tid") is not None:
                _id(t.get("native_tid"), "gpu_track.native_tid")
            _id(t.get("track_id", t.get("thread_id")), "gpu_track.track_id")
        elif t.get("tid_namespace") == "guest":
            if t.get("native_tid") is not None:
                raise TraceError("guest thread must not claim a native TID")
            _id(t.get("guest_tid"), "guest_thread.guest_tid")
        else:
            _id(t.get("native_tid"), "thread.native_tid")
        _id(t.get("incarnation"), "thread.incarnation")
        if t.get("process_id") not in processes:
            raise TraceError("thread references unknown process")
    for c in clocks.values():
        for field in ("api", "source", "domain", "resolution_ns", "uncertainty_ns", "alignment"):
            _id(c.get(field), f"clock.{field}")
        if c.get("unit") not in ("ns", "ticks", "count"):
            raise TraceError(f"unsupported clock unit: {c.get('unit')!r}")
        if c.get("kind") not in ("monotonic", "thread_cpu_counter", "sample_count", "gpu",
                                  "external_counter"):
            raise TraceError(f"unsupported clock kind: {c.get('kind')!r}")
        mapping = c.get("mapping")
        if mapping is not None:
            m = _mapping(mapping, "clock.mapping")
            host_clock = m.get("host_clock_id")
            if host_clock not in clocks or clocks[host_clock].get("kind") != "monotonic":
                raise TraceError("clock mapping references unknown host monotonic clock")
            samples = _array(m.get("calibration", []), "clock.mapping.calibration")
            if len(samples) < 2:
                raise TraceError("clock mapping requires at least two calibration observations")
            _uint(m.get("scale_num"), "mapping.scale_num")
            _uint(m.get("scale_den"), "mapping.scale_den")
            _signed(m.get("offset_ns"), "mapping.offset_ns")
            _timestamp(m.get("valid_from"), "mapping.valid_from")
            _timestamp(m.get("valid_to"), "mapping.valid_to")
            bound = _uint(m.get("error_ns"), "mapping.error_ns")
            for sample in samples:
                sample = _mapping(sample, "calibration observation")
                source_ns = _timestamp(sample.get("source_ticks"), "calibration.source_ticks")
                host_ns = _timestamp(sample.get("host_ns"), "calibration.host_ns")
                predicted, _ = map_clock_timestamp(c, str(source_ns), bound)
                if abs(predicted-host_ns) > bound:
                    raise TraceError("clock mapping does not fit calibration observation")
    events = {e["event_id"]: e for e in bundle["events"]}
    scopes: dict[tuple[str, str, str, str], dict[str, Any]] = {}
    source_orders: set[tuple[str, str, str]] = set()
    point_generations: dict[tuple[str, str], int] = {}
    for event in events.values():
        for key, ref, label in (("process_id", processes, "process"),
                                ("thread_id", threads, "thread"),
                                ("clock_id", clocks, "clock")):
            if event.get(key) not in ref:
                raise TraceError(f"event references unknown {label}")
        _id(event.get("source_record"), "event.source_record")
        _id(event.get("collector"), "event.collector")
        order = _id(event.get("source_order"), "event.source_order")
        if threads[event["thread_id"]]["process_id"] != event["process_id"]:
            raise TraceError("event thread belongs to a different process")
        track_kind = event.get("track_kind", "thread")
        if track_kind not in ("thread", "process", "async", "gpu"):
            raise TraceError(f"unsupported event track kind: {track_kind!r}")
        if track_kind == "gpu" and threads[event["thread_id"]].get("track_kind") != "gpu":
            raise TraceError("GPU event must reference a GPU track")
        if track_kind != "gpu" and threads[event["thread_id"]].get("track_kind", "thread") == "gpu":
            raise TraceError("non-GPU event references a GPU track")
        if track_kind == "gpu" and str(event.get("track_id")) != str(
                threads[event["thread_id"]].get("track_id", event["thread_id"])):
            raise TraceError("GPU event track identity disagrees with its registry row")
        order_key = (event["collector"], event["thread_id"], order)
        if order_key in source_orders:
            raise TraceError("duplicate source_order on collector/thread")
        source_orders.add(order_key)
        _timestamp(event.get("timestamp"), "event.timestamp")
        clock = clocks[event["clock_id"]]
        if clock.get("mapping") is not None:
            map_clock_timestamp(clock, event["timestamp"], _uint(clock["mapping"]["error_ns"], "mapping.error_ns"))
        if not isinstance(event.get("kind"), str) or not event["kind"]:
            raise TraceError("event.kind is required")
        if not isinstance(event.get("attrs", {}), dict):
            raise TraceError("event.attrs must be an object")
        if event["kind"] in ("scope_begin", "scope_end"):
            sid = _id(event.get("scope_id"), "event.scope_id")
            thread = threads[event["thread_id"]]
            key = (event["process_id"], event["thread_id"], thread["incarnation"], sid)
            pair = scopes.setdefault(key, {})
            edge = "begin" if event["kind"] == "scope_begin" else "end"
            if edge in pair:
                raise TraceError(f"duplicate scope {edge}: {sid}")
            pair[edge] = event
    policy_limit = bundle.get("qualification_policy", {}).get("max_clock_error_ns")
    max_error_ns = (_uint(policy_limit, "qualification_policy.max_clock_error_ns")
                    if policy_limit is not None else None)
    calibration_issues: list[str] = []
    calibration_rows = _array(bundle.get("clock_calibrations", []), "clock_calibrations")
    calibrations: dict[tuple[str, str], dict[str, Any]] = {}
    for index, raw_calibration in enumerate(calibration_rows):
        calibration = _mapping(raw_calibration, f"clock_calibrations[{index}]")
        mapping_kind = _id(calibration.get("mapping_kind"), "calibration.mapping_kind")
        if mapping_kind not in ("host_api_identity", "two_anchor", "bracket",
                                "point_bracket"):
            raise TraceError(f"unsupported calibration mapping_kind: {mapping_kind!r}")
        source_clock_id = _id(calibration.get("source_clock_id"), "calibration.source_clock_id")
        calibration_id = _id(calibration.get("calibration_id"), "calibration.calibration_id")
        key = (source_clock_id, calibration_id)
        if key in calibrations:
            raise TraceError("duplicate calibration source_clock_id/calibration_id")
        host_clock_id = _id(calibration.get("host_clock_id"), "calibration.host_clock_id")
        if host_clock_id not in clocks or clocks[host_clock_id].get("kind") != "monotonic":
            raise TraceError("calibration references unknown host monotonic clock")
        source_ticks = _timestamp(calibration.get("source_ticks"), "calibration.source_ticks")
        host_ns = _timestamp(calibration.get("host_ns"), "calibration.host_ns")
        scale = _decimal(calibration.get("scale"), "calibration.scale")
        offset = _decimal(calibration.get("offset_ns"), "calibration.offset_ns")
        error_ns = _uint(calibration.get("error_ns"), "calibration.error_ns")
        if max_error_ns is not None and error_ns > max_error_ns:
            calibration_issues.append(f"calibration_error_exceeds_policy:{calibration_id}")
        if scale <= 0:
            raise TraceError("calibration scale must be positive")
        if abs(_map_calibrated_timestamp(source_ticks, scale, offset) - host_ns) > error_ns:
            raise TraceError("calibration source observation does not map to host timestamp")
        if mapping_kind == "host_api_identity":
            if scale != 1 or offset != 0 or source_ticks != host_ns:
                raise TraceError("host API identity calibration is not an exact ns identity")
        else:
            source_before = _timestamp(calibration.get("source_before"),
                                       "calibration.source_before")
            source_after = _timestamp(calibration.get("source_after"),
                                      "calibration.source_after")
            host_before = _timestamp(calibration.get("host_before_ns"),
                                     "calibration.host_before_ns")
            host_after = _timestamp(calibration.get("host_after_ns"),
                                    "calibration.host_after_ns")
            valid_begin = _timestamp(calibration.get("valid_begin"),
                                     "calibration.valid_begin")
            valid_end = _timestamp(calibration.get("valid_end"),
                                   "calibration.valid_end")
            if mapping_kind == "point_bracket":
                frequency = _uint(calibration.get("frequency"),
                                  "calibration.frequency")
                resolution_ns = _uint(calibration.get("resolution_ns"),
                                      "calibration.resolution_ns")
                if not frequency:
                    raise TraceError("point-bracket frequency must be positive")
                if (source_before != source_ticks or source_after != source_ticks or
                        valid_begin != source_ticks or valid_end != source_ticks):
                    raise TraceError("point-bracket calibration must have single-point validity")
                if host_before > host_after:
                    raise TraceError("point-bracket host endpoints are inverted")
                midpoint = (host_before + host_after) // 2
                if host_ns != midpoint:
                    raise TraceError("point-bracket host timestamp is not the floor midpoint")
                expected_scale = Decimal(1_000_000_000) / Decimal(frequency)
                expected_offset = Decimal(host_ns) - expected_scale * Decimal(source_ticks)
                scale_tolerance = max(abs(expected_scale), Decimal(1)) * Decimal("1e-15")
                if abs(scale - expected_scale) > scale_tolerance:
                    raise TraceError("point-bracket scale disagrees with its QPC frequency")
                offset_tolerance = min(
                    max(abs(expected_offset), Decimal(1)) * Decimal("1e-15"),
                    Decimal(1000))
                if abs(offset - expected_offset) > offset_tolerance:
                    raise TraceError("point-bracket transform does not fit the exact observation")
                tick_error_ns = (1_000_000_000 + frequency - 1) // frequency
                minimum_error = ((host_after - host_before + 1) // 2 +
                                 tick_error_ns + resolution_ns)
                if error_ns < minimum_error:
                    raise TraceError("point-bracket calibration error underclaims its bound")
                if str(host_clock_id) in clocks:
                    host_resolution = _uint(clocks[host_clock_id]["resolution_ns"],
                                            "host clock resolution_ns")
                    if resolution_ns < host_resolution:
                        raise TraceError("point-bracket resolution is below host clock resolution")
            else:
                if source_before >= source_after or valid_begin >= valid_end:
                    raise TraceError("calibration anchors or validity range are not increasing")
                for source_anchor, host_anchor in ((source_before, host_before),
                                                   (source_after, host_after)):
                    if abs(_map_calibrated_timestamp(source_anchor, scale, offset) -
                           host_anchor) > error_ns:
                        raise TraceError("calibration transform does not fit its anchors")
        calibrations[key] = calibration

    for event in events.values():
        attrs = event.get("attrs", {})
        if attrs.get("source_timestamp") is None:
            event_code = str(attrs.get("event_type", attrs.get("event_code", "")))
            if event.get("track_kind") == "gpu" and event_code in {"18", "19", "20", "21", "22", "23"}:
                calibration_issues.append(f"event_calibration_metadata_missing:{event['event_id']}")
            continue
        source_ticks = _timestamp(attrs["source_timestamp"], "event.source_timestamp")
        source_clock_id = _id(attrs.get("source_clock_id"), "event.source_clock_id")
        calibration_id = _id(attrs.get("calibration_id"), "event.calibration_id")
        calibration = calibrations.get((source_clock_id, calibration_id))
        if calibration is None:
            calibration_issues.append(f"event_calibration_missing:{event['event_id']}")
            continue
        unit = attrs.get("source_timestamp_unit")
        if calibration["mapping_kind"] == "host_api_identity":
            valid_begin = _timestamp(calibration.get("valid_begin"),
                                     "calibration.valid_begin")
            valid_end = _timestamp(calibration.get("valid_end"),
                                   "calibration.valid_end")
            if not valid_begin <= source_ticks <= valid_end:
                calibration_issues.append(f"event_calibration_out_of_range:{event['event_id']}")
                continue
            if unit != "ns" or source_ticks != int(event["timestamp"]):
                raise TraceError("host API identity event timestamp is not an exact ns identity")
            continue
        if unit != "ticks":
            raise TraceError("non-identity calibrated event timestamp unit must be ticks")
        valid_begin = int(calibration["valid_begin"])
        valid_end = int(calibration["valid_end"])
        if calibration["mapping_kind"] == "point_bracket":
            generation = _uint(attrs.get("generation"), "event.generation")
            if generation == 0:
                raise TraceError("point-bracket event generation must be positive")
            generation_key = (event["process_id"], source_clock_id)
            prior_generation = point_generations.setdefault(generation_key, generation)
            if prior_generation != generation:
                raise TraceError("point-bracket generation changed within source clock")
            if bundle.get("collector") == "engine-perfetto":
                process_generation = _uint(
                    processes[event["process_id"]]["incarnation"],
                    "engine-perfetto process incarnation")
                if generation != process_generation:
                    raise TraceError(
                        "point-bracket generation disagrees with process incarnation")
            if attrs.get("clock_frequency") is not None and _uint(
                    attrs["clock_frequency"], "event.clock_frequency") != int(
                        calibration["frequency"]):
                raise TraceError("point-bracket frequency disagrees with event clock frequency")
            if source_ticks != int(calibration["source_ticks"]):
                raise TraceError("point-bracket event source timestamp differs from calibrated point")
            if int(event["timestamp"]) != int(calibration["host_ns"]):
                raise TraceError("point-bracket event timestamp is not the exact host midpoint")
            event_error = _uint(attrs.get("uncertainty_ns"), "event.uncertainty_ns")
            if event_error < int(calibration["error_ns"]):
                raise TraceError("point-bracket event uncertainty underclaims calibration error")
            if (attrs.get("source_api") != "QueryPerformanceCounter" or
                    attrs.get("source_timestamp_unit") != "ticks"):
                raise TraceError("point-bracket event must preserve QueryPerformanceCounter ticks")
            if attrs.get("calibration_resolution_ns") is None or int(
                    _uint(attrs["calibration_resolution_ns"],
                          "event.calibration_resolution_ns")) != int(calibration["resolution_ns"]):
                raise TraceError("point-bracket event resolution annotation disagrees with calibration")
            if max_error_ns is not None and event_error > max_error_ns:
                calibration_issues.append(f"event_uncertainty_exceeds_policy:{event['event_id']}")
            continue
        if not valid_begin <= source_ticks <= valid_end:
            calibration_issues.append(f"event_calibration_out_of_range:{event['event_id']}")
            continue
        mapped = _map_calibrated_timestamp(source_ticks,
            _decimal(calibration["scale"], "calibration.scale"),
            _decimal(calibration["offset_ns"], "calibration.offset_ns"))
        event_error = _uint(attrs.get("uncertainty_ns"), "event.uncertainty_ns")
        allowed_error = int(calibration["error_ns"])
        if max_error_ns is not None:
            allowed_error = min(allowed_error, max_error_ns)
            if event_error > max_error_ns:
                calibration_issues.append(f"event_uncertainty_exceeds_policy:{event['event_id']}")
        if abs(mapped - int(event["timestamp"])) > allowed_error:
            calibration_issues.append(f"event_calibration_mapping_mismatch:{event['event_id']}")
    clipped = 0
    complete_scopes = []
    for sid, pair in scopes.items():
        if set(pair) != {"begin", "end"}:
            clipped += 1
            continue
        begin, end = pair["begin"], pair["end"]
        if begin["clock_id"] != end["clock_id"] or int(begin["timestamp"]) > int(end["timestamp"]):
            raise TraceError(f"invalid scope endpoints: {sid[3]}")
        complete_scopes.append((sid[:3], begin, end))
    by_track: dict[tuple[str, str, str], list[tuple[int, int, str]]] = {}
    for track, begin, end in complete_scopes:
        by_track.setdefault(track, []).append((int(begin["timestamp"]), int(end["timestamp"]),
                                               begin["scope_id"]))
    for intervals in by_track.values():
        stack: list[tuple[int, str]] = []
        previous_interval = None
        for start, end, scope_id in sorted(intervals, key=lambda item: (item[0], -item[1], item[2])):
            while stack and start >= stack[-1][0]:
                stack.pop()
            if stack and end > stack[-1][0]:
                raise TraceError(f"overlapping non-nested scopes: {stack[-1][1]} and {scope_id}")
            if previous_interval == (start, end):
                raise TraceError("duplicate scope interval has ambiguous nesting")
            stack.append((end, scope_id))
            previous_interval = (start, end)
    for relation in bundle["relations"]:
        if relation.get("source_event") not in events or relation.get("target_event") not in events:
            raise TraceError("relation references unknown event")
        if relation.get("evidence_event") is not None and relation["evidence_event"] not in events:
            raise TraceError("relation references unknown evidence event")
        if relation.get("type") not in VALID_RELATIONS:
            raise TraceError(f"unknown relation type: {relation.get('type')!r}")
        _id(relation.get("evidence"), "relation.evidence")
        if "flow" in relation and not isinstance(relation["flow"], bool):
            raise TraceError("relation.flow must be boolean")
        for field in ("source_id", "target_id"):
            if relation.get(field) is not None:
                _id(relation[field], f"relation.{field}")
    window = _mapping(bundle["window"], "window")
    if not isinstance(window.get("closed"), bool):
        raise TraceError("window.closed must be boolean")
    if window.get("start") is not None:
        _timestamp(window["start"], "window.start")
    if window.get("end") is not None:
        _timestamp(window["end"], "window.end")
    if window.get("start") is not None and window.get("end") is not None:
        if int(window["start"]) >= int(window["end"]):
            raise TraceError("empty or inverted window")
    window_clock_id = window.get("clock_id")
    if window_clock_id is not None and window_clock_id not in clocks:
        raise TraceError("window references unknown clock")
    if window.get("start") is None or window.get("end") is None or window_clock_id is None:
        reasons_window = ["capture_window_bounds_or_clock_missing"]
    else:
        reasons_window = []
    if window.get("start") is not None and window.get("end") is not None and window_clock_id:
        for event in bundle["events"]:
            event_clock = clocks[event["clock_id"]]
            timestamp = int(event["timestamp"])
            if event["clock_id"] == window_clock_id:
                aligned = timestamp
            elif (event_clock.get("mapping") is not None and
                  event_clock["mapping"].get("host_clock_id") == window_clock_id):
                aligned, _ = map_clock_timestamp(
                    event_clock, event["timestamp"],
                    _uint(event_clock["mapping"]["error_ns"], "mapping.error_ns"))
            else:
                reasons_window.append("event_clock_not_aligned_to_window")
                continue
            if not int(window["start"]) <= aligned <= int(window["end"]):
                raise TraceError(f"event timestamp outside declared capture window: {event['event_id']}")
    loss = _mapping(bundle["loss"], "loss")
    if "total" not in loss:
        raise TraceError("loss.total must be declared")
    loss_total = _uint(loss["total"], "loss.total")
    coverage = _mapping(bundle["coverage"], "coverage")
    relation_requirements: set[str] = set()
    if "relation_requirements" in bundle:
        values = _array(bundle["relation_requirements"], "relation_requirements")
        for index, value in enumerate(values):
            value = _id(value, f"relation_requirements[{index}]")
            if value not in RELATION_REQUIREMENTS:
                raise TraceError(f"unknown relation requirement: {value!r}")
            if value in relation_requirements:
                raise TraceError(f"duplicate relation requirement: {value}")
            relation_requirements.add(value)
    relation_diagnostics: dict[str, int] = {}
    if "relation_diagnostics" in bundle:
        raw_diagnostics = _mapping(bundle["relation_diagnostics"], "relation_diagnostics")
        for key, value in raw_diagnostics.items():
            key = _id(key, "relation_diagnostics key")
            if key not in ALL_RELATION_DIAGNOSTICS:
                raise TraceError(f"unknown relation diagnostic: {key!r}")
            relation_diagnostics[key] = _uint(value, f"relation_diagnostics.{key}")
    policy = _mapping(bundle.get("qualification_policy", {}), "qualification_policy")
    reasons = []
    reasons.extend(calibration_issues)
    reasons.extend(reasons_window)
    if loss_total:
        reasons.append("event_loss")
    if clipped:
        reasons.append("unpaired_or_clipped_scopes")
    if not window["closed"]:
        reasons.append("window_open")
    weak_processes = [p["process_id"] for p in processes.values()
                      if p.get("identity_quality") != "qualified" or p.get("incarnation") == "unknown"]
    weak_threads = [t["thread_id"] for t in threads.values()
                    if t.get("identity_quality") != "qualified" or t.get("incarnation") == "unknown"]
    if weak_processes or weak_threads:
        reasons.append("identity_incarnation_unqualified")
    if any(c.get("alignment") != "qualified" for c in clocks.values()):
        reasons.append("clock_mapping_unqualified")
    if any(c.get("kind") in ("monotonic", "gpu") and
           str(c.get("resolution_ns", "")).lower() in ("unknown", "not_applicable", "0")
           for c in clocks.values()):
        reasons.append("clock_resolution_unknown")
    max_clock_error = policy.get("max_clock_error_ns")
    for clock in clocks.values():
        uncertainty = clock.get("mapping", {}).get("error_ns") if clock.get("mapping") else clock.get("uncertainty_ns")
        if str(uncertainty).lower() == "unknown":
            reasons.append("clock_uncertainty_unknown")
            continue
        error_ns = _uint(uncertainty, "clock.uncertainty_ns")
        if require_complete and max_clock_error is None:
            reasons.append("clock_error_threshold_missing")
        elif max_clock_error is not None and error_ns > _uint(max_clock_error, "qualification_policy.max_clock_error_ns"):
            reasons.append("clock_uncertainty_exceeds_policy")
    if not bundle["events"]:
        reasons.append("empty_event_set")
    if require_complete:
        if "relation_requirements" not in bundle:
            reasons.append("relation_requirements_missing")
        if "relation_diagnostics" not in bundle:
            reasons.append("relation_diagnostics_missing")
    if relation_requirements and relation_diagnostics.get("duplicate_source_records", 0):
        reasons.append("relation_diagnostic_nonzero:duplicate_source_records")
    for family in relation_requirements:
        for diagnostic in RELATION_DIAGNOSTICS[family]:
            if diagnostic not in relation_diagnostics:
                if require_complete:
                    reasons.append(f"relation_diagnostic_missing:{diagnostic}")
            elif relation_diagnostics[diagnostic]:
                reasons.append(f"relation_diagnostic_nonzero:{diagnostic}")
    for name, datum in coverage.items():
        row = _mapping(datum, f"coverage.{name}")
        if not {"observed", "population"} <= row.keys():
            raise TraceError(f"coverage.{name} must declare observed and population")
        population = _uint(row["population"], f"coverage.{name}.population")
        observed = _uint(row["observed"], f"coverage.{name}.observed")
        if observed > population:
            raise TraceError(f"coverage exceeds population: {name}")
        if observed != population:
            reasons.append(f"coverage_incomplete:{name}")
    qualified = not reasons
    if require_complete and not bundle["inputs"]:
        reasons.append("source_digest_missing")
        qualified = False
    result = {"verdict": "qualified" if qualified else "unqualified",
              "reasons": sorted(set(reasons)), "events": len(events),
              "processes": len(processes), "threads": len(threads),
              "relations": len(bundle["relations"]), "scopes": len(scopes),
              "clipped_scopes": clipped, "loss": loss_total,
              "window_closed": window["closed"]}
    if require_complete and (not qualified or not coverage):
        if not coverage:
            result["reasons"].append("coverage_not_declared")
        raise TraceError("complete-attribution requirement failed: " + ",".join(result["reasons"]))
    return result


def _from_runtime_events(source: dict[str, Any], *, producer: str) -> dict[str, Any]:
    """Map a runtime's JSON object representation into the canonical table shape."""
    output = dict(source)
    output["schema"] = SCHEMA
    output["producer"] = producer
    output.setdefault("inputs", [])
    output.setdefault("processes", [])
    output.setdefault("threads", [])
    output.setdefault("clocks", [])
    output.setdefault("relations", [])
    output.setdefault("coverage", {})
    output.setdefault("loss", {"total": "0"})
    output.setdefault("window", {"closed": False})
    output.setdefault("run_id", "unknown")
    output["window"] = dict(output.get("window", {}))
    if "start" not in output["window"] and "start_ns" in output["window"]:
        output["window"]["start"] = str(output["window"].pop("start_ns"))
    if "end" not in output["window"] and "end_ns" in output["window"]:
        output["window"]["end"] = str(output["window"].pop("end_ns"))
    for process in output["processes"]:
        if isinstance(process, dict) and process.get("pid") is not None:
            process["pid"] = str(process["pid"])
        if isinstance(process, dict) and process.get("process_id") is not None:
            process["process_id"] = str(process["process_id"])
    for thread in output["threads"]:
        if not isinstance(thread, dict):
            continue
        for field in ("thread_id", "process_id", "native_tid", "incarnation",
                      "guest_tid", "creation_time100ns"):
            if thread.get(field) is not None:
                thread[field] = str(thread[field])
        if thread.get("guest_process_id") is not None:
            thread["guest_process_id"] = str(thread["guest_process_id"])
    for process in output["processes"]:
        if not isinstance(process, dict):
            continue
        for field in ("guest_process_id", "creation_time100ns"):
            if process.get(field) is not None:
                process[field] = str(process[field])
    for clock in output["clocks"]:
        if not isinstance(clock, dict):
            continue
        if clock.get("clock_id") is not None:
            clock["clock_id"] = str(clock["clock_id"])
        if clock.get("kind") == "native_monotonic":
            clock["kind"] = "monotonic"
        for field in ("resolution_ns", "uncertainty_ns"):
            if clock.get(field) is not None:
                clock[field] = str(clock[field])
    result_events = []
    raw_event_refs: dict[str, str | None] = {}
    raw_calibrations = _array(source.get("clock_calibrations", []), "clock_calibrations")
    calibration_by_key = {}
    for index, raw_calibration in enumerate(raw_calibrations):
        calibration = _mapping(raw_calibration, f"clock_calibrations[{index}]")
        source_clock = calibration.get("source_clock_id")
        calibration_id = calibration.get("calibration_id")
        if source_clock is not None and calibration_id is not None:
            calibration_by_key[(str(source_clock), str(calibration_id))] = calibration

    def bind_raw_ref(value: Any, event_id: str) -> None:
        if value is None:
            return
        ref = str(value)
        if ref in raw_event_refs and raw_event_refs[ref] != event_id:
            raw_event_refs[ref] = None
        else:
            raw_event_refs[ref] = event_id

    for index, raw in enumerate(_array(source.get("events", []), "events")):
        event = _mapping(raw, f"events[{index}]")
        phase = event.get("phase")
        if phase not in ("instant", "begin", "end", "counter"):
            raise TraceError(f"unknown runtime event phase: {phase!r}")
        kind = {"begin": "scope_begin", "end": "scope_end"}.get(phase, phase)
        timestamp = event.get("timestamp_ns", event.get("timestamp"))
        if event.get("source_order") is None:
            raise TraceError(f"events[{index}].source_order is required")
        source_order = str(event["source_order"])
        collector = event.get("collector", source.get("collector", "engine"))
        attrs = dict(event.get("attrs", event.get("fields", {})))
        # Preserve the runtime's typed binding inputs as opaque values. They
        # are serialized as annotations by the native Perfetto exporter.
        for raw_key, canonical_key in (("arg0", "owner_id"), ("arg1", "related_id"),
                                       ("event_id", "event_code"),
                                       ("generation", "generation"),
                                       ("uncertainty_ns", "uncertainty_ns"),
                                       ("clock_frequency", "clock_frequency"),
                                       ("source_clock_id", "source_clock_id"),
                                       ("source_timestamp", "source_timestamp"),
                                       ("source_timestamp_unit", "source_timestamp_unit"),
                                       ("source_api", "source_api"),
                                       ("guest_process_id", "guest_process_id"),
                                       ("guest_thread_id", "guest_thread_id"),
                                       ("process_create_time100ns", "process_create_time100ns"),
                                       ("thread_create_time100ns", "thread_create_time100ns"),
                                       ("calibration_id", "calibration_id"),
                                       ("calibration_scale", "calibration_scale"),
                                       ("calibration_offset", "calibration_offset"),
                                       ("calibration_source_first", "calibration_source_first"),
                                       ("calibration_host_first_ns", "calibration_host_first_ns"),
                                       ("calibration_resolution_ns", "calibration_resolution_ns"),
                                       ("calibration_source_second", "calibration_source_second"),
                                       ("calibration_host_second_ns", "calibration_host_second_ns"),
                                       ("calibration_valid_begin", "calibration_valid_begin"),
                                       ("calibration_valid_end", "calibration_valid_end"),
                                       ("relation_queue_identity", "relation_queue_identity"),
                                       ("queue_id", "queue_id"),
                                       ("device_generation", "device_generation"),
                                       ("lane_id", "lane_id"),
                                       ("binding_kind", "binding_kind"),
                                       ("relation_type", "relation_type"),
                                       ("relation_evidence", "relation_evidence"),
                                       ("relation_source_id", "relation_source_id"),
                                       ("relation_target_id", "relation_target_id")):
            raw_value = event.get(raw_key, attrs.get(raw_key))
            if raw_value is not None:
                attrs[canonical_key] = str(raw_value)
        calibration_ref = calibration_by_key.get((
            str(event.get("source_clock_id", attrs.get("source_clock_id", ""))),
            str(event.get("calibration_id", attrs.get("calibration_id", "")))))
        if calibration_ref is not None:
            attrs.setdefault("calibration_scale", str(calibration_ref.get("scale", "")))
            attrs.setdefault("calibration_offset", str(calibration_ref.get("offset_ns", "")))
        if event.get("native_observation") is not None:
            attrs["native_observation"] = event["native_observation"]
        if event.get("event_type") is not None:
            attrs["event_code"] = str(event["event_type"])
            attrs["event_type"] = str(event["event_type"])
        if event.get("event_id") is not None and not isinstance(event["event_id"], str):
            attrs.setdefault("event_code", str(event["event_id"]))
        raw_id = event.get("trace_event_id", event.get("event_id"))
        if not isinstance(raw_id, str) or not raw_id:
            raw_id = f"{collector}:{source_order}"
        row = {"event_id": raw_id,
               "source_record": str(event.get("source_record", event.get("source_order", index))),
               "source_order": source_order,
               "collector": collector,
               "process_id": str(event.get("process_id")), "thread_id": str(event.get("thread_id")),
               "clock_id": str(event.get("clock_id")), "timestamp": str(timestamp),
               "kind": kind, "attrs": attrs}
        if event.get("track_id") is not None:
            row["attrs"]["source_track_id"] = str(event["track_id"])
            row["track_id"] = str(event["track_id"])
        row["track_kind"] = event.get("track_kind", "thread")
        if event.get("source_order") is not None:
            row["attrs"]["source_order"] = str(event["source_order"])
        if event.get("generation") is not None:
            row["attrs"]["generation"] = str(event["generation"])
        scope_id = event.get("scope_id", attrs.get("scope_id"))
        if scope_id is not None:
            row["scope_id"] = str(scope_id)
            if str(scope_id) not in ("", "0"):
                attrs["scope_id"] = str(scope_id)
        if event.get("work_ids") is not None:
            row["work_ids"] = event["work_ids"]
            if isinstance(event["work_ids"], list) and len(event["work_ids"]) == 1:
                attrs["work_id"] = str(event["work_ids"][0])
        if event.get("source_record") is not None:
            attrs["source_record"] = str(event["source_record"])
        if event.get("name") is not None:
            row["attrs"]["name"] = event["name"]
            if phase in ("begin", "end"):
                row["attrs"]["event_name"] = event["name"]
        if event.get("category") is not None:
            row["attrs"].setdefault("category", event["category"])
        result_events.append(row)
        bind_raw_ref(raw_id, row["event_id"])
        bind_raw_ref(event.get("event_id"), row["event_id"])
        bind_raw_ref(event.get("source_record"), row["event_id"])
    output["events"] = result_events
    relations = _array(source.get("relations", []), "relations")
    normalized_relations = []
    for index, raw_relation in enumerate(relations):
        relation = dict(_mapping(raw_relation, f"relations[{index}]"))
        for endpoint in ("source_event", "target_event", "evidence_event"):
            raw_ref = relation.get(endpoint)
            if raw_ref is not None:
                mapped = raw_event_refs.get(str(raw_ref))
                if mapped is None:
                    # Do not let an unknown raw reference accidentally validate
                    # just because it equals a canonical ID in the output.
                    raise TraceError(f"relation references unknown or ambiguous {endpoint}")
                relation[endpoint] = mapped
        if relation.get("relation_id") is not None:
            relation["relation_id"] = str(relation["relation_id"])
        if relation.get("evidence_record") is not None:
            evidence_ref = str(relation.pop("evidence_record"))
            if evidence_ref not in ("", "0"):
                mapped = raw_event_refs.get(evidence_ref)
                if mapped is None:
                    raise TraceError("relation references unknown or ambiguous evidence_record")
                relation["evidence_event"] = mapped
        for field in ("type", "evidence", "source_id", "target_id"):
            if relation.get(field) is not None:
                relation[field] = str(relation[field])
        normalized_relations.append(relation)
    output["relations"] = normalized_relations
    if "relation_requirements" in source:
        output["relation_requirements"] = [
            _id(value, f"relation_requirements[{index}]")
            for index, value in enumerate(_array(source["relation_requirements"],
                                                 "relation_requirements"))]
    if "relation_diagnostics" in source:
        raw_diagnostics = _mapping(source["relation_diagnostics"], "relation_diagnostics")
        output["relation_diagnostics"] = {
            key: str(_uint(value, f"relation_diagnostics.{key}"))
            for key, value in raw_diagnostics.items()}
    return output


def normalize_bundle(source: dict[str, Any], *, producer: str = "tracing_normalize.py") -> dict[str, Any]:
    """Validate a runtime v1 event object and wrap it in the normalized envelope."""
    _mapping(source, "source")
    if source.get("schema") not in ("dxmt9-trace-events-v1", "dxmt9.trace.raw.v1", SCHEMA):
        raise TraceError(f"unsupported input schema: {source.get('schema')!r}")
    if source.get("schema") == SCHEMA:
        validate_bundle(source)
        return source
    output = _from_runtime_events(source, producer=producer)
    output["qualification"] = validate_bundle(output)
    return output


def adapt_runtime_jsonl(text: str, *, producer: str = "tracing_normalize.py") -> dict[str, Any]:
    lines = [line for line in text.splitlines() if line.strip()]
    if not lines:
        raise TraceError("empty runtime trace JSONL")
    try:
        metadata = json.loads(lines[0])
        records = [json.loads(line) for line in lines[1:]]
    except json.JSONDecodeError as exc:
        raise TraceError(f"invalid runtime JSONL at line {exc.lineno}") from exc
    if metadata.get("schema") != "dxmt9.trace.raw.v1":
        raise TraceError("unknown runtime JSONL schema")
    inline_events = metadata.get("events")
    if inline_events is None:
        metadata["events"] = records
    elif not isinstance(inline_events, list):
        raise TraceError("runtime metadata events must be an array")
    elif inline_events and records:
        raise TraceError("runtime JSONL metadata must not inline events when records follow")
    elif records:
        metadata["events"] = records
    return normalize_bundle(metadata, producer=producer)


def _base(run_id: str, producer: str, input_path: Path, input_digest: str,
          process: dict[str, Any], clocks: list[dict[str, Any]], threads: list[dict[str, Any]],
          events: list[dict[str, Any]], *, closed: bool, loss: int,
          coverage: dict[str, Any], window: dict[str, Any], adapter: str) -> dict[str, Any]:
    result = {"schema": SCHEMA, "producer": producer, "stage": "reduce", "domain": "dxmt9",
              "run_id": run_id, "inputs": [{"path": str(input_path), "sha256": input_digest,
                                              "adapter": adapter}],
              "env_snapshot": {}, "validity": "partial" if not closed or loss else "raw",
              "processes": [process], "threads": threads, "clocks": clocks,
              "events": events, "relations": [], "window": window,
              "loss": {"total": str(loss), "source": adapter}, "coverage": coverage}
    result["qualification"] = validate_bundle(result)
    return result


def _event(event_id: str, source: str, process_id: str, thread_id: str,
           clock_id: str, timestamp: int, kind: str, attrs: dict[str, Any],
           *, scope_id: str | None = None) -> dict[str, Any]:
    row = {"event_id": event_id, "source_record": source, "source_order": event_id,
           "collector": "legacy-adapter", "process_id": process_id,
           "thread_id": thread_id, "clock_id": clock_id, "timestamp": str(timestamp),
           "kind": kind, "track_kind": "thread", "attrs": attrs}
    if scope_id is not None:
        row["scope_id"] = scope_id
    return row


def adapt_cpu_supply(text: str, input_path: Path, *, run_id: str,
                     process_incarnation: str = "unknown") -> dict[str, Any]:
    lines = text.splitlines()
    if not lines or not lines[0].startswith("# cpu-supply-v2 "):
        raise TraceError("unknown CPU supply schema; only cpu-supply-v2 is supported")
    header = dict(re.findall(r"([a-z_]+)=([0-9]+)", lines[0]))
    required = {"first", "last", "closed", "overflow", "incomplete", "cpu_clock", "pid"}
    if not required <= header.keys():
        raise TraceError("CPU supply header incomplete")
    if int(header["cpu_clock"]) != 1:
        raise TraceError("unsupported CPU counter clock")
    if len(lines) < 2 or lines[1] != "wall_ns,cpu_ns,tid,owner,id,value,frame,event":
        raise TraceError("unexpected CPU supply columns")
    rows = list(csv.DictReader(io.StringIO("\n".join(lines[1:]))))
    events = []
    process_id = f"legacy-cpu-pid:{header['pid']}"
    thread_ids: set[str] = set()
    for i, raw in enumerate(rows):
        try:
            row = {k: _uint(v, f"cpu_supply[{i}].{k}") for k, v in raw.items()}
            kind = EVENTS[row["event"]]
        except (IndexError, KeyError, TypeError) as exc:
            raise TraceError(f"invalid CPU supply row {i}") from exc
        if not row["tid"] or not row["wall_ns"] or not row["cpu_ns"]:
            raise TraceError(f"missing CPU supply identity/clock in row {i}")
        tid = str(row["tid"])
        thread_ids.add(tid)
        attrs = {"legacy_event": kind, "owner": str(row["owner"]), "id": str(row["id"]),
                 "value": str(row["value"]), "frame": str(row["frame"]),
                 "thread_cpu_counter_ns": str(row["cpu_ns"]),
                 "thread_cpu_kind": "native_thread_cpu_counter"}
        if kind.endswith("_begin"):
            event_kind, scope_id = "scope_begin", f"cpu:{row['owner']}:{row['id']}:{kind[:-6]}"
        elif kind.endswith("_end"):
            event_kind, scope_id = "scope_end", f"cpu:{row['owner']}:{row['id']}:{kind[:-4]}"
        else:
            event_kind, scope_id = "instant", None
        events.append(_event(f"cpu:{i}", f"row:{i}", process_id, f"tid:{tid}",
                             "cpu-supply-wall", row["wall_ns"], event_kind, attrs,
                             scope_id=scope_id))
    last_cpu: dict[str, int] = {}
    for row in sorted((r for r in events), key=lambda e: int(e["timestamp"])):
        tid = row["thread_id"]
        cpu = int(row["attrs"]["thread_cpu_counter_ns"])
        if tid in last_cpu and cpu < last_cpu[tid]:
            raise TraceError("native thread CPU counter regressed")
        last_cpu[tid] = cpu
    first, last = int(header["first"]), int(header["last"])
    return _base(run_id, "legacy-cpu-supply-v2", input_path, _digest(input_path),
                 {"process_id": process_id, "pid": header["pid"],
                  "incarnation": process_incarnation, "identity_quality": "pid_only"},
                 [{"clock_id": "cpu-supply-wall", "unit": "ns", "kind": "monotonic",
                   "api": "clock_gettime(CLOCK_MONOTONIC)", "source": "CLOCK_MONOTONIC",
                   "domain": "host-monotonic-local", "resolution_ns": "unknown",
                   "uncertainty_ns": "unknown",
                   "alignment": "unqualified"}],
                 [{"thread_id": f"tid:{tid}", "process_id": process_id,
                   "native_tid": tid, "incarnation": "unknown", "identity_quality": "tid_only"}
                  for tid in sorted(thread_ids)], events,
                 closed=header["closed"] == "1", loss=int(header["overflow"])+int(header["incomplete"]),
                 coverage={"legacy_rows": {"observed": str(len(rows)), "population": str(len(rows))}},
                 window={"start": None, "end": None, "closed": header["closed"] == "1",
                         "frame_first": header["first"], "frame_last": header["last"]},
                 adapter="cpu-supply-v2")


def adapt_wait_trace(text: str, input_path: Path, *, run_id: str,
                     process_incarnation: str = "unknown") -> dict[str, Any]:
    lines = text.splitlines()
    if not lines or not lines[0].startswith("# wait-trace-v1 "):
        raise TraceError("unknown wait trace schema")
    header = dict(re.findall(r"([a-z_]+)=([0-9]+)", lines[0]))
    required = {"pid", "first", "last", "closed", "overflow", "incomplete", "unknown",
                "owners", "capacity", "mutex_min_ns", "storage", "cv_entries"}
    if not required <= header.keys() or len(lines) < 3:
        raise TraceError("wait trace header/trailer incomplete")
    if (header["closed"] != "1" or int(header["pid"]) <= 0 or
            int(header["first"]) <= 0 or int(header["last"]) < int(header["first"]) or
            int(header["owners"]) > 256 or int(header["capacity"]) != 32768):
        raise TraceError("wait trace closure/identity/capacity is unqualified")
    end = re.fullmatch(r"# end rows=([0-9]+)", lines[-1])
    if not end:
        raise TraceError("wait trace is missing row-count trailer")
    rows = list(csv.DictReader(io.StringIO("\n".join(lines[1:-1]))))
    if len(rows) != int(end[1]):
        raise TraceError("wait trace row count mismatch")
    events = []
    process_id = f"legacy-wait-pid:{header['pid']}"
    threads = {}
    for i, raw in enumerate(rows):
        try:
            row = {k: _uint(v, f"wait[{i}].{k}") for k, v in raw.items()}
            site = WAIT_SITES[row["site"]]
        except (KeyError, TypeError) as exc:
            raise TraceError(f"invalid wait trace row {i}") from exc
        if not row["tid"] or not row["begin_ns"] or not row["begin_ns"] <= row["acquired_ns"] <= row["end_ns"]:
            raise TraceError(f"invalid wait clocks in row {i}")
        if row["site"] == 4:
            if not row["queue"] or row["waits"]:
                raise TraceError("invalid query queue acquisition")
        elif (not row["owner"] or not row["target"] or
              (row["waits"] and (row["state_before"] != 0 or row["state_after"] == 0))):
            raise TraceError("invalid replay owner or wait settlement")
        if row["site"] == 0 and row["raw_before"] != row["raw_after"]:
            raise TraceError("query receipt changed its captured prefix")
        tid = str(row["tid"])
        threads[tid] = {"thread_id": f"tid:{tid}", "process_id": process_id,
                        "native_tid": tid, "incarnation": "unknown", "identity_quality": "tid_only"}
        scope = f"wait:{i}"
        attrs = {k: str(v) for k, v in row.items()}
        attrs.update(site_name=site, duration_kind="elapsed_wait_or_acquisition",
                     interpretation="hooked_site_elapsed_not_cpu")
        events.append(_event(f"wait:{i}:begin", f"row:{i}", process_id, f"tid:{tid}",
                             "wait-steady-clock", row["begin_ns"], "scope_begin", attrs,
                             scope_id=scope))
        events.append(_event(f"wait:{i}:end", f"row:{i}", process_id, f"tid:{tid}",
                             "wait-steady-clock", row["end_ns"], "scope_end", attrs,
                             scope_id=scope))
        if row["acquired_ns"] != row["begin_ns"]:
            events.append(_event(f"wait:{i}:acquired", f"row:{i}", process_id, f"tid:{tid}",
                                 "wait-steady-clock", row["acquired_ns"], "instant",
                                 {"site_name": site, "milestone": "mutex_acquired",
                                  "elapsed_ns": str(row["acquired_ns"]-row["begin_ns"])}))
    if sum(int(row["waits"]) for row in rows) != int(header["cv_entries"]):
        raise TraceError("wait trace CV entry count mismatch")
    loss = sum(int(header[k]) for k in ("overflow", "incomplete", "unknown"))
    loss += int(header["storage"] != "1") + int(header["closed"] != "1")
    return _base(run_id, "legacy-wait-trace-v1", input_path, _digest(input_path),
                 {"process_id": process_id, "pid": header["pid"],
                  "incarnation": process_incarnation, "identity_quality": "pid_only"},
                 [{"clock_id": "wait-steady-clock", "unit": "ns", "kind": "monotonic",
                   "api": "std::chrono::steady_clock", "source": "std::chrono::steady_clock",
                   "domain": "host-steady-local", "resolution_ns": "unknown",
                   "uncertainty_ns": "unknown",
                   "alignment": "unqualified"}],
                 list(threads.values()), events, closed=header["closed"] == "1", loss=loss,
                 coverage={"captured_rows": {"observed": str(len(rows)),
                                              "population": str(len(rows))}},
                 window={"start": None, "end": None, "closed": header["closed"] == "1",
                         "frame_first": header["first"], "frame_last": header["last"]},
                 adapter="wait-trace-v1")


def adapt_pe_sampler(text: str, input_path: Path, *, run_id: str) -> dict[str, Any]:
    """Retain PE sampler logs as frame-anchored cumulative sample counters."""
    groups = []
    current: dict[str, Any] | None = None
    frame = None
    for line_no, line in enumerate(text.splitlines()):
        match = re.search(r"\[dxmt9-perf-frame frame=(\d+)", line)
        if match:
            frame = match.group(1)
        if "[dxmt9-pe-sampler]" not in line:
            continue
        if re.search(r"\bpresents=\d+.*\bsamples=", line):
            current = {"source_line": line_no, "frame_anchor": frame, "fields": [],
                       "modules": [], "selfpc": [],
                       "sample_total": re.search(r"\bsamples=(\d+)", line).group(1)}
            groups.append(current)
        if current is not None:
            current["fields"].append(line)
            module = re.search(r"\bmodule=(\S+)\s+samples=(\d+)", line)
            if module:
                current["modules"].append((module[1], int(module[2])))
            pc = re.search(r"\bselfpc(?:_module)?=(\S+).*?\bsamples=(\d+)", line)
            if pc:
                current["selfpc"].append((pc[1], int(pc[2])))
            if "selfpc_summary" in line:
                current["summary"] = line
    if not groups:
        raise TraceError("no PE sampler groups found")
    events = []
    for gi, group in enumerate(groups):
        # Cumulative population deltas are intentionally not synthesized here:
        # the existing consumer requires stable module membership and anchors.
        total = group["sample_total"]
        if total is None or not total.isdigit():
            raise TraceError("PE sampler group lacks sample population")
        events.append(_event(f"pe:{gi}", f"line:{group['source_line']}", "pe-process:unknown",
                             "pe-thread:unknown", "pe-sample-count", gi, "counter",
                             {"sample_population": total, "counter_kind": "cumulative_guest_pc_samples",
                              "frame_anchor": group["frame_anchor"] or "unknown",
                              "module_counts": [{"module": n, "samples": str(v)}
                                                for n, v in group["modules"]],
                              "selfpc_counts": [{"bucket": n, "samples": str(v)}
                                                for n, v in group["selfpc"]],
                              "qualification": "snapshot_counter_not_elapsed_or_thread_cpu"}))
    return _base(run_id, "legacy-pe-sampler-log", input_path, _digest(input_path),
                 {"process_id": "pe-process:unknown", "pid": "unknown",
                  "incarnation": "unknown", "identity_quality": "unavailable"},
                 [{"clock_id": "pe-sample-count", "unit": "count", "kind": "sample_count",
                   "api": "DXMT9_PE_THREAD_SAMPLER", "source": "cumulative guest PC reports",
                   "domain": "sample-population", "resolution_ns": "not_applicable",
                   "uncertainty_ns": "unknown",
                   "alignment": "frame-anchor-only"}],
                 [{"thread_id": "pe-thread:unknown", "process_id": "pe-process:unknown",
                   "native_tid": "unknown", "incarnation": "unknown",
                   "identity_quality": "guest-thread-not-mapped"}], events,
                 closed=True, loss=0,
                 coverage={"sampler_groups": {"observed": str(len(groups)),
                                                "population": str(len(groups))}},
                 window={"start": None, "end": None, "closed": True,
                         "semantics": "log-frame-anchored-snapshots"},
                 adapter="pe-sampler-log")


def adapt(path: Path, fmt: str, *, run_id: str, process_incarnation: str) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    if fmt == "auto":
        if text.startswith("# cpu-supply-v2 "):
            fmt = "cpu-supply-v2"
        elif text.startswith("# wait-trace-v1 "):
            fmt = "wait-trace-v1"
        elif "[dxmt9-pe-sampler]" in text:
            fmt = "pe-sampler-log"
        else:
            try:
                if text.lstrip().startswith('{') and '"schema":"dxmt9.trace.raw.v1"' in text.splitlines()[0].replace(" ", ""):
                    result = adapt_runtime_jsonl(text)
                else:
                    result = normalize_bundle(json.loads(text))
            except json.JSONDecodeError as exc:
                result = adapt_runtime_jsonl(text)
            fmt = "dxmt9-v1"
    if fmt == "cpu-supply-v2":
        result = adapt_cpu_supply(text, path, run_id=run_id,
                                  process_incarnation=process_incarnation)
    elif fmt == "wait-trace-v1":
        result = adapt_wait_trace(text, path, run_id=run_id,
                                  process_incarnation=process_incarnation)
    elif fmt == "pe-sampler-log":
        result = adapt_pe_sampler(text, path, run_id=run_id)
    elif fmt == "dxmt9-v1":
        try:
            result = normalize_bundle(json.loads(text))
        except json.JSONDecodeError:
            result = adapt_runtime_jsonl(text)
    else:
        raise TraceError(f"unsupported format: {fmt}")
    input_ref = {"path": str(path), "sha256": _digest(path), "adapter": fmt}
    refs = result.get("inputs", [])
    if not isinstance(refs, list):
        raise TraceError("bundle inputs must be an array")
    if not any(isinstance(ref, dict) and ref.get("sha256") == input_ref["sha256"] for ref in refs):
        result["inputs"] = [input_ref, *refs]
    validate_bundle(result)
    return result


def chrome_trace(bundle: dict[str, Any]) -> dict[str, Any]:
    """Explicit compatibility adapter to Chrome Trace Event JSON.

    Export only the declared primary clock. Scope, instant, counter, and flow
    records retain distinct event phases. Ambiguous clocks are rejected.
    """
    validate_bundle(bundle)
    clocks = {c["clock_id"]: c for c in bundle["clocks"]}
    timeline_clocks = [c for c in clocks.values() if c["kind"] == "monotonic"]
    if len(timeline_clocks) != 1 or timeline_clocks[0].get("alignment") == "unqualified":
        raise TraceError("Chrome export requires exactly one qualified monotonic clock")
    primary = timeline_clocks[0]["clock_id"]
    if any(not str(p.get("pid", "")).isdigit() for p in bundle["processes"]):
        raise TraceError("Chrome export requires numeric source process IDs")
    if any(not str(t.get("native_tid", "")).isdigit() for t in bundle["threads"]):
        raise TraceError("Chrome export requires numeric source thread IDs")
    # Export IDs are explicitly bijective over incarnations, so OS PID/TID reuse
    # cannot merge two lifetimes in Chrome's (pid, tid) track key.
    process_keys = sorted(p["process_id"] for p in bundle["processes"])
    thread_keys = sorted(t["thread_id"] for t in bundle["threads"])
    pids = {key: i + 1 for i, key in enumerate(process_keys)}
    tids = {key: i + 1 for i, key in enumerate(thread_keys)}
    processes = {p["process_id"]: p for p in bundle["processes"]}
    threads = {t["thread_id"]: t for t in bundle["threads"]}
    entries = []
    for e in bundle["events"]:
        if e["clock_id"] != primary:
            continue
        ph = {"scope_begin": "B", "scope_end": "E", "instant": "i", "counter": "C"}.get(e["kind"])
        if not ph:
            raise TraceError(f"Chrome adapter does not support event kind {e['kind']!r}")
        attrs = dict(e.get("attrs", {}))
        attrs["source_pid"] = processes[e["process_id"]]["pid"]
        attrs["process_incarnation"] = processes[e["process_id"]]["incarnation"]
        attrs["source_tid"] = threads[e["thread_id"]]["native_tid"]
        attrs["thread_incarnation"] = threads[e["thread_id"]]["incarnation"]
        row = {"name": attrs.get("name", e["kind"]), "ph": ph,
               "ts": int(e["timestamp"]) / 1000, "pid": pids[e["process_id"]],
               "tid": tids[e["thread_id"]], "args": attrs}
        if ph in ("B", "E"):
            row["cat"] = e.get("attrs", {}).get("category", "dxmt9")
        if ph == "i":
            row["s"] = "t"
        if ph == "C":
            row["args"].setdefault("value", e.get("value"))
        entries.append(row)
    event_index = {e["event_id"]: e for e in bundle["events"]}
    for rel in bundle["relations"]:
        if rel["type"] != "flow":
            continue
        source, target = event_index[rel["source_event"]], event_index[rel["target_event"]]
        if source["clock_id"] != primary or target["clock_id"] != primary:
            continue
        for e, phase in ((source, "s"), (target, "f")):
            entries.append({"name": rel["relation_id"], "cat": "dxmt9.flow", "ph": phase,
                            "ts": int(e["timestamp"]) / 1000,
                            "pid": pids[e["process_id"]], "tid": tids[e["thread_id"]],
                            "id": rel["relation_id"], "bp": "e", "args": {"evidence": rel["evidence"]}})
    return {"traceEvents": entries, "displayTimeUnit": "ns",
            "metadata": {"adapter": "dxmt9-chrome-trace-v1", "run_id": bundle["run_id"],
                         "source_schema": SCHEMA,
                         "process_tracks": [{"export_pid": pids[p["process_id"]], **p}
                                            for p in bundle["processes"]],
                         "thread_tracks": [{"export_tid": tids[t["thread_id"]], **t}
                                           for t in bundle["threads"]],
                         "relations": bundle["relations"],
                         "clocks": bundle["clocks"], "loss": bundle["loss"],
                         "coverage": bundle["coverage"]}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--format", choices=("auto", "dxmt9-v1", "cpu-supply-v2",
                                               "wait-trace-v1", "pe-sampler-log"), default="auto")
    parser.add_argument("--run-id", default="legacy-unbound")
    parser.add_argument("--process-incarnation", default="unknown")
    parser.add_argument("--require-complete", action="store_true")
    parser.add_argument("--chrome-output", type=Path)
    parser.add_argument("--envelope", type=Path)
    args = parser.parse_args()
    try:
        bundle = adapt(args.input, args.format, run_id=args.run_id,
                       process_incarnation=args.process_incarnation)
        if args.envelope:
            envelope = json.loads(args.envelope.read_text(encoding="utf-8"))
            for key in ("schema", "producer", "stage", "domain", "inputs", "env_snapshot", "validity"):
                if key in envelope:
                    bundle[key] = envelope[key]
        refs = bundle.get("inputs", [])
        digest = _digest(args.input)
        if not any(isinstance(ref, dict) and ref.get("sha256") == digest for ref in refs):
            bundle["inputs"] = [{"path": str(args.input), "sha256": digest,
                                 "adapter": args.format}, *refs]
        qualification = validate_bundle(bundle, require_complete=args.require_complete)
        bundle["qualification"] = qualification
        encoded = json.dumps(bundle, indent=2, sort_keys=True) + "\n"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.output.with_name(args.output.name + ".tmp")
        tmp.write_text(encoded, encoding="utf-8")
        tmp.replace(args.output)
        if args.chrome_output:
            args.chrome_output.write_text(json.dumps(chrome_trace(bundle), indent=2) + "\n",
                                          encoding="utf-8")
        print(json.dumps(qualification, sort_keys=True))
        return 0
    except (OSError, json.JSONDecodeError, TraceError, TypeError) as exc:
        print(f"tracing normalization failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
