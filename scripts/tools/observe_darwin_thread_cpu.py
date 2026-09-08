#!/usr/bin/env python3
"""External producer-thread CPU receipts; never PE-exclusive or wall-time samples."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time


MARKER = re.compile(r"unix_commit_chunk_entry device=(0x[0-9a-fA-F]+) native_tid=(0x[0-9a-fA-F]+)")
NEGOTIATION = re.compile(r"command chunk negotiation selected canonical transport=(0x[0-9a-fA-F]+)")
FRAME = re.compile(r"\[dxmt9-perf-frame frame=(\d+) wall_ms=([0-9.]+).*? draw_calls=(\d+) ")
LIMIT = 8 * 1024 * 1024
MAX_QUERY_NS = 100_000_000


class LiveLog:
    """One fresh run log; rotation/truncation and selector ambiguity fail closed."""

    def __init__(self, path: Path):
        self.path = path
        self.offset = 0
        self.file_identity = None
        self.metadata = None
        self.pending = b""
        self.selectors = set()
        self.negotiations = []
        self.frame = None

    def read(self):
        with self.path.open("rb") as stream:
            stat = os.fstat(stream.fileno())
            identity = (stat.st_dev, stat.st_ino)
            if self.file_identity not in (None, identity) or stat.st_size < self.offset:
                raise ValueError("log identity changed or truncated")
            self.file_identity = identity
            self.metadata = dict(device=stat.st_dev, inode=stat.st_ino,
                modified_unix_ns=stat.st_mtime_ns,
                birth_unix_ns=int(stat.st_birthtime*1e9) if hasattr(stat, 'st_birthtime') else None)
            stream.seek(self.offset)
            data = stream.read(LIMIT + 1)
            if len(data) > LIMIT:
                raise ValueError("log read bound exceeded")
            self.offset += len(data)
        lines = (self.pending + data).split(b"\n")
        self.pending = lines.pop()
        if len(self.pending) > LIMIT:
            raise ValueError("log line bound exceeded")
        for line in lines:
            text = line.decode("utf-8", errors="replace")
            marker = MARKER.search(text)
            if marker:
                self.selectors.add(tuple(int(value, 16) for value in marker.groups()))
            negotiation = NEGOTIATION.search(text)
            if negotiation:
                self.negotiations.append(int(negotiation[1], 16))
            frame = FRAME.search(text)
            if frame:
                ordinal, wall, draws = frame.groups()
                current = dict(frame=int(ordinal), wall_ms=float(wall), draws=int(draws))
                if self.frame and current["frame"] <= self.frame["frame"]:
                    raise ValueError("frame ordinal did not increase")
                self.frame = current
        if len(self.selectors) > 1:
            raise ValueError("ambiguous producer device/native thread selector")
        if len(self.negotiations) > 1:
            raise ValueError("multiple successful device negotiations")
        return self.frame

    @property
    def native_tid(self):
        return next(iter(self.selectors))[1] if self.selectors else None


def qualified_snapshot(value, pid, tid):
    if (value.get("status") != "ok" or type(value.get("schema_version")) is not int or
            value.get("schema_version") != 1 or
            value.get("units") != "nanoseconds"):
        raise ValueError("helper did not return a successful version-1 snapshot")
    fields = ("pid", "native_tid", "start_sec", "start_usec", "monotonic_begin_ns",
              "monotonic_end_ns", "user_ns", "system_ns")
    if any(type(value.get(key)) is not int or value[key] < 0 for key in fields):
        raise ValueError("invalid or missing integer snapshot field")
    if value["pid"] != pid or value["native_tid"] != tid or pid <= 0 or tid <= 0:
        raise ValueError("snapshot target mismatch")
    if value["start_usec"] >= 1_000_000 or value["start_sec"] == 0:
        raise ValueError("invalid process start identity")
    duration = value["monotonic_end_ns"] - value["monotonic_begin_ns"]
    if duration < 0 or duration > MAX_QUERY_NS:
        raise ValueError("invalid or excessive query duration")
    return value


def identity(value):
    return tuple(value[key] for key in ("pid", "native_tid", "start_sec", "start_usec"))


class Observer:
    def __init__(self, helper: Path, log: Path, pid: int):
        self.helper, self.pid = helper, pid
        self.log = LiveLog(log)
        self.target = None

    def sample(self):
        before = self.log.read()
        tid = self.log.native_tid
        if tid is None:
            return dict(status="pending_selector")
        if self.log.negotiations != [1]:
            raise ValueError("commit marker without a unique contiguous device negotiation receipt")
        begin = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        proc = subprocess.run([str(self.helper), "--pid", str(self.pid), "--tid", str(tid)],
                              capture_output=True, text=True, timeout=3)
        end = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        after = self.log.read()
        try:
            raw = json.loads(proc.stdout)
        except (ValueError, TypeError):
            return dict(status="query_error", returncode=proc.returncode,
                        stdout=proc.stdout[:4096], stderr=proc.stderr[:4096])
        if proc.returncode != 0:
            return dict(status="query_error", returncode=proc.returncode, helper=raw)
        value = qualified_snapshot(raw, self.pid, tid)
        if self.log.metadata['modified_unix_ns'] < value['start_sec']*1_000_000_000 + value['start_usec']*1000:
            raise ValueError("log last modification precedes target process start")
        if self.target is None:
            self.target = identity(value)
        if identity(value) != self.target:
            raise ValueError("process/thread identity changed; do not rebind")
        if not (begin <= value["monotonic_begin_ns"] <= value["monotonic_end_ns"] <= end):
            raise ValueError("helper monotonic clock is outside observation bracket")
        if end - begin > MAX_QUERY_NS:
            return dict(status="slow_query", elapsed_ns=end-begin, helper=value)
        return dict(status="sample", snapshot=value, frame_before=before, frame_after=after,
                    observer_begin_ns=begin, observer_end_ns=end,
                    selector=dict(device=next(iter(self.log.selectors))[0],
                                  native_tid=tid, transport=1), log_metadata=self.log.metadata)


def summarize(rows, first_frame: int, last_frame: int):
    """Adjacent stable endpoints only; errors/gaps never become zero-cost samples."""
    rejected = collections.Counter()
    previous = None
    target = None
    selector = None
    selector_missing = False
    intervals = []
    for row in rows:
        if row.get("status") != "sample":
            rejected[row.get("status", "unknown")] += 1
            previous = None
            continue
        value = row["snapshot"]
        qualified_snapshot(value, value["pid"], value["native_tid"])
        if target is None:
            target = identity(value)
        if identity(value) != target:
            raise ValueError("mixed process/thread identities in one report")
        current_selector = row.get('selector')
        if current_selector is None:
            selector_missing = True
        elif selector is None:
            selector = current_selector
        elif current_selector != selector:
            raise ValueError("mixed device selectors in one report")
        before, after = row.get("frame_before"), row.get("frame_after")
        if (not before or before != after or before["draws"] <= 100 or
                before["wall_ms"] <= 0 or not first_frame <= before["frame"] < last_frame):
            rejected["unstable_or_outside_scene_endpoint"] += 1
            previous = None
            continue
        if previous is not None:
            old = previous["snapshot"]
            frame_delta = before["frame"] - previous["frame_before"]["frame"]
            begin = old["monotonic_end_ns"]
            end = value["monotonic_begin_ns"]
            if value["user_ns"] < old["user_ns"] or value["system_ns"] < old["system_ns"]:
                raise ValueError("thread CPU counter decreased")
            if end <= begin:
                raise ValueError("overlapping or nonmonotonic CPU samples")
            if frame_delta > 0:
                intervals.append(dict(first_frame=previous["frame_before"]["frame"],
                    last_frame=before["frame"], frames=frame_delta,
                    user_ns=value["user_ns"]-old["user_ns"],
                    system_ns=value["system_ns"]-old["system_ns"],
                    wall_ns=end-begin))
            else:
                rejected["no_frame_progress"] += 1
        previous = row
    frames = sum(row["frames"] for row in intervals)
    user = sum(row["user_ns"] for row in intervals)
    system = sum(row["system_ns"] for row in intervals)
    wall = sum(row["wall_ns"] for row in intervals)
    return dict(status="partial" if frames else "unmeasured", identity=target,
        selector=None if selector_missing else selector,
        frames=frames, user_ns=user, system_ns=system, wall_ns=wall,
        cpu_ms_per_frame=(user+system)/frames/1e6 if frames else None,
        average_cores=(user+system)/wall if wall else None,
        intervals=intervals, rejected=dict(rejected),
        qualification="Partial producer native-thread CPU, including game/Wine/synchronous Unix; not PE-exclusive. Stable log-frame endpoints are approximate, not exact Present timestamps. No whole-scene or CPU-saving claim.")


def _frame_record(value):
    if not isinstance(value, dict):
        raise ValueError("sample is missing a log-frame record")
    if (type(value.get("frame")) is not int or value["frame"] < 0 or
            type(value.get("draws")) is not int or value["draws"] < 0 or
            not isinstance(value.get("wall_ms"), (int, float)) or
            isinstance(value["wall_ms"], bool) or
            not math.isfinite(value["wall_ms"]) or value["wall_ms"] < 0):
        raise ValueError("invalid log-frame record")
    return value


def _frame_in_scene(value, first_frame, last_frame):
    return first_frame <= value["frame"] < last_frame


def _stable_scene_endpoint(row, first_frame, last_frame):
    if row.get("status") != "sample":
        return False
    try:
        value = row["snapshot"]
        qualified_snapshot(value, value["pid"], value["native_tid"])
        before = _frame_record(row.get("frame_before"))
        after = _frame_record(row.get("frame_after"))
    except (KeyError, TypeError, ValueError):
        return False
    return (before == after and before["draws"] > 100 and before["wall_ms"] > 0 and
            _frame_in_scene(before, first_frame, last_frame))


def _selector_key(selector, native_tid):
    if selector is None:
        return None
    if not isinstance(selector, dict):
        raise ValueError("invalid device selector")
    device = selector.get("device")
    tid = selector.get("native_tid")
    transport = selector.get("transport")
    if (type(device) is not int or device <= 0 or type(tid) is not int or tid <= 0 or
            type(transport) is not int or transport != 1 or tid != native_tid):
        raise ValueError("invalid device selector")
    return (device, tid, transport)


def _log_provenance_key(metadata):
    if metadata is None:
        return None
    if not isinstance(metadata, dict):
        raise ValueError("invalid log provenance")
    device = metadata.get("device")
    inode = metadata.get("inode")
    modified = metadata.get("modified_unix_ns")
    if (type(device) is not int or device < 0 or type(inode) is not int or inode < 0 or
            type(modified) is not int or modified < 0):
        raise ValueError("invalid log provenance")
    return (device, inode)


def _validate_query_bounds(row, value):
    fields = ("observer_begin_ns", "observer_end_ns")
    present = [row.get(field) is not None for field in fields]
    if not all(present):
        raise ValueError("incomplete observer query bounds")
    begin = row["observer_begin_ns"]
    end = row["observer_end_ns"]
    if (type(begin) is not int or type(end) is not int or begin < 0 or end < begin or
            end - begin > MAX_QUERY_NS or
            not (begin <= value["monotonic_begin_ns"] <= value["monotonic_end_ns"] <= end)):
        raise ValueError("invalid observer query bounds")
    return dict(observer_begin_ns=begin, observer_end_ns=end,
                elapsed_ns=end - begin)


def _excluded_rows(rows):
    statuses = collections.Counter(row.get("status", "unknown") for row in rows)
    return dict(rows=len(rows), statuses=dict(statuses))


def _envelope_unmeasured(rows, first_frame, last_frame, rejected=None,
                         prefix=None, suffix=None, reason=None):
    rejected = collections.Counter(rejected or {})
    if reason and reason not in rejected:
        rejected[reason] += 1
    prefix = _excluded_rows(rows) if prefix is None else prefix
    suffix = _excluded_rows([]) if suffix is None else suffix
    return dict(status="unmeasured", identity=None, selector=None,
        selector_provenance="unverified", frames=0, frame_span=0,
        coverage_frames=0, scene_frames=max(0, last_frame - first_frame),
        coverage_fraction=None, user_ns=0, system_ns=0, wall_ns=0,
        cpu_ns=0, cpu_ms=0.0, cpu_ms_per_frame=None,
        average_cores=None, intervals=[], rejected=dict(rejected),
        excluded_prefix=prefix, excluded_suffix=suffix,
        endpoint_approximation=(
            "Stable log-frame observations are approximate scene endpoints, "
            "not exact Present timestamps."),
        query_bounds=dict(max_query_ns=MAX_QUERY_NS, first=None, last=None),
        log_provenance="unverified",
        qualification=(
            "Unmeasured external producer native-thread CPU; when measured, "
            "includes game/Wine/synchronous Unix, not PE-exclusive CPU and "
            "not exact Present CPU."))


def summarize_envelope(rows, first_frame: int, last_frame: int):
    """Measure one closed scene envelope across unstable frame samples.

    Stable samples define the endpoints. Every row between those endpoints is
    validated, but only the endpoint CPU counters are differenced, so an
    unstable intermediate frame never creates a false partial population.
    """
    rows = list(rows)
    if last_frame <= first_frame:
        return _envelope_unmeasured(rows, first_frame, last_frame,
                                    reason="invalid_scene_bounds")

    endpoint_indices = [index for index, row in enumerate(rows)
                        if _stable_scene_endpoint(row, first_frame, last_frame)]
    if len(endpoint_indices) < 2:
        rejected = collections.Counter()
        for row in rows:
            if row.get("status") != "sample":
                rejected[row.get("status", "unknown")] += 1
            elif not _stable_scene_endpoint(row, first_frame, last_frame):
                rejected["unstable_or_outside_scene_endpoint"] += 1
        return _envelope_unmeasured(rows, first_frame, last_frame, rejected=rejected,
                                    reason="insufficient_stable_endpoints")

    first_index, last_index = endpoint_indices[0], endpoint_indices[-1]
    prefix = _excluded_rows(rows[:first_index])
    suffix = _excluded_rows(rows[last_index + 1:])
    target = None
    selector = None
    provenance = None
    previous = None
    previous_frames = None
    samples = []

    for index in range(first_index, last_index + 1):
        row = rows[index]
        if row.get("status") != "sample":
            raise ValueError("non-sample row inside CPU envelope")
        try:
            value = row["snapshot"]
            qualified_snapshot(value, value["pid"], value["native_tid"])
            before = _frame_record(row.get("frame_before"))
            after = _frame_record(row.get("frame_after"))
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError("invalid sample inside CPU envelope") from error
        current_identity = identity(value)
        if target is None:
            target = current_identity
        elif current_identity != target:
            raise ValueError("mixed process/thread identities in one CPU envelope")

        current_selector = _selector_key(row.get("selector"), value["native_tid"])
        if current_selector is None:
            raise ValueError("device selector missing from part of CPU envelope")
        if selector is None:
            selector = current_selector
        elif current_selector != selector:
            raise ValueError("mixed device selectors in one CPU envelope")

        current_provenance = _log_provenance_key(row.get("log_metadata"))
        if current_provenance is None:
            raise ValueError("log provenance missing from part of CPU envelope")
        if provenance is None:
            provenance = current_provenance
        elif current_provenance != provenance:
            raise ValueError("log file identity changed inside CPU envelope")
        query = _validate_query_bounds(row, value)
        if previous is not None:
            if (value["user_ns"] < previous["user_ns"] or
                    value["system_ns"] < previous["system_ns"]):
                raise ValueError("thread CPU counter decreased inside CPU envelope")
            if value["monotonic_begin_ns"] <= previous["monotonic_end_ns"]:
                raise ValueError("overlapping or nonmonotonic CPU samples")
            if before["frame"] < previous_frames["frame"]:
                raise ValueError("log-frame ordinal regressed inside CPU envelope")
        if after["frame"] < before["frame"]:
            raise ValueError("log-frame interval regressed inside CPU envelope")
        if not (_frame_in_scene(before, first_frame, last_frame) and
                _frame_in_scene(after, first_frame, last_frame)):
            raise ValueError("sample outside scene inside CPU envelope")
        samples.append((row, value, before, after, query))
        previous, previous_frames = value, after

    _, first_value, first_before, _, first_query = samples[0]
    last_value, _, last_after, last_query = samples[-1][1:]
    frame_span = last_after["frame"] - first_before["frame"]
    if frame_span <= 0:
        return _envelope_unmeasured(
            rows, first_frame, last_frame,
            rejected={"no_frame_progress": 1}, prefix=prefix, suffix=suffix,
            reason="no_frame_progress")
    wall_ns = last_value["monotonic_begin_ns"] - first_value["monotonic_end_ns"]
    if wall_ns <= 0:
        return _envelope_unmeasured(
            rows, first_frame, last_frame,
            rejected={"no_wall_progress": 1}, prefix=prefix, suffix=suffix,
            reason="no_wall_progress")
    user = last_value["user_ns"] - first_value["user_ns"]
    system = last_value["system_ns"] - first_value["system_ns"]
    scene_frames = last_frame - first_frame
    query_bounds = dict(max_query_ns=MAX_QUERY_NS,
        first=dict(snapshot_begin_ns=first_value["monotonic_begin_ns"],
                   snapshot_end_ns=first_value["monotonic_end_ns"],
                   query_duration_ns=(first_value["monotonic_end_ns"] -
                                      first_value["monotonic_begin_ns"])),
        last=dict(snapshot_begin_ns=last_value["monotonic_begin_ns"],
                  snapshot_end_ns=last_value["monotonic_end_ns"],
                  query_duration_ns=(last_value["monotonic_end_ns"] -
                                     last_value["monotonic_begin_ns"])))
    if first_query is not None:
        query_bounds["first"].update(first_query)
    if last_query is not None:
        query_bounds["last"].update(last_query)
    return dict(status="partial", identity=target,
        selector=None if selector is None else dict(device=selector[0],
                                                    native_tid=selector[1],
                                                    transport=selector[2]),
        selector_provenance="stable",
        frames=frame_span, frame_span=frame_span, coverage_frames=frame_span,
        scene_frames=scene_frames, coverage_fraction=frame_span / scene_frames,
        user_ns=user, system_ns=system, wall_ns=wall_ns,
        cpu_ns=user + system, cpu_ms=(user + system) / 1e6,
        cpu_ms_per_frame=(user + system) / frame_span / 1e6,
        average_cores=(user + system) / wall_ns,
        intervals=[dict(first_frame=first_before["frame"],
                        last_frame=last_after["frame"], frames=frame_span,
                        user_ns=user, system_ns=system, wall_ns=wall_ns)],
        rejected={}, excluded_prefix=prefix, excluded_suffix=suffix,
        endpoint_approximation=(
            "Stable log-frame observations are approximate scene endpoints, "
            "not exact Present timestamps."),
        query_bounds=query_bounds,
        log_provenance="stable-file-identity",
        qualification=(
            "Partial envelope producer native-thread CPU, including game/Wine/"
            "synchronous Unix; not PE-exclusive and not exact Present CPU. "
            "CPU is the cumulative delta between stable log-frame endpoints; "
            "endpoint and query bounds remain approximate. Caller-supplied log "
            "provenance does not establish causal ownership."))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--helper", type=Path, default=Path("build/tools/dxmt9-thread-cpu"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=120)
    parser.add_argument("--interval", type=float, default=2.0)
    args = parser.parse_args()
    if args.pid <= 0 or not 1 <= args.samples <= 10000 or not 0.5 <= args.interval <= 60:
        parser.error("positive PID, 1..10000 samples and 0.5..60 second interval required")
    observer = Observer(args.helper.resolve(), args.log, args.pid)
    with args.output.open("x") as stream:
        header = dict(status="header", pid=args.pid, log=str(args.log.resolve()),
                      interval_seconds=args.interval,
                      log_provenance='caller-supplied; freshness metadata is not causal ownership proof',
                      helper_sha256=hashlib.sha256(args.helper.read_bytes()).hexdigest())
        print(json.dumps(header), file=stream, flush=True)
        sample_count = 0
        for index in range(args.samples):
            try:
                row = observer.sample()
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                print(json.dumps(dict(status="fatal", reason=str(error))), file=stream, flush=True)
                return 1
            print(json.dumps(row), file=stream, flush=True)
            sample_count += row['status'] == 'sample'
            if row["status"] == "query_error":
                return 1
            if index + 1 < args.samples:
                time.sleep(args.interval)
    return 0 if sample_count else 1


if __name__ == "__main__":
    raise SystemExit(main())
