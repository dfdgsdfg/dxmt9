#!/usr/bin/env python3
"""Cold authentication of capture provenance and actual queue publications.

The canonical digest is isomorphic to device_c_render_tape_source_plan.cpp /
digestPlan. Captured identities only qualify record ranges; queue identities
remain local execution facts and are never rebased to captured sequence IDs.
"""

from __future__ import annotations

import hashlib
import struct
from typing import Any


CAPTURE_KEYS = ("event_ordinal", "source_ordinal", "captured_seq_id",
                "command_chunk_index", "first_record", "record_count")
PLAN_DOMAIN = b"dxmt9.capture-source-replay-plan"
MAX_RAW_RECORDS = 4096


def _uint(value: Any, bits: int = 64, minimum: int = 0) -> bool:
    return type(value) is int and minimum <= value < 1 << bits


def _digest(value: Any) -> bool:
    return (type(value) is str and len(value) == 64
            and all(char in "0123456789abcdef" for char in value))


def source_plan_digest(components: Any, rows: Any) -> str | None:
    """Recompute the native plan's exact digest, not a hash of JSON spelling."""
    if not isinstance(components, dict) or not isinstance(rows, list) or not rows:
        return None
    if len(rows) > MAX_RAW_RECORDS:
        return None
    identities = []
    for name, filename in (("events", "events.bin"), ("identity", "identity.bin")):
        component = components.get(name)
        if (not isinstance(component, dict) or component.get("path") != filename
                or not _uint(component.get("bytes"), minimum=1)
                or not _digest(component.get("sha256"))):
            return None
        identities.append(bytes.fromhex(component["sha256"]))
    digest = hashlib.sha256(PLAN_DOMAIN + struct.pack("<I", 1)
                            + b"".join(identities) + struct.pack("<I", len(rows)))
    previous_event = previous_source = previous_seq = 0
    chunk_index = -1
    next_record = 0
    for row in rows:
        if not isinstance(row, dict):
            return None
        values = [row.get(key) for key in CAPTURE_KEYS]
        if any(not _uint(value, 64 if index < 3 else 32,
                         1 if index < 3 or index == 5 else 0)
               for index, value in enumerate(values)):
            return None
        event, source, seq, chunk, first, count = values
        if source <= previous_source or seq <= previous_seq:
            return None
        if event != previous_event:
            if event <= previous_event or chunk != chunk_index + 1 or first != 0:
                return None
            chunk_index = chunk
            next_record = 0
        elif chunk != chunk_index:
            return None
        if first != next_record or first + count > MAX_RAW_RECORDS:
            return None
        next_record = first + count
        previous_event, previous_source, previous_seq = event, source, seq
        digest.update(struct.pack("<QQQIII", *values))
    return digest.hexdigest()


def source_replay_accepts(value: Any, policy: Any, components: Any) -> bool:
    """Require exact input-qualified rows and fresh local publication evidence."""
    if (not isinstance(policy, dict)
            or policy.get("schema") != "dxmt9.render_tape.cpu_policy.v2"
            or policy.get("source_boundary_mode") != "capture-source"
            or not _digest(policy.get("source_plan_sha256"))
            or not isinstance(value, dict)
            or any(value.get(key) is not True for key in ("requested", "validated", "complete"))
            or value.get("storage_overflow") is not False
            or value.get("plan_sha256") != policy["source_plan_sha256"]):
        return False
    rows = value.get("sources")
    plan_digest = source_plan_digest(components, rows)
    if plan_digest is None or plan_digest != policy["source_plan_sha256"]:
        return False
    count_keys = ("source_count", "completed_source_count", "publication_count",
                  "empty_source_count", "present_source_count")
    if (any(not _uint(value.get(key)) for key in count_keys)
            or value["source_count"] != len(rows)
            or value["completed_source_count"] != len(rows)):
        return False
    publications = empty = present = 0
    previous_chunk = -1
    previous_raw = previous_seq = 0
    range_index = 0
    source_keys: set[tuple[int, int]] = set()
    storage_ranges: dict[int, list[tuple[int, int]]] = {}
    for row in rows:
        raw = row.get("local_raw_ordinal")
        if (row.get("settled") is not True or not _uint(raw, minimum=1)
                or not _uint(row.get("range_index"), 32)):
            return False
        if row["command_chunk_index"] != previous_chunk:
            if raw <= previous_raw:
                return False
            previous_chunk = row["command_chunk_index"]
            previous_raw = raw
            range_index = 0
        if raw != previous_raw or row["range_index"] != range_index:
            return False
        range_index += 1
        receipts = row.get("publications")
        if (not isinstance(receipts, list)
                or len(receipts) > row["record_count"] * 2 + 1):
            return False
        kind = row.get("settlement")
        if kind not in ("empty", "published", "present-published"):
            return False
        if (kind == "empty") != (len(receipts) == 0):
            return False
        row_present = False
        for receipt in receipts:
            if not isinstance(receipt, dict):
                return False
            positive64 = ("source_generation", "storage_generation", "source_ordinal", "seq_id")
            unsigned64 = ("raw_ordinal", "build_generation", "used_bytes", "control_index")
            unsigned32 = ("source_index", "storage_first_page", "padding_pages_before")
            if (any(not _uint(receipt.get(key), minimum=1) for key in positive64)
                    or any(not _uint(receipt.get(key)) for key in unsigned64)
                    or any(not _uint(receipt.get(key), 32) for key in unsigned32)
                    or any(not _uint(receipt.get(key), 32, 1)
                           for key in ("page_count", "storage_page_count"))
                    or type(receipt.get("strict_admission")) is not bool
                    or type(receipt.get("has_present")) is not bool):
                return False
            seq = receipt["seq_id"]
            first, pages = receipt["storage_first_page"], receipt["storage_page_count"]
            key = receipt["source_index"], receipt["source_generation"]
            generation = receipt["storage_generation"]
            if (seq <= previous_seq or receipt["source_ordinal"] != seq
                    or receipt["raw_ordinal"] != 0
                    or receipt["page_count"] != pages or first + pages > 1 << 32
                    or receipt["source_index"] == (1 << 32) - 1
                    or receipt["control_index"] == (1 << 64) - 1
                    or key in source_keys
                    or any(first < end and start < first + pages
                           for start, end in storage_ranges.get(generation, []))):
                return False
            previous_seq = seq
            source_keys.add(key)
            storage_ranges.setdefault(generation, []).append((first, first + pages))
            row_present |= receipt["has_present"]
        if (kind == "present-published") != row_present:
            return False
        publications += len(receipts)
        empty += kind == "empty"
        present += kind == "present-published"
    return (value["publication_count"] == publications
            and value["empty_source_count"] == empty
            and value["present_source_count"] == present)


def cpu_source_trace_matches(iteration: Any) -> bool:
    """Join the source driver witness to the actual CPU completion trace."""
    if not isinstance(iteration, dict):
        return False
    evidence, trace = iteration.get("source_replay"), iteration.get("trace")
    if not isinstance(evidence, dict) or not isinstance(trace, list):
        return False
    sources = evidence.get("sources")
    if not isinstance(sources, list):
        return False
    by_seq = {}
    for row in trace:
        if (not isinstance(row, dict) or not _uint(row.get("submission"), minimum=1)
                or row["submission"] in by_seq):
            return False
        by_seq[row["submission"]] = row
    matched = set()
    for source in sources:
        if not isinstance(source, dict) or not isinstance(source.get("publications"), list):
            return False
        for publication in source["publications"]:
            if not isinstance(publication, dict):
                return False
            seq = publication.get("seq_id")
            if not _uint(seq, minimum=1) or seq in matched:
                return False
            row = by_seq.get(seq)
            if (row is None or row.get("submitted") is not True
                    or row.get("completed") is not True
                    or row.get("event") != source["event_ordinal"]
                    or row.get("source") != publication.get("source_index")
                    or any(row.get(key) != publication.get(key) for key in (
                        "source_generation", "storage_first_page", "storage_page_count",
                        "storage_generation"))):
                return False
            matched.add(seq)
    # Bootstrap/resource events can legitimately publish outside the source
    # plan. No extra trace may claim one of its command events, however.
    command_events = {source["event_ordinal"] for source in sources}
    return all(row.get("event") not in command_events or seq in matched
               for seq, row in by_seq.items())
