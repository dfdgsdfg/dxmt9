#!/usr/bin/env python3
"""External producer-thread CPU receipts; never PE-exclusive or wall-time samples."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
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
