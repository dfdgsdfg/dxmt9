#!/usr/bin/env python3
"""Run pinned native Perfetto Trace Processor qualification queries."""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import io
import math
import json
import platform
import subprocess
import urllib.request
import sys
from pathlib import Path
from typing import Any

LOCK_PATH = Path(__file__).with_name("tracing_toolchain.lock.json")


class ConsumerError(ValueError):
    pass


def _annotation_rows_match(expected: list[dict[str, Any]],
                           actual: list[dict[str, Any]]) -> bool:
    """Compare rows exactly, allowing only Perfetto's float-string rounding.

    Trace Processor serializes numeric debug annotations through SQLite's
    floating-point text formatter. Calibration scale/offset are the only
    floating-point annotations; timestamps, identities, integer values, keys,
    row order, and all other strings remain exact.
    """
    if len(expected) != len(actual):
        return False
    float_keys = {"debug.calibration_scale", "debug.calibration_offset",
                  "debug.begin_calibration_scale", "debug.begin_calibration_offset",
                  "debug.end_calibration_scale", "debug.end_calibration_offset"}
    for left, right in zip(expected, actual):
        if left.keys() != right.keys():
            return False
        for key in left:
            if key == "value" and left.get("key") in float_keys:
                try:
                    a, b = float(left[key]), float(right[key])
                except (TypeError, ValueError):
                    return False
                if not math.isfinite(a) or not math.isfinite(b):
                    return False
                if not math.isclose(a, b, rel_tol=1e-14, abs_tol=0.0):
                    return False
            elif left[key] != right[key]:
                return False
    return True


def load_lock() -> dict[str, Any]:
    return json.loads(LOCK_PATH.read_text(encoding="utf-8"))


def _platform_key() -> str:
    os_name = {"Darwin": "darwin", "Linux": "linux"}.get(platform.system())
    arch = {"arm64": "arm64", "aarch64": "aarch64", "x86_64": "x86_64",
            "AMD64": "x86_64"}.get(platform.machine())
    if not os_name or not arch:
        raise ConsumerError(f"unsupported Trace Processor host: {platform.system()}-{platform.machine()}")
    return f"{os_name}-{arch}"


def resolve_pinned_processor(cache_dir: Path | None = None) -> tuple[Path, dict[str, Any]]:
    lock = load_lock()
    key = _platform_key()
    try:
        pin = lock["prebuilts"][key]
    except KeyError as exc:
        raise ConsumerError(f"no pinned Trace Processor for {key}") from exc
    if cache_dir is None:
        cache_dir = Path.home() / ".cache" / "dxmt9-tracing" / lock["version"]
    cache_dir.mkdir(parents=True, exist_ok=True)
    binary = cache_dir / ("trace_processor_shell-" + pin["sha256"][:16])
    if binary.exists():
        if _sha256(binary) != pin["sha256"]:
            binary.unlink()
        else:
            _validate_binary(binary, lock)
            return binary, lock
    with urllib.request.urlopen(pin["url"], timeout=60) as response:
        payload = response.read()
    if len(payload) != pin["size"]:
        raise ConsumerError(f"Trace Processor size mismatch: got {len(payload)}, expected {pin['size']}")
    if hashlib.sha256(payload).hexdigest() != pin["sha256"]:
        raise ConsumerError("Trace Processor SHA-256 mismatch")
    temp = binary.with_suffix(".tmp")
    temp.write_bytes(payload)
    temp.chmod(0o755)
    temp.replace(binary)
    _validate_binary(binary, lock)
    return binary, lock


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _validate_binary(binary: Path, lock: dict[str, Any]) -> str:
    key = _platform_key()
    expected = lock["prebuilts"][key]["sha256"]
    actual = _sha256(binary)
    if actual != expected:
        raise ConsumerError(f"Trace Processor executable digest mismatch: {actual}")
    try:
        result = subprocess.run([str(binary), "--version"], check=True, text=True,
                                capture_output=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        raise ConsumerError(f"pinned Trace Processor is not executable: {exc}") from exc
    version = result.stdout.strip()
    if not version.startswith(f"Perfetto {lock['version']}-"):
        raise ConsumerError(f"Trace Processor version mismatch: {version}")
    return version


QUERY = """
SELECT
  (SELECT COUNT(*) FROM slice) AS slice_count,
  (SELECT COUNT(*) FROM slice WHERE dur >= 0) AS complete_slice_count,
  (SELECT COUNT(*) FROM slice WHERE dur < 0) AS open_slice_count,
  (SELECT COALESCE(MIN(ts), 0) FROM slice) AS min_ts,
  (SELECT COALESCE(MAX(ts + CASE WHEN dur > 0 THEN dur ELSE 0 END), 0) FROM slice) AS max_ts,
  (SELECT COALESCE(SUM(CASE WHEN dur > 0 THEN dur ELSE 0 END), 0) FROM slice) AS total_duration_ns,
  (SELECT COUNT(*) FROM counter) AS counter_count,
  (SELECT COUNT(*) FROM flow) AS flow_count,
  (SELECT COUNT(*) FROM flow WHERE slice_out IS NULL OR slice_in IS NULL) AS incomplete_flow_count,
  (SELECT COUNT(DISTINCT track_id) FROM slice) AS slice_track_count,
  (SELECT COUNT(*) FROM thread_track) AS thread_track_count,
  (SELECT COUNT(*) FROM process_track) AS process_track_count,
  (SELECT COUNT(*) FROM slice WHERE depth > 0) AS nested_slice_count
"""

SLICE_ROWS_QUERY = """
SELECT s.ts, s.dur, s.name, s.depth,
       CASE WHEN tt.id IS NOT NULL THEN 'thread'
            WHEN pt.id IS NOT NULL THEN 'process' ELSE 'other' END AS track_kind,
       th.tid AS tid, COALESCE(p_thread.pid, p_process.pid) AS pid
FROM slice s
LEFT JOIN thread_track tt ON tt.id = s.track_id
LEFT JOIN thread th ON th.utid = tt.utid
LEFT JOIN process p_thread ON p_thread.upid = th.upid
LEFT JOIN process_track pt ON pt.id = s.track_id
LEFT JOIN process p_process ON p_process.upid = pt.upid
ORDER BY s.ts, s.dur, s.name, track_kind, pid, tid
"""

FLOW_ROWS_QUERY = """
SELECT o.ts AS out_ts, o.name AS out_name, ot.tid AS out_tid,
       COALESCE(op_thread.pid, op_process.pid) AS out_pid,
       i.ts AS in_ts, i.name AS in_name, it.tid AS in_tid,
       COALESCE(ip_thread.pid, ip_process.pid) AS in_pid
FROM flow f
JOIN slice o ON o.id = f.slice_out
JOIN slice i ON i.id = f.slice_in
LEFT JOIN thread_track ott ON ott.id = o.track_id
LEFT JOIN thread ot ON ot.utid = ott.utid
LEFT JOIN process op_thread ON op_thread.upid = ot.upid
LEFT JOIN process_track opt ON opt.id = o.track_id
LEFT JOIN process op_process ON op_process.upid = opt.upid
LEFT JOIN thread_track itt ON itt.id = i.track_id
LEFT JOIN thread it ON it.utid = itt.utid
LEFT JOIN process ip_thread ON ip_thread.upid = it.upid
LEFT JOIN process_track ipt ON ipt.id = i.track_id
LEFT JOIN process ip_process ON ip_process.upid = ipt.upid
ORDER BY out_ts, in_ts, out_name, in_name
"""

COUNTER_ROWS_QUERY = """
SELECT c.ts, c.value, tr.name AS track_name
FROM counter c JOIN track tr ON tr.id = c.track_id
ORDER BY c.ts, c.value, tr.name
"""

PROCESS_TRACK_IDS_QUERY = """
SELECT a.int_value AS track_uuid
FROM track t JOIN args a ON a.arg_set_id = t.source_arg_set_id
WHERE t.type = 'process_track_event' AND a.key = 'trace_id'
ORDER BY track_uuid
"""

ANNOTATION_ROWS_QUERY = """
SELECT s.ts, s.dur, s.name,
       CASE WHEN tt.id IS NOT NULL THEN 'thread'
            WHEN pt.id IS NOT NULL THEN 'process' ELSE 'other' END AS track_kind,
       th.tid AS tid, COALESCE(p_thread.pid, p_process.pid) AS pid,
       a.key, a.display_value AS value
FROM slice s JOIN args a ON a.arg_set_id = s.arg_set_id
LEFT JOIN thread_track tt ON tt.id = s.track_id
LEFT JOIN thread th ON th.utid = tt.utid
LEFT JOIN process p_thread ON p_thread.upid = th.upid
LEFT JOIN process_track pt ON pt.id = s.track_id
LEFT JOIN process p_process ON p_process.upid = pt.upid
WHERE a.key GLOB 'debug.*'
ORDER BY s.ts, s.dur, s.name, track_kind, pid, tid, a.key, value
"""

DIAGNOSTIC_QUERY = """
SELECT name, severity, source, value FROM stats
WHERE value != 0 AND (
  severity IN ('warning', 'error', 'data_loss') OR name LIKE '%loss%' OR
  name LIKE '%drop%' OR name LIKE '%invalid%'
)
ORDER BY name
"""


def _run_query(binary: Path, trace: Path, query: str) -> str:
    try:
        result = subprocess.run([str(binary), "query", str(trace), query], check=True,
                                text=True, capture_output=True, timeout=180)
    except subprocess.CalledProcessError as exc:
        raise ConsumerError("Trace Processor rejected trace: " + (exc.stderr.strip() or str(exc))) from exc
    except (OSError, subprocess.SubprocessError) as exc:
        raise ConsumerError(f"Trace Processor query failed: {exc}") from exc
    return result.stdout


def _first_csv(stdout: str) -> dict[str, str]:
    rows = list(csv.DictReader(io.StringIO(stdout.strip())))
    if len(rows) != 1:
        raise ConsumerError(f"unexpected Trace Processor query result: {stdout!r}")
    return rows[0]


def _csv_rows(stdout: str) -> list[dict[str, str]]:
    return list(csv.DictReader(io.StringIO(stdout.strip())))


def derive_expected(bundle: dict[str, Any]) -> dict[str, Any]:
    """Derive exact native SQL expectations from a normalized source bundle."""
    processes = {p["process_id"]: p for p in bundle["processes"]}
    threads = {t["thread_id"]: t for t in bundle["threads"]}
    clocks = {c["clock_id"]: c for c in bundle["clocks"]}
    target_clock = bundle.get("window", {}).get("clock_id")
    if target_clock not in clocks:
        raise ConsumerError("native query expectations require an explicit window clock")

    def mapped_ts(event: dict[str, Any]) -> int:
        timestamp = int(event["timestamp"])
        clock = clocks[event["clock_id"]]
        if event["clock_id"] == target_clock:
            return timestamp
        mapping = clock.get("mapping")
        if not mapping or mapping.get("host_clock_id") != target_clock:
            raise ConsumerError("native query expectations require clocks mapped to the window clock")
        scale_num, scale_den = int(mapping["scale_num"]), int(mapping["scale_den"])
        if not scale_num or not scale_den:
            raise ConsumerError("invalid rational clock transform")
        if not int(mapping["valid_from"]) <= timestamp <= int(mapping["valid_to"]):
            raise ConsumerError("event timestamp lies outside clock calibration")
        return (timestamp * scale_num + scale_den // 2) // scale_den + int(mapping["offset_ns"])

    scopes: dict[tuple[str, str], dict[str, dict[str, Any]]] = {}
    slice_rows: list[dict[str, Any]] = []
    event_slice: dict[str, dict[str, Any]] = {}
    counters: list[dict[str, Any]] = []
    annotation_rows: list[dict[str, Any]] = []
    annotation_fields = {
        "owner_id": "debug.owner_id", "work_id": "debug.work_id",
        "related_id": "debug.related_id", "source_record": "debug.source_record",
        "scope_id": "debug.scope_id", "uncertainty_ns": "debug.uncertainty_ns",
        "clock_frequency": "debug.clock_frequency",
        "source_clock_id": "debug.source_clock_id",
        "source_timestamp": "debug.source_timestamp",
        "source_api": "debug.source_api",
        "source_timestamp_unit": "debug.source_timestamp_unit",
        "event_type": "debug.event_type",
        "event_name": "debug.event_name",
        "guest_process_id": "debug.guest_process_id",
        "guest_thread_id": "debug.guest_thread_id",
        "process_create_time100ns": "debug.process_create_time100ns",
        "thread_create_time100ns": "debug.thread_create_time100ns",
        "calibration_id": "debug.calibration_id",
        "calibration_scale": "debug.calibration_scale",
        "calibration_offset": "debug.calibration_offset",
        "calibration_source_first": "debug.calibration_source_first",
        "calibration_host_first_ns": "debug.calibration_host_first_ns",
        "calibration_source_second": "debug.calibration_source_second",
        "calibration_host_second_ns": "debug.calibration_host_second_ns",
        "calibration_resolution_ns": "debug.calibration_resolution_ns",
        "calibration_valid_begin": "debug.calibration_valid_begin",
        "calibration_valid_end": "debug.calibration_valid_end",
        "relation_type": "debug.relation_type",
        "relation_evidence": "debug.relation_evidence",
        "relation_source_id": "debug.relation_source_id",
        "relation_target_id": "debug.relation_target_id",
        "binding_kind": "debug.binding_kind",
        "relation_queue_identity": "debug.relation_queue_identity",
        "queue_id": "debug.queue_id",
        "device_generation": "debug.device_generation",
        "lane_id": "debug.lane_id"}
    include_zero_annotations = {"source_record", "relation_source_id",
                                "calibration_offset", "calibration_source_first",
                                "calibration_host_first_ns", "calibration_source_second",
                                "calibration_host_second_ns"}

    def collect_annotations(event: dict[str, Any], row: dict[str, Any],
                            endpoint: str = "") -> None:
        attrs = event.get("attrs", {})
        for field, key in annotation_fields.items():
            value = event.get("source_record") if field == "source_record" else attrs.get(field)
            if value is None or str(value) == "":
                continue
            if str(value) == "0" and field not in include_zero_annotations:
                continue
            if field in ("calibration_scale", "calibration_offset"):
                try:
                    numeric = float(str(value))
                except ValueError as exc:
                    raise ConsumerError(f"{field} annotation must be numeric") from exc
                value = str(numeric)
            annotation_rows.append({"ts": row["ts"], "dur": row["dur"],
                "name": row["name"], "track_kind": row["track_kind"],
                "tid": row["tid"], "pid": row["pid"],
                "key": "debug." + endpoint + field, "value": str(value)})

    for event in bundle["events"]:
        attrs = event.get("attrs", {})
        track_kind = event.get("track_kind", "thread")
        if track_kind not in ("thread", "process", "gpu"):
            raise ConsumerError("exact native query checking requires thread/process/GPU tracks")
        pid = int(processes[event["process_id"]]["pid"])
        tid = int(threads[event["thread_id"]]["native_tid"]) if track_kind == "thread" else None
        query_track_kind = "thread" if track_kind == "thread" else "process"
        name = str(attrs.get("name", attrs.get("event_name", event["kind"])))
        if event["kind"] in ("scope_begin", "scope_end"):
            key = (event["process_id"], event["scope_id"])
            pair = scopes.setdefault(key, {})
            edge = "begin" if event["kind"] == "scope_begin" else "end"
            if edge in pair:
                raise ConsumerError("duplicate normalized scope endpoint")
            pair[edge] = event
        elif event["kind"] == "instant":
            row = {"ts": mapped_ts(event), "dur": 0, "name": name,
                   "depth": 0, "track_kind": query_track_kind, "tid": tid, "pid": pid}
            slice_rows.append(row)
            event_slice[event["event_id"]] = row
            collect_annotations(event, row)
        elif event["kind"] == "counter":
            value = attrs.get("counter_value", attrs.get("value"))
            if value is None:
                raise ConsumerError("counter event lacks an exact counter value")
            counters.append({"ts": mapped_ts(event), "value": int(str(value)),
                             "track_name": str(attrs.get("track_name", name))})
        else:
            raise ConsumerError(f"unsupported event kind in native comparison: {event['kind']!r}")
    intervals = []
    for pair in scopes.values():
        if set(pair) != {"begin", "end"}:
            raise ConsumerError("normalized bundle contains an unpaired scope")
        begin, end = pair["begin"], pair["end"]
        if begin["thread_id"] != end["thread_id"] or begin["clock_id"] != end["clock_id"]:
            raise ConsumerError("scope endpoints cross thread or clock")
        attrs = begin.get("attrs", {})
        name = str(attrs.get("name", attrs.get("event_name", begin["scope_id"])))
        track_kind = begin.get("track_kind", "thread")
        pid = int(processes[begin["process_id"]]["pid"])
        if track_kind not in ("thread", "process", "gpu"):
            raise ConsumerError("unsupported scope track kind")
        tid = int(threads[begin["thread_id"]]["native_tid"]) if track_kind == "thread" else None
        query_track_kind = "thread" if track_kind == "thread" else "process"
        if begin["clock_id"] != end["clock_id"]:
            raise ConsumerError("scope endpoints cross clocks")
        row = {"ts": mapped_ts(begin),
               "dur": mapped_ts(end)-mapped_ts(begin),
               "name": name, "depth": 0, "track_kind": query_track_kind,
               "tid": tid, "pid": pid}
        slice_rows.append(row)
        event_slice[begin["event_id"]] = row
        event_slice[end["event_id"]] = row
        intervals.append((begin, end, row))
        collect_annotations(begin, row, "begin_")
        collect_annotations(end, row, "end_")
    for begin, end, row in intervals:
        for outer_begin, outer_end, _ in intervals:
            if outer_begin is begin:
                continue
            if (outer_begin["process_id"] == begin["process_id"] and
                    outer_begin["thread_id"] == begin["thread_id"] and
                    mapped_ts(outer_begin) <= mapped_ts(begin) and
                    mapped_ts(outer_end) >= mapped_ts(end) and
                    (mapped_ts(outer_begin) < mapped_ts(begin) or
                     mapped_ts(outer_end) > mapped_ts(end))):
                row["depth"] += 1
    # Perfetto reports instant slices at the current nesting depth of their
    # track. Include points enclosed by normalized scopes in the expected
    # depth, while leaving process/GPU events on independent tracks alone.
    for event in bundle["events"]:
        if event.get("kind") != "instant":
            continue
        row = event_slice[event["event_id"]]
        timestamp = mapped_ts(event)
        for begin, end, _ in intervals:
            if (begin["process_id"] == event["process_id"] and
                    begin["thread_id"] == event["thread_id"] and
                    begin.get("track_kind", "thread") == event.get("track_kind", "thread") and
                    mapped_ts(begin) <= timestamp < mapped_ts(end)):
                row["depth"] += 1
    slice_rows.sort(key=lambda r: (r["ts"], r["dur"], r["name"], r["track_kind"],
                                   r["pid"], -1 if r["tid"] is None else r["tid"]))
    flows = []
    event_by_id = {e["event_id"]: e for e in bundle["events"]}
    for relation in bundle["relations"]:
        is_native_flow = relation.get("flow") is True or relation["type"] == "flow"
        if not is_native_flow:
            source = event_by_id[relation["source_event"]]
            attrs = source.get("attrs", {})
            if (attrs.get("relation_type") != relation["type"] or
                    attrs.get("relation_evidence") != relation["evidence"]):
                raise ConsumerError("typed relation is not preserved by native annotations")
            if (relation.get("source_id") is not None and
                    str(attrs.get("relation_source_id")) != str(relation["source_id"])):
                raise ConsumerError("typed relation source identity is not preserved")
            if (relation.get("target_id") is not None and
                    str(attrs.get("relation_target_id")) != str(relation["target_id"])):
                raise ConsumerError("typed relation target identity is not preserved")
            continue
        source = event_by_id[relation["source_event"]]
        target = event_by_id[relation["target_event"]]
        if relation["type"] != "flow":
            source_attrs = source.get("attrs", {})
            target_attrs = target.get("attrs", {})
            source_code = str(source_attrs.get("event_type", source_attrs.get("event_code", "")))
            target_code = str(target_attrs.get("event_type", target_attrs.get("event_code", "")))
            if relation["type"] == "consumes" and (source_code, target_code) == ("5", "6"):
                queue_id = str(source_attrs.get("owner_id", "0"))
                sequence_id = str(source_attrs.get("work_id", "0"))
                if (queue_id in ("", "0") or sequence_id in ("", "0") or
                        queue_id != str(target_attrs.get("owner_id", "")) or
                        sequence_id != str(target_attrs.get("work_id", "")) or
                        (relation.get("source_id") is not None and
                         str(relation["source_id"]) != queue_id) or
                        (relation.get("target_id") is not None and
                         str(relation["target_id"]) != sequence_id)):
                    raise ConsumerError("queue-to-encode flow lacks exact queue/sequence binding")
            elif relation["type"] == "submitted_in" and (source_code, target_code) == ("26", "18"):
                queue_id = str(source_attrs.get("owner_id", "0"))
                command_buffer = str(source_attrs.get("work_id", "0"))
                if (queue_id in ("", "0") or command_buffer in ("", "0") or
                        queue_id != str(target_attrs.get("relation_queue_identity", "")) or
                        command_buffer != str(target_attrs.get("owner_id", "")) or
                        (relation.get("source_id") is not None and
                         str(relation["source_id"]) != queue_id) or
                        (relation.get("target_id") is not None and
                         str(relation["target_id"]) != command_buffer)):
                    raise ConsumerError("submit-to-GPU flow lacks exact queue/command-buffer binding")
            elif relation["type"] == "publishes" and (source_code, target_code) == ("3", "5"):
                evidence_id = relation.get("evidence_event")
                evidence = event_by_id.get(evidence_id) if evidence_id is not None else None
                evidence_attrs = evidence.get("attrs", {}) if evidence else {}
                evidence_code = str(evidence_attrs.get("event_type",
                    evidence_attrs.get("event_code", "")))
                queue_id = str(target_attrs.get("owner_id", "0"))
                sequence_id = str(target_attrs.get("work_id", "0"))
                if (evidence is None or evidence_code != "12" or
                        queue_id in ("", "0") or sequence_id in ("", "0") or
                        str(evidence_attrs.get("owner_id", "")) != str(source_attrs.get("owner_id", "")) or
                        str(evidence_attrs.get("work_id", "")) != str(source_attrs.get("work_id", "")) or
                        str(evidence_attrs.get("relation_queue_identity", "")) != queue_id or
                        str(evidence_attrs.get("related_id", "")) != sequence_id or
                        str(source_attrs.get("owner_id", "0")) in ("", "0") or
                        str(source_attrs.get("work_id", "0")) in ("", "0") or
                        (relation.get("source_id") is not None and
                         str(relation["source_id"]) != queue_id) or
                        (relation.get("target_id") is not None and
                         str(relation["target_id"]) != sequence_id)):
                    raise ConsumerError("replay-to-queue flow lacks exact evidence binding")
            else:
                raise ConsumerError(f"unsupported typed native flow relation: {relation['type']!r}")
        try:
            source, target = event_slice[relation["source_event"]], event_slice[relation["target_event"]]
        except KeyError as exc:
            raise ConsumerError("flow relation endpoint is not serialized as a slice") from exc
        flows.append({"out_ts": source["ts"], "out_name": source["name"],
                      "out_tid": source["tid"], "out_pid": source["pid"],
                      "in_ts": target["ts"], "in_name": target["name"],
                      "in_tid": target["tid"], "in_pid": target["pid"]})
    flows.sort(key=lambda r: (r["out_ts"], r["in_ts"], r["out_name"], r["in_name"]))
    annotation_rows.sort(key=lambda r: (r["ts"], r["dur"], r["name"], r["track_kind"],
                                        r["pid"], -1 if r["tid"] is None else r["tid"],
                                        r["key"], r["value"]))
    thread_tracks = {(e["process_id"], e["thread_id"]) for e in bundle["events"]
                     if e.get("track_kind", "thread") == "thread"}
    process_track_uuids = []
    for event in bundle["events"]:
        if event.get("track_kind", "thread") not in ("process", "gpu"):
            continue
        if event.get("track_id") is None or not str(event["track_id"]).isdigit():
            raise ConsumerError("process-associated tracks require a decimal track_id")
        process_track_uuids.append(int(event["track_id"]))
    process_track_uuids = sorted(set(process_track_uuids))
    process_tracks = set(process_track_uuids)
    if not slice_rows and not counters:
        raise ConsumerError("normalized source has no timeline events")
    expected = {"slice_count": len(slice_rows), "complete_slice_count": len(slice_rows),
            "open_slice_count": 0, "counter_count": len(counters), "flow_count": len(flows),
            "incomplete_flow_count": 0, "slice_track_count": len(thread_tracks)+len(process_tracks),
            "thread_track_count": len(thread_tracks), "process_track_count": len(process_tracks),
            "process_track_uuids": process_track_uuids,
            "nested_slice_count": sum(row["depth"] > 0 for row in slice_rows),
            "min_ts": min((r["ts"] for r in slice_rows), default=0),
            "max_ts": max((r["ts"]+max(r["dur"], 0) for r in slice_rows), default=0),
            "total_duration_ns": sum(r["dur"] for r in slice_rows if r["dur"] > 0),
            "slice_rows": slice_rows, "flow_rows": flows, "counter_rows": counters}
    expected["annotation_rows"] = annotation_rows
    canonical = json.dumps(bundle, separators=(",", ":"), sort_keys=True).encode()
    expected["_normalized_bundle_sha256"] = hashlib.sha256(canonical).hexdigest()
    expected["_source_input_sha256"] = sorted(
        ref["sha256"] for ref in bundle.get("inputs", []) if isinstance(ref, dict) and "sha256" in ref)
    return expected


def qualify_trace(trace: Path, *, tp: Path | None = None,
                  expected: dict[str, Any] | None = None,
                  require_complete: bool = True) -> dict[str, Any]:
    if not trace.is_file() or trace.stat().st_size == 0:
        raise ConsumerError("trace file is missing or empty")
    with trace.open("rb") as stream:
        prefix = stream.read(32).lstrip()
    if prefix[:1] in (b"{", b"[", b"<"):
        raise ConsumerError("input is not native Perfetto protobuf; JSON/XML compatibility inputs are separate artifacts")
    if tp is None:
        tp, lock = resolve_pinned_processor()
    else:
        lock = load_lock()
        _validate_binary(tp, lock)
    version = _validate_binary(tp, lock)
    query_output = _run_query(tp, trace, QUERY)
    stats = {k: int(v) for k, v in _first_csv(query_output).items()}
    slice_rows = _csv_rows(_run_query(tp, trace, SLICE_ROWS_QUERY))
    flow_rows = _csv_rows(_run_query(tp, trace, FLOW_ROWS_QUERY))
    counter_rows = _csv_rows(_run_query(tp, trace, COUNTER_ROWS_QUERY))
    annotation_rows = _csv_rows(_run_query(tp, trace, ANNOTATION_ROWS_QUERY))
    process_track_ids = _csv_rows(_run_query(tp, trace, PROCESS_TRACK_IDS_QUERY))
    details = {
        "slice_rows": [{"ts": int(r["ts"]), "dur": int(r["dur"]), "name": r["name"],
                        "depth": int(r["depth"]), "track_kind": r["track_kind"],
                        "tid": None if r["tid"] == "[NULL]" else int(r["tid"]),
                        "pid": None if r["pid"] == "[NULL]" else int(r["pid"])}
                       for r in slice_rows],
        "flow_rows": [{"out_ts": int(r["out_ts"]), "out_name": r["out_name"],
                       "out_tid": None if r["out_tid"] == "[NULL]" else int(r["out_tid"]),
                       "out_pid": None if r["out_pid"] == "[NULL]" else int(r["out_pid"]),
                       "in_ts": int(r["in_ts"]), "in_name": r["in_name"],
                       "in_tid": None if r["in_tid"] == "[NULL]" else int(r["in_tid"]),
                       "in_pid": None if r["in_pid"] == "[NULL]" else int(r["in_pid"])}
                      for r in flow_rows],
        "counter_rows": [{"ts": int(r["ts"]), "value": int(r["value"]),
                          "track_name": r["track_name"]} for r in counter_rows],
        "process_track_uuids": sorted(int(r["track_uuid"]) & ((1 << 64) - 1)
                                      for r in process_track_ids),
        "annotation_rows": [{"ts": int(r["ts"]), "dur": int(r["dur"]), "name": r["name"],
                              "track_kind": r["track_kind"],
                              "tid": None if r["tid"] == "[NULL]" else int(r["tid"]),
                              "pid": None if r["pid"] == "[NULL]" else int(r["pid"]),
                              "key": r["key"], "value": r["value"]}
                             for r in annotation_rows]}
    diagnostics_text = _run_query(tp, trace, DIAGNOSTIC_QUERY).strip()
    diagnostics = [] if not diagnostics_text else list(csv.DictReader(io.StringIO(diagnostics_text)))
    if stats["slice_count"] == 0 and stats["counter_count"] == 0:
        raise ConsumerError("Trace Processor parsed no timeline slices or counters")
    result: dict[str, Any] = {"schema": "dxmt9-trace-processor-receipt-v1",
        "consumer": {"name": "Perfetto Trace Processor", "version": version,
                     "sha256": _sha256(tp), "source": lock["source"]},
        "trace": {"path": str(trace), "size": trace.stat().st_size,
                  "sha256": _sha256(trace)},
        "queries": {"native_timeline": stats, **details}, "diagnostics": diagnostics,
        "verdict": "qualified"}
    if expected is not None:
        result["expectation_source"] = {"normalized_bundle_sha256": expected.get("_normalized_bundle_sha256"),
                                        "source_input_sha256": expected.get("_source_input_sha256", [])}
    reasons = []
    if stats["open_slice_count"]:
        reasons.append("open_slices")
    if stats["incomplete_flow_count"]:
        reasons.append("incomplete_flow_endpoints")
    if diagnostics:
        reasons.append("trace_processor_diagnostics")
    if expected is None:
        reasons.append("query_expectations_missing")
    if reasons:
        result["verdict"] = "unqualified"
        result["reasons"] = reasons
    if expected is None and require_complete:
        raise ConsumerError("complete native qualification requires expectations derived from normalized input")
    if expected is not None:
        mismatches = []
        required_expectations = {"slice_rows", "flow_rows", "counter_rows", "slice_count",
                                 "counter_count", "flow_count", "min_ts", "max_ts",
                                 "total_duration_ns", "thread_track_count", "process_track_count",
                                 "annotation_rows", "process_track_uuids",
                                 "_normalized_bundle_sha256", "_source_input_sha256"}
        if require_complete and not required_expectations <= expected.keys():
            raise ConsumerError("query expectations are incomplete for native qualification")
        for key in ("slice_count", "complete_slice_count", "open_slice_count", "counter_count",
                    "flow_count", "incomplete_flow_count", "slice_track_count", "min_ts", "max_ts",
                    "total_duration_ns", "thread_track_count", "process_track_count",
                    "nested_slice_count"):
            if key in expected and stats[key] != int(expected[key]):
                mismatches.append(f"{key}:expected={expected[key]}:actual={stats[key]}")
        for key in ("slice_rows", "flow_rows", "counter_rows", "annotation_rows"):
            if key not in expected:
                continue
            matches = (_annotation_rows_match(expected[key], details[key])
                       if key == "annotation_rows" else details[key] == expected[key])
            if not matches:
                mismatches.append(f"{key}:expected={expected[key]!r}:actual={details[key]!r}")
        if "process_track_uuids" in expected and details["process_track_uuids"] != expected["process_track_uuids"]:
            mismatches.append(f"process_track_uuids:expected={expected['process_track_uuids']!r}:"
                              f"actual={details['process_track_uuids']!r}")
        if mismatches:
            result["verdict"] = "unqualified"
            result.setdefault("reasons", []).append("query_fixture_mismatch")
            result["mismatches"] = mismatches
    if result["verdict"] != "qualified" and require_complete:
        raise ConsumerError("native Trace Processor qualification failed: " +
                            json.dumps(result.get("reasons", [])))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="native Perfetto protobuf trace")
    parser.add_argument("--tp", type=Path, help="explicit executable; must match tracing_toolchain.lock.json")
    parser.add_argument("--expected", type=Path, help="JSON object of required query result values")
    parser.add_argument("--bundle", type=Path, help="normalized bundle used to derive query expectations")
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--allow-partial", action="store_true")
    args = parser.parse_args()
    try:
        if args.expected and args.bundle:
            raise ConsumerError("choose either --expected or --bundle")
        if args.bundle:
            normalizer_path = Path(__file__).with_name("tracing_normalize.py")
            spec = importlib.util.spec_from_file_location("tracing_normalize", normalizer_path)
            normalizer = importlib.util.module_from_spec(spec)
            assert spec.loader is not None
            spec.loader.exec_module(normalizer)
            bundle = json.loads(args.bundle.read_text(encoding="utf-8"))
            normalizer.validate_bundle(bundle, require_complete=not args.allow_partial)
            expected = derive_expected(bundle)
        else:
            expected = json.loads(args.expected.read_text(encoding="utf-8")) if args.expected else None
        receipt = qualify_trace(args.trace, tp=args.tp, expected=expected,
                                require_complete=not args.allow_partial)
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.receipt.with_name(args.receipt.name + ".tmp")
        tmp.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        tmp.replace(args.receipt)
        print(json.dumps(receipt, sort_keys=True))
        return 0
    except (OSError, json.JSONDecodeError, ConsumerError) as exc:
        print(f"native trace qualification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
