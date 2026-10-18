#!/usr/bin/env python3
"""Immutable companion SQLite for Perfetto and supplemental experiment evidence.

This is a local analysis schema, not a Trace Processor database or a .pftrace.
Raw timestamps stay in their declared clocks; publication never qualifies data.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
from pathlib import Path
import sqlite3
import stat
import sys
import tempfile
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.tools.tracing_normalize import adapt, validate_bundle  # noqa: E402

SCHEMA = "dxmt9.observation.sqlite.v1"
MAX_INPUT_BYTES = 256 * 1024 * 1024
MAX_EVENTS = 1_000_000

DDL = """
PRAGMA foreign_keys=ON;
PRAGMA user_version=1;
CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE test_result(name TEXT PRIMARY KEY, status TEXT NOT NULL, details_json TEXT NOT NULL);
CREATE TABLE provenance(key TEXT PRIMARY KEY, value_json TEXT NOT NULL);
CREATE TABLE artifact(id INTEGER PRIMARY KEY, role TEXT NOT NULL, path TEXT NOT NULL,
 sha256 TEXT, bytes INTEGER, digest_kind TEXT, status TEXT NOT NULL, collector TEXT, reason TEXT);
CREATE TABLE diagnostic_row(id INTEGER PRIMARY KEY, artifact_id INTEGER REFERENCES artifact,
 line_number INTEGER NOT NULL, category TEXT NOT NULL, record_text TEXT NOT NULL);
CREATE TABLE diagnostic_value(row_id INTEGER REFERENCES diagnostic_row, key TEXT NOT NULL,
 value_exact TEXT NOT NULL, PRIMARY KEY(row_id, key));
CREATE TABLE bundle(id INTEGER PRIMARY KEY, artifact_id INTEGER REFERENCES artifact,
 run_id TEXT NOT NULL, validity_json TEXT NOT NULL, envelope_json TEXT NOT NULL);
CREATE TABLE observation_report(id INTEGER PRIMARY KEY, bundle_id INTEGER REFERENCES bundle,
 source_event_id INTEGER REFERENCES source_event, source TEXT NOT NULL, source_record TEXT NOT NULL,
 schema TEXT NOT NULL, encoding INTEGER NOT NULL, status TEXT NOT NULL, payload TEXT NOT NULL,
 value_exact TEXT, payload_bytes BLOB NOT NULL);
CREATE TABLE observation_field(report_id INTEGER REFERENCES observation_report,
 ordinal INTEGER NOT NULL, key TEXT NOT NULL, value_exact TEXT NOT NULL,
 PRIMARY KEY(report_id, ordinal));
CREATE TABLE structural_value(artifact_id INTEGER REFERENCES artifact,
 path TEXT NOT NULL, value_json TEXT NOT NULL, PRIMARY KEY(artifact_id, path));
CREATE TABLE clock(id INTEGER PRIMARY KEY, bundle_id INTEGER REFERENCES bundle,
 source_id TEXT NOT NULL, unit TEXT NOT NULL, alignment TEXT NOT NULL, definition_json TEXT NOT NULL,
 UNIQUE(bundle_id, source_id));
CREATE TABLE track(id INTEGER PRIMARY KEY, bundle_id INTEGER REFERENCES bundle,
 source_id TEXT NOT NULL, name TEXT NOT NULL, type TEXT NOT NULL, unit TEXT,
 process_id TEXT, thread_id TEXT, clock_id INTEGER REFERENCES clock, definition_json TEXT NOT NULL);
CREATE TABLE source_event(id INTEGER PRIMARY KEY, bundle_id INTEGER REFERENCES bundle,
 source_id TEXT NOT NULL, source_record TEXT NOT NULL, collector TEXT NOT NULL,
 track_id INTEGER REFERENCES track, clock_id INTEGER REFERENCES clock,
 ts INTEGER, timestamp_exact TEXT NOT NULL, kind TEXT NOT NULL, record_json TEXT NOT NULL,
 UNIQUE(bundle_id, source_id));
CREATE TABLE args(arg_set_id INTEGER REFERENCES source_event, key TEXT NOT NULL,
 value_json TEXT NOT NULL, PRIMARY KEY(arg_set_id, key));
CREATE TABLE slice(id INTEGER PRIMARY KEY, track_id INTEGER REFERENCES track,
 clock_id INTEGER REFERENCES clock, ts INTEGER, dur INTEGER, name TEXT NOT NULL,
 arg_set_id INTEGER REFERENCES source_event, end_event_id INTEGER REFERENCES source_event,
 status TEXT NOT NULL);
CREATE TABLE counter(id INTEGER PRIMARY KEY, track_id INTEGER REFERENCES track,
 clock_id INTEGER REFERENCES clock, ts INTEGER, value REAL, value_exact TEXT NOT NULL,
 arg_set_id INTEGER REFERENCES source_event);
CREATE TABLE flow(id INTEGER PRIMARY KEY, bundle_id INTEGER REFERENCES bundle,
 source_event_id INTEGER REFERENCES source_event, target_event_id INTEGER REFERENCES source_event,
 type TEXT NOT NULL, evidence_json TEXT NOT NULL);
CREATE TABLE qualification(id INTEGER PRIMARY KEY, bundle_id INTEGER REFERENCES bundle,
 scope TEXT NOT NULL, status TEXT NOT NULL, details_json TEXT NOT NULL);
CREATE TABLE artifact_link(id INTEGER PRIMARY KEY, artifact_id INTEGER REFERENCES artifact,
 source_event_id INTEGER REFERENCES source_event, work_id TEXT, owner_id TEXT,
 relation TEXT NOT NULL, evidence TEXT NOT NULL);
CREATE INDEX event_clock_time ON source_event(clock_id, ts);
CREATE INDEX slice_track_time ON slice(track_id, ts);
CREATE INDEX counter_track_time ON counter(track_id, ts);
"""


def encoded(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def artifact_identity(path: Path) -> tuple[str, int, str]:
    if path.is_file():
        return digest(path), path.stat().st_size, "sha256"
    if not path.is_dir():
        raise ValueError(f"unsupported artifact kind: {path}")
    rows, size = [], 0
    for item in sorted(path.rglob("*")):
        safe_path(item)
        if item.is_file():
            info = item.stat()
            rows.append({"path": item.relative_to(path).as_posix(),
                         "sha256": digest(item), "bytes": info.st_size})
            size += info.st_size
        elif item.is_dir():
            rows.append({"path": item.relative_to(path).as_posix(), "kind": "directory"})
        else:
            raise ValueError(f"unsupported capture entry: {item}")
        if len(rows) > 100_000:
            raise ValueError("capture directory entry budget exceeded")
    return hashlib.sha256(encoded(rows).encode()).hexdigest(), size, "tree-sha256-v1"


def safe_path(path: Path) -> Path:
    path = Path(os.path.abspath(path))
    # macOS /var and /tmp are system aliases; normalize that prefix, but reject
    # symlinks inside the actual evidence/output directory.
    for part in (path, *path.parents):
        if part.is_symlink() and str(part) not in ("/var", "/tmp"):
            raise ValueError(f"symlinked evidence path: {part}")
    return path


def integer(value: Any) -> int | None:
    value = int(value)
    return value if -(1 << 63) <= value < (1 << 63) else None


def _insert(db: sqlite3.Connection, table: str, **values: Any) -> int:
    # Table and column identifiers are owned by this module, never inputs.
    return db.execute(f"INSERT INTO {table} ({','.join(values)}) VALUES "
                      f"({','.join('?' for _ in values)})", tuple(values.values())).lastrowid


def _artifact(db: sqlite3.Connection, ref: dict[str, Any]) -> int:
    path = safe_path(Path(ref["path"]))
    if not path.exists():
        if ref.get("required", True):
            raise ValueError(f"missing artifact: {path}")
        return _insert(db, "artifact", role=ref["role"], path=str(path),
                       status="missing", collector=ref.get("collector"),
                       reason=ref.get("reason", "not collected"))
    info = path.stat()
    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
        raise ValueError(f"artifact must be a file or capture directory: {path}")
    actual, size, digest_kind = artifact_identity(path)
    if ref.get("sha256") is not None and ref["sha256"] != actual:
        raise ValueError(f"artifact digest mismatch: {path}")
    return _insert(db, "artifact", role=ref["role"], path=str(path), sha256=actual,
                   bytes=size, digest_kind=digest_kind, status="retained", collector=ref.get("collector"),
                   reason=ref.get("reason"))


def _diagnostic_log(db: sqlite3.Connection, artifact_id: int, path: Path) -> None:
    """Logs without clocks stay keyed by source line, never fake timeline time."""
    if path.stat().st_size > MAX_INPUT_BYTES:
        raise ValueError("diagnostic log exceeds byte budget")
    rows = 0
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            category = re.search(r"\[(dxmt9-(?:perf|pe|bridge|queue|encode|scheduling)[^\] ]*)", line)
            if not category:
                continue
            rows += 1
            if rows > MAX_EVENTS:
                raise ValueError("diagnostic log exceeds row budget")
            row_id = _insert(db, "diagnostic_row", artifact_id=artifact_id,
                             line_number=line_number, category=category[1], record_text=line.rstrip("\n"))
            values = dict(re.findall(r"([A-Za-z_][A-Za-z_0-9.]*)=([^\s\]]+)", line))
            for key, value in values.items():
                _insert(db, "diagnostic_value", row_id=row_id, key=key, value_exact=value)


def _structured_artifact(db: sqlite3.Connection, aid: int, path: Path) -> None:
    """Preserve JSON trees and CSV rows without guessing clocks or units."""
    if path.suffix.lower() not in (".json", ".csv"):
        return
    if path.stat().st_size > MAX_INPUT_BYTES:
        raise ValueError("structural artifact exceeds byte budget")
    before = digest(path)
    count = 0
    def insert(key, value):
        nonlocal count
        count += 1
        if count > MAX_EVENTS:
            raise ValueError("structural artifact exceeds row budget")
        _insert(db, "structural_value", artifact_id=aid, path=key, value_json=encoded(value))
    if path.suffix.lower() == ".json":
        document = json.loads(path.read_text())
        pending = [("", document)]
        while pending:
            key, value = pending.pop()
            if isinstance(value, dict) and value:
                pending.extend((key + "/" + str(k).replace("~", "~0").replace("/", "~1"), v)
                               for k, v in value.items())
            elif isinstance(value, list) and value:
                pending.extend((key + "/" + str(i), v) for i, v in enumerate(value))
            else:
                insert(key, value)
    else:
        with path.open(newline="") as stream:
            for i, row in enumerate(csv.reader(stream)):
                insert("/" + str(i), row)
    if before != digest(path):
        raise ValueError("structural input changed during import")


def _shared_reports(db, bid, artifact_id, observations):
    groups = {}
    for event, eid in observations:
        a = event.get("attrs", {})
        if "diagnostic_parts" not in a:
            continue
        encoding = int(a["diagnostic_encoding"])
        key = ((event["process_id"], a.get("guest_process_id"), a.get("process_create_time100ns")), a["observation_source"], a["observation_record"],
               a["diagnostic_schema"], encoding,
               eid if encoding == 3 else None)
        groups.setdefault(key, []).append((event, eid))
    artifacts = {}
    for (_, source, record, schema, encoding, _), rows in groups.items():
        attrs = [e["attrs"] for e, _ in rows]
        count = int(attrs[0]["diagnostic_parts"])
        complete = (0 < count <= 64 and len(rows) == count and
                    all(int(a["diagnostic_parts"]) == count for a in attrs) and
                    sorted(int(a["diagnostic_part"]) for a in attrs) == list(range(count)))
        attrs.sort(key=lambda a: int(a["diagnostic_part"]))
        payload_bytes = bytes.fromhex("".join(a["diagnostic_payload"] for a in attrs))
        payload = payload_bytes.decode("utf-8", errors="backslashreplace")
        rid = _insert(db, "observation_report", bundle_id=bid, source_event_id=rows[0][1],
                      source=source, source_record=record, schema=schema, encoding=encoding,
                      status="complete" if complete else "incomplete", payload=payload, payload_bytes=payload_bytes,
                      value_exact=attrs[0]["diagnostic_value"] if encoding == 2 else None)
        if not complete:
            _insert(db, "qualification", bundle_id=bid, scope="shared-report:" + str(rid),
                    status="unqualified", details_json=encoded({"reason": "fragment coverage"}))
            continue
        values = [(schema, attrs[0]["diagnostic_value"])] if encoding == 2 else re.findall(
            r"([A-Za-z_][A-Za-z_0-9.]*)=([^\s]+)", payload)
        for i, (key, value) in enumerate(values):
            _insert(db, "observation_field", report_id=rid, ordinal=i, key=key, value_exact=value)
        if source in ("cpu-detail", "wait-detail") and ("qualified", "0") in values:
            _insert(db, "qualification", bundle_id=bid, scope="endpoint-clock:" + str(rid),
                    status="unqualified", details_json=encoded({"reason": "endpoint bracket"}))
        if schema == "dxmt9-artifact-v1":
            path = safe_path(Path(payload))
            if path not in artifacts:
                if len(artifacts) >= 1024:
                    raise ValueError("structural artifact count exceeds budget")
                aid = _artifact(db, {"role": "renderer-structure", "path": str(path),
                                    "required": False, "reason": "shared producer output"})
                artifacts[path] = aid
                if path.is_file():
                    before = db.execute("SELECT sha256 FROM artifact WHERE id=?", (aid,)).fetchone()[0]
                    _structured_artifact(db, aid, path)
                    if before != digest(path):
                        raise ValueError("structural input changed after hashing")
            _insert(db, "artifact_link", artifact_id=artifacts[path], source_event_id=rows[0][1],
                    relation="produced", evidence="shared-output-path-v1")


def _bundle(db: sqlite3.Connection, role: str, path: Path, run_id: str) -> tuple[int, dict[str, int]]:
    path = safe_path(path)
    if path.stat().st_size > MAX_INPUT_BYTES:
        raise ValueError("normalized input exceeds byte budget")
    before = digest(path)
    value = adapt(path, "auto", run_id=run_id, process_incarnation="unknown")
    if value["run_id"] != run_id:
        raise ValueError("bundle run identity mismatch")
    validity = validate_bundle(value)
    if len(value["events"]) > MAX_EVENTS:
        raise ValueError("normalized input exceeds event budget")
    artifact_id = _artifact(db, {"role": role, "path": str(path), "sha256": before})
    envelope = {k: v for k, v in value.items() if k != "events"}
    bid = _insert(db, "bundle", artifact_id=artifact_id, run_id=run_id,
                  validity_json=encoded(validity), envelope_json=encoded(envelope))
    try:
        validate_bundle(value, require_complete=True)
        status, details = "qualified", validity
    except ValueError as exc:
        status, details = "unqualified", {"reason": str(exc), "validity": validity}
    _insert(db, "qualification", bundle_id=bid, scope="normalized-source-coverage",
            status=status, details_json=encoded(details))
    clocks = {c["clock_id"]: _insert(db, "clock", bundle_id=bid,
        source_id=c["clock_id"], unit=c["unit"], alignment=c["alignment"],
        definition_json=encoded(c)) for c in value["clocks"]}
    threads = {t["thread_id"]: t for t in value["threads"]}
    tracks: dict[tuple, int] = {}
    events: dict[str, int] = {}
    scopes: dict[tuple, dict[str, dict]] = {}
    for e in value["events"]:
        attrs = e.get("attrs", {})
        is_counter = e["kind"] == "counter"
        name = str(attrs.get("track_name", attrs.get("name", e["kind"])))
        key = (e["thread_id"], e["clock_id"], name if is_counter else "", is_counter,
               attrs.get("owner_id") if is_counter else None, attrs.get("unit") if is_counter else None)
        if key not in tracks:
            tracks[key] = _insert(db, "track", bundle_id=bid,
                source_id=str(e.get("track_id", e["thread_id"])),
                name=name if is_counter else str(threads[e["thread_id"]].get("name", e["thread_id"])),
                type="counter" if is_counter else e.get("track_kind", "thread"),
                unit=attrs.get("unit") if is_counter else None,
                process_id=e["process_id"], thread_id=e["thread_id"],
                clock_id=clocks[e["clock_id"]], definition_json=encoded(threads[e["thread_id"]]))
        tid, cid, ts = tracks[key], clocks[e["clock_id"]], integer(e["timestamp"])
        eid = _insert(db, "source_event", bundle_id=bid, source_id=e["event_id"],
            source_record=e["source_record"], collector=e["collector"], track_id=tid,
            clock_id=cid, ts=ts, timestamp_exact=e["timestamp"], kind=e["kind"], record_json=encoded(e))
        events[e["event_id"]] = eid
        for k, v in attrs.items():
            _insert(db, "args", arg_set_id=eid, key=k, value_json=encoded(v))
        if is_counter:
            exact = str(attrs.get("counter_value", attrs.get("value", "")))
            if not exact:
                raise ValueError("counter lacks value")
            approximate = float(exact)
            if not math.isfinite(approximate):
                raise ValueError("nonfinite counter value")
            _insert(db, "counter", track_id=tid, clock_id=cid, ts=ts, value=approximate,
                    value_exact=exact, arg_set_id=eid)
        elif e["kind"] == "instant":
            _insert(db, "slice", track_id=tid, clock_id=cid, ts=ts, dur=0,
                    name=name, arg_set_id=eid, status="instant")
        elif e["kind"] in ("scope_begin", "scope_end"):
            key = (e["process_id"], e["thread_id"], e["clock_id"], e["scope_id"])
            scopes.setdefault(key, {})[e["kind"]] = {"event": e, "id": eid, "track": tid}
        cpu = attrs.get("thread_cpu_counter_ns")
        if cpu is not None:
            cpu_key = (e["thread_id"], e["clock_id"], "native_thread_cpu_ns", True)
            if cpu_key not in tracks:
                tracks[cpu_key] = _insert(db, "track", bundle_id=bid,
                    source_id=e["thread_id"], name="native_thread_cpu_ns", type="counter", unit="ns",
                    process_id=e["process_id"], thread_id=e["thread_id"], clock_id=cid,
                    definition_json=encoded({"kind": attrs.get("thread_cpu_kind"), "source": "cpu-supply-v2"}))
            _insert(db, "counter", track_id=tracks[cpu_key], clock_id=cid, ts=ts,
                    value=float(cpu), value_exact=str(cpu), arg_set_id=eid)
    for pair in scopes.values():
        begin, end = pair.get("scope_begin"), pair.get("scope_end")
        item = begin or end
        e = item["event"]
        duration = integer(int(end["event"]["timestamp"]) - int(begin["event"]["timestamp"])) if begin and end else None
        _insert(db, "slice", track_id=item["track"], clock_id=clocks[e["clock_id"]],
            ts=integer(e["timestamp"]) if begin else None, dur=duration,
            name=str(e.get("attrs", {}).get("name", e["scope_id"])), arg_set_id=item["id"],
            end_event_id=end["id"] if end else None,
            status="complete" if begin and end else "missing_end" if begin else "missing_begin")
    for relation in value["relations"]:
        _insert(db, "flow", bundle_id=bid,
                source_event_id=events.get(relation.get("source_event")),
                target_event_id=events.get(relation.get("target_event")),
                type=relation["type"], evidence_json=encoded(relation))
    _shared_reports(db, bid, artifact_id, [(e, events[e["event_id"]]) for e in value["events"]])
    return bid, events


def publish_store(output: Path, *, run_id: str, attempt_id: str,
                  provenance: dict[str, Any], bundles: list[tuple[str, Path]],
                  artifacts: list[dict[str, Any]]) -> dict[str, Any]:
    """Seal a bounded SQL projection. All joins are input declarations, not inference."""
    if not isinstance(run_id, str) or not run_id or not isinstance(attempt_id, str) or not attempt_id:
        raise ValueError("run and attempt identities are required")
    if not isinstance(provenance, dict) or not isinstance(artifacts, list) or not isinstance(bundles, list):
        raise ValueError("invalid observation envelope")
    for ref in artifacts:
        if not isinstance(ref, dict) or any(not isinstance(ref.get(key), str) or not ref[key]
                                            for key in ("role", "path")):
            raise ValueError("artifact requires nonempty role and path")
        if ref.get("sha256") is not None and (not isinstance(ref["sha256"], str) or
                                               re.fullmatch(r"[0-9a-f]{64}", ref["sha256"]) is None):
            raise ValueError("artifact requires a lowercase SHA-256 digest")
    if len(bundles) > 32 or len(artifacts) > 1024:
        raise ValueError("observation source budget exceeded")
    output = safe_path(output)
    if output.exists():
        raise FileExistsError(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".observation-", suffix=".sqlite", dir=output.parent)
    os.close(fd)
    temp = Path(temporary)
    db = None
    try:
        db = sqlite3.connect(temp)
        db.executescript(DDL)
        for k, v in {"schema": SCHEMA, "run_id": run_id, "attempt_id": attempt_id,
                     "producer_sha256": digest(Path(__file__)),
                     "normalizer_sha256": digest(ROOT / "scripts/tools/tracing_normalize.py")}.items():
            _insert(db, "metadata", key=k, value=v)
        for k, v in provenance.items():
            _insert(db, "provenance", key=k, value_json=encoded(v))
        test_results = provenance.get("test_results", {})
        if not isinstance(test_results, dict):
            raise ValueError("test_results must be a name-to-verdict mapping")
        for name, verdict in test_results.items():
            status = verdict if isinstance(verdict, str) else verdict.get("status")
            if not isinstance(status, str) or not status:
                raise ValueError("test verdict must declare a status")
            _insert(db, "test_result", name=name, status=status, details_json=encoded(verdict))
        by_role = {}
        for role, path in bundles:
            if role in by_role:
                raise ValueError("duplicate bundle role")
            by_role[role] = _bundle(db, role, path, run_id)[1]
        for ref in artifacts:
            aid = _artifact(db, ref)
            if ref.get("format") == "dxmt9-log" and Path(ref["path"]).exists():
                before = db.execute("SELECT sha256 FROM artifact WHERE id=?", (aid,)).fetchone()[0]
                _diagnostic_log(db, aid, Path(ref["path"]))
                if before != digest(Path(ref["path"])):
                    raise ValueError("diagnostic input changed during import")
            event_id = ref.get("event_id")
            endpoint = None
            if event_id is not None:
                endpoint = by_role.get(ref.get("bundle_role", "engine"), {}).get(str(event_id))
                if endpoint is None:
                    raise ValueError("supplemental artifact references unknown event")
            if event_id is not None or ref.get("work_id") is not None:
                if not ref.get("evidence"):
                    raise ValueError("artifact link requires explicit evidence")
                if ref.get("work_id") is not None and not ref.get("owner_id"):
                    raise ValueError("work link requires owner identity")
                _insert(db, "artifact_link", artifact_id=aid, source_event_id=endpoint,
                        work_id=ref.get("work_id"), owner_id=ref.get("owner_id"),
                        relation=ref.get("relation", "annotates"), evidence=ref["evidence"])
        db.commit()
        if db.execute("PRAGMA integrity_check").fetchall() != [("ok",)] or db.execute("PRAGMA foreign_key_check").fetchall():
            raise ValueError("observation database integrity failure")
        db.close()
        db = None
        with temp.open("rb") as stream:
            os.fsync(stream.fileno())
        # Exclusive hard-link publication, including racing attempts.
        os.link(temp, output)
        return {"schema": SCHEMA, "path": str(output), "sha256": digest(output),
                "bytes": output.stat().st_size, "status": "published"}
    finally:
        if db is not None:
            db.close()
        temp.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--attempt-id", required=True)
    parser.add_argument("--bundle", action="append", default=[], metavar="ROLE=PATH")
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, help="JSON list of hashed artifact references and explicit links")
    args = parser.parse_args()
    try:
        bundles = [(role, Path(path)) for role, path in (v.split("=", 1) for v in args.bundle)]
        result = publish_store(args.output, run_id=args.run_id, attempt_id=args.attempt_id,
            provenance=json.loads(args.provenance.read_text()), bundles=bundles,
            artifacts=json.loads(args.artifacts.read_text()) if args.artifacts else [])
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, sqlite3.Error) as exc:
        print(f"observation store failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
