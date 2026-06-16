#!/usr/bin/env python3
"""Summarize xctrace CPU thread samples for 3DMark05 pacing probes.

This consumes XML tables exported from Instruments / xctrace. The main input is
`time-profile`, which carries symbolicated sampled stacks. Optional
`time-sample` input adds thread-state distribution from kperf samples.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path
from typing import Iterable


DEFAULT_KEYWORDS = [
    "OnMainThread",
    "kevent",
    "dispatch_semaphore_wait",
    "macdrv_clip_cursor",
    "macdrv_get_cursor_position",
    "macdrv_set_cursor_position",
    "macdrv_get_cocoa_window_frame",
    "macdrv_view_get_metal_layer",
    "ClipCursor",
    "GetCursorPos",
    "SetCursorPos",
    "CA::Transaction",
    "CAMetalLayer",
    "presentDrawable",
    "nextDrawable",
]

P4_WAIT_KEYWORDS = [
    "OnMainThread",
    "kevent",
    "dispatch_semaphore_wait",
    "macdrv_clip_cursor",
    "macdrv_get_cursor_position",
    "macdrv_set_cursor_position",
    "macdrv_get_cocoa_window_frame",
    "macdrv_view_get_metal_layer",
    "ClipCursor",
    "GetCursorPos",
    "SetCursorPos",
]

P4_HOLDER_KEYWORDS = [
    "CA::Transaction",
    "CAMetalLayer",
    "presentDrawable",
    "nextDrawable",
]

PE_THREAD_ID_RE = re.compile(r"\bthread_id=(0x[0-9a-fA-F]+)\b")
NATIVE_THREAD_ID_RE = re.compile(r"\bnative_tid=(0x[0-9a-fA-F]+)\b")

BLOCKED_STATE_SUBSTRINGS = [
    "block",
    "wait",
    "sleep",
    "park",
]


def parse_duration_ms(value: str, fallback_text: str = "") -> float:
    text = (value or "").strip().replace(",", "")
    if text:
        parts = text.split()
        try:
            amount = float(parts[0])
        except (ValueError, IndexError):
            amount = 0.0
        unit = parts[1] if len(parts) > 1 else "ms"
        if unit.startswith("ns"):
            return amount / 1_000_000.0
        if unit.startswith("us") or unit.startswith("\u00b5s"):
            return amount / 1_000.0
        if unit.startswith("ms"):
            return amount
        if unit.startswith("s"):
            return amount * 1_000.0
        if amount:
            return amount
    try:
        return float((fallback_text or "").strip() or 0.0) / 1_000_000.0
    except ValueError:
        return 0.0


def load_xml(path: Path) -> tuple[ET.Element, dict[str, ET.Element]]:
    root = ET.parse(path).getroot()
    ids: dict[str, ET.Element] = {}
    for elem in root.iter():
        elem_id = elem.get("id")
        if elem_id:
            ids[elem_id] = elem
    return root, ids


def resolve(elem: ET.Element, ids: dict[str, ET.Element]) -> ET.Element:
    ref = elem.get("ref")
    if ref:
        return ids.get(ref, elem)
    return elem


def value(elem: ET.Element | None, ids: dict[str, ET.Element]) -> str:
    if elem is None:
        return ""
    resolved = resolve(elem, ids)
    return resolved.get("fmt") or (resolved.text or "")


def child(row: ET.Element, tag: str) -> ET.Element | None:
    for elem in row:
        if elem.tag == tag:
            return elem
    return None


def iter_frames(elem: ET.Element | None, ids: dict[str, ET.Element]) -> Iterable[tuple[str, str]]:
    if elem is None:
        return
    resolved = resolve(elem, ids)
    for frame in resolved.iter("frame"):
        actual = resolve(frame, ids)
        name = actual.get("name") or ""
        binary = ""
        binary_elem = child(actual, "binary")
        if binary_elem is not None:
            binary_actual = resolve(binary_elem, ids)
            binary = binary_actual.get("name") or binary_actual.get("path") or ""
        yield name, binary


def thread_key(thread: str) -> str:
    return thread or "<unknown-thread>"


def make_thread_row(thread: str) -> dict[str, object]:
    return {
        "thread": thread,
        "pid": "",
        "tid": "",
        "thread_info_name": "",
        "is_main_thread": "",
        "thread_info_rows": 0,
        "time_profile_rows": 0,
        "time_profile_weight_ms": 0.0,
        "time_profile_states": Counter(),
        "time_sample_rows": 0,
        "time_sample_states": Counter(),
        "top_frames": Counter(),
        "top_binaries": Counter(),
        "keyword_hits": Counter(),
        "keyword_weight_ms": Counter(),
    }


def parse_time_profile(
    path: Path,
    *,
    process_re: re.Pattern[str],
    keywords: list[str],
) -> tuple[dict[str, dict[str, object]], Counter[str], Counter[str], int]:
    root, ids = load_xml(path)
    threads: dict[str, dict[str, object]] = {}
    keyword_samples: Counter[str] = Counter()
    keyword_weight_ms: Counter[str] = Counter()
    matched_rows = 0
    lowered = [(keyword, keyword.lower()) for keyword in keywords]
    for row in root.iter("row"):
        process = value(child(row, "process"), ids)
        thread = value(child(row, "thread"), ids)
        if not (process_re.search(process) or process_re.search(thread)):
            continue
        matched_rows += 1
        key = thread_key(thread)
        thread_row = threads.setdefault(key, make_thread_row(thread))
        weight_elem = child(row, "weight")
        weight_actual = resolve(weight_elem, ids) if weight_elem is not None else None
        weight_ms = parse_duration_ms(
            weight_actual.get("fmt") if weight_actual is not None else "",
            weight_actual.text if weight_actual is not None else "",
        )
        state = value(child(row, "thread-state"), ids) or "<unknown-state>"
        frames = list(iter_frames(child(row, "tagged-backtrace"), ids))
        stack_text = "\n".join(f"{frame} {binary}" for frame, binary in frames).lower()

        thread_row["time_profile_rows"] = int(thread_row["time_profile_rows"]) + 1
        thread_row["time_profile_weight_ms"] = float(thread_row["time_profile_weight_ms"]) + weight_ms
        thread_row["time_profile_states"][state] += 1  # type: ignore[index]
        if frames:
            top_frame, top_binary = frames[0]
            thread_row["top_frames"][top_frame or "<unknown-frame>"] += 1  # type: ignore[index]
            thread_row["top_binaries"][top_binary or "<unknown-binary>"] += 1  # type: ignore[index]
        for keyword, lowered_keyword in lowered:
            if lowered_keyword in stack_text:
                thread_row["keyword_hits"][keyword] += 1  # type: ignore[index]
                thread_row["keyword_weight_ms"][keyword] += weight_ms  # type: ignore[index]
                keyword_samples[keyword] += 1
                keyword_weight_ms[keyword] += weight_ms
    return threads, keyword_samples, keyword_weight_ms, matched_rows


def parse_time_sample(
    path: Path,
    *,
    process_re: re.Pattern[str],
    threads: dict[str, dict[str, object]],
) -> int:
    root, ids = load_xml(path)
    matched_rows = 0
    for row in root.iter("row"):
        thread = value(child(row, "thread"), ids)
        if not process_re.search(thread):
            continue
        matched_rows += 1
        key = thread_key(thread)
        thread_row = threads.setdefault(key, make_thread_row(thread))
        state = value(child(row, "thread-state"), ids) or "<unknown-state>"
        thread_row["time_sample_rows"] = int(thread_row["time_sample_rows"]) + 1
        thread_row["time_sample_states"][state] += 1  # type: ignore[index]
    return matched_rows


def parse_thread_info(
    path: Path,
    *,
    process_re: re.Pattern[str],
    threads: dict[str, dict[str, object]],
) -> int:
    root, ids = load_xml(path)
    matched_rows = 0
    for row in root.iter("row"):
        process = value(child(row, "process"), ids)
        thread = value(child(row, "thread"), ids)
        if not (process_re.search(process) or process_re.search(thread)):
            continue
        matched_rows += 1
        key = thread_key(thread)
        thread_row = threads.setdefault(key, make_thread_row(thread))
        thread_row["thread_info_rows"] = int(thread_row["thread_info_rows"]) + 1
        thread_row["pid"] = value(child(row, "pid"), ids)
        thread_row["tid"] = value(child(row, "tid"), ids)
        thread_row["thread_info_name"] = value(child(row, "thread-name"), ids)
        thread_row["is_main_thread"] = value(child(row, "boolean"), ids)
    return matched_rows


def counter_summary(counter: Counter[str], limit: int = 3) -> str:
    return "; ".join(f"{name}={count}" for name, count in counter.most_common(limit))


def blocked_state_rows(states: Counter[str]) -> int:
    total = 0
    for state, count in states.items():
        lowered = state.lower()
        if any(fragment in lowered for fragment in BLOCKED_STATE_SUBSTRINGS):
            total += count
    return total


def running_state_rows(states: Counter[str]) -> int:
    total = 0
    for state, count in states.items():
        if "running" in state.lower():
            total += count
    return total


def is_main_thread_value(value: str) -> bool:
    lowered = value.strip().lower()
    return lowered in {"1", "true", "yes", "y"} or "main" in lowered


def producer_thread_regex_from_pe_log(path: Path) -> tuple[str, str]:
    native_commit_threads: Counter[str] = Counter()
    buckets = [
        ("pe-log-clear-return", Counter()),
        ("pe-log-clear-entry", Counter()),
        ("pe-log-setrt-return", Counter()),
        ("pe-log-next-call", Counter()),
        ("pe-log-present", Counter()),
        ("pe-log-any", Counter()),
    ]
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            native_match = NATIVE_THREAD_ID_RE.search(line)
            if native_match and "unix_commit_chunk_entry" in line:
                native_thread_id = native_match.group(1)
                if native_thread_id != "0x0":
                    native_commit_threads[native_thread_id] += 1
            match = PE_THREAD_ID_RE.search(line)
            if not match:
                continue
            thread_id = match.group(1)
            buckets[-1][1][thread_id] += 1
            if "pe_present_call_return" in line and "call=Clear" in line:
                buckets[0][1][thread_id] += 1
            if "pe_present_call_milestone" in line and "call=Clear" in line:
                buckets[1][1][thread_id] += 1
            if "pe_present_call_return" in line and "call=SetRenderTarget" in line:
                buckets[2][1][thread_id] += 1
            if "pe_present_next_call" in line:
                buckets[3][1][thread_id] += 1
            if "pe_present_timing" in line:
                buckets[4][1][thread_id] += 1
    if native_commit_threads:
        return native_commit_threads.most_common(1)[0][0], "native-log-commit-chunk-entry"
    for source, counter in buckets:
        if counter:
            return counter.most_common(1)[0][0], source
    return "", "pe-log-no-thread-id"


def rows_for_output(threads: dict[str, dict[str, object]]) -> list[dict[str, str]]:
    output: list[dict[str, str]] = []
    for row in sorted(
        threads.values(),
        key=lambda item: (float(item["time_profile_weight_ms"]), int(item["time_sample_rows"])),
        reverse=True,
    ):
        keyword_hits: Counter[str] = row["keyword_hits"]  # type: ignore[assignment]
        keyword_weight: Counter[str] = row["keyword_weight_ms"]  # type: ignore[assignment]
        time_sample_states: Counter[str] = row["time_sample_states"]  # type: ignore[assignment]
        wait_keyword_hits = sum(keyword_hits[keyword] for keyword in P4_WAIT_KEYWORDS)
        wait_keyword_weight_ms = sum(keyword_weight[keyword] for keyword in P4_WAIT_KEYWORDS)
        holder_keyword_hits = sum(keyword_hits[keyword] for keyword in P4_HOLDER_KEYWORDS)
        holder_keyword_weight_ms = sum(keyword_weight[keyword] for keyword in P4_HOLDER_KEYWORDS)
        sample_blocked_rows = blocked_state_rows(time_sample_states)
        sample_running_rows = running_state_rows(time_sample_states)
        output.append(
            {
                "thread": str(row["thread"]),
                "pid": str(row["pid"]),
                "tid": str(row["tid"]),
                "thread_info_name": str(row["thread_info_name"]),
                "is_main_thread": str(row["is_main_thread"]),
                "thread_info_rows": str(row["thread_info_rows"]),
                "time_profile_rows": str(row["time_profile_rows"]),
                "time_profile_weight_ms": f"{float(row['time_profile_weight_ms']):.3f}",
                "time_profile_states": counter_summary(row["time_profile_states"]),  # type: ignore[arg-type]
                "time_sample_rows": str(row["time_sample_rows"]),
                "time_sample_states": counter_summary(row["time_sample_states"]),  # type: ignore[arg-type]
                "top_frames": counter_summary(row["top_frames"]),  # type: ignore[arg-type]
                "top_binaries": counter_summary(row["top_binaries"]),  # type: ignore[arg-type]
                "keyword_hits": counter_summary(keyword_hits, 8),
                "keyword_weight_ms": "; ".join(
                    f"{name}={keyword_weight[name]:.3f}" for name, _ in keyword_hits.most_common(8)
                ),
                "p4_wait_keyword_hits": str(wait_keyword_hits),
                "p4_wait_keyword_weight_ms": f"{wait_keyword_weight_ms:.3f}",
                "p4_holder_keyword_hits": str(holder_keyword_hits),
                "p4_holder_keyword_weight_ms": f"{holder_keyword_weight_ms:.3f}",
                "time_sample_running_rows": str(sample_running_rows),
                "time_sample_blocked_rows": str(sample_blocked_rows),
            }
        )
    return output


def row_matches_producer(row: dict[str, str], producer_thread_re: re.Pattern[str]) -> bool:
    haystacks = [
        row.get("thread", ""),
        row.get("tid", ""),
        row.get("pid", ""),
        row.get("thread_info_name", ""),
    ]
    return any(producer_thread_re.search(value) for value in haystacks if value)


def p4_holder_summary(rows: list[dict[str, str]]) -> dict[str, str]:
    holder_thread_count = sum(1 for row in rows if int(row["p4_holder_keyword_hits"]) > 0)
    holder_hits = sum(int(row["p4_holder_keyword_hits"]) for row in rows)
    main_holder_thread_count = sum(
        1
        for row in rows
        if int(row["p4_holder_keyword_hits"]) > 0 and is_main_thread_value(row.get("is_main_thread", ""))
    )
    main_holder_hits = sum(
        int(row["p4_holder_keyword_hits"])
        for row in rows
        if is_main_thread_value(row.get("is_main_thread", ""))
    )
    if main_holder_hits:
        status = "main-thread-holder-positive"
        reason = "Main-thread samples include CoreAnimation/CAMetalLayer/present holder keywords."
    elif holder_hits:
        status = "holder-positive-non-main-thread"
        reason = "Holder keywords were sampled, but not on a thread marked as main by thread-info."
    else:
        status = "holder-not-sampled"
        reason = "No CoreAnimation/CAMetalLayer/present holder keyword was sampled."
    return {
        "holder_status": status,
        "holder_keyword_thread_count": str(holder_thread_count),
        "holder_keyword_hits": str(holder_hits),
        "main_thread_holder_keyword_thread_count": str(main_holder_thread_count),
        "main_thread_holder_keyword_hits": str(main_holder_hits),
        "holder_reason": reason,
    }


def p4_scout_verdict(
    rows: list[dict[str, str]],
    *,
    producer_thread_re: re.Pattern[str] | None = None,
    producer_thread_regex: str = "",
    producer_selection_source: str = "",
    producer_selection_required: bool = False,
) -> dict[str, str]:
    selection_source = producer_selection_source or (
        "explicit-regex" if producer_thread_regex else "auto-highest-weight"
    )
    selection = producer_thread_regex or "auto-highest-weight"
    holder = p4_holder_summary(rows)
    if not rows:
        return {
            "status": "no-target-process-rows",
            "producer_thread": "",
            "producer_selection": selection,
            "producer_selection_source": selection_source,
            "producer_selection_required": str(int(producer_selection_required)),
            "wait_keyword_thread_count": "0",
            "nonproducer_wait_keyword_hits": "0",
            "reason": "No rows matched the target process filter.",
            **holder,
        }

    if producer_selection_required and not producer_thread_regex:
        total_wait_thread_count = sum(1 for row in rows if int(row["p4_wait_keyword_hits"]) > 0)
        total_wait_hits = sum(int(row["p4_wait_keyword_hits"]) for row in rows)
        return {
            "status": "producer-thread-selector-missing",
            "producer_thread": "",
            "producer_selection": "",
            "producer_selection_source": selection_source,
            "producer_selection_required": "1",
            "producer_profile_weight_ms": "",
            "producer_wait_keyword_hits": "",
            "producer_wait_keyword_weight_ms": "",
            "producer_sample_rows": "",
            "producer_sample_running_rows": "",
            "producer_sample_blocked_rows": "",
            "wait_keyword_thread_count": str(total_wait_thread_count),
            "nonproducer_wait_keyword_hits": str(total_wait_hits),
            "reason": (
                "A producer selector was required, but no native or PE thread id was "
                "found in the direct log."
            ),
            **holder,
        }

    if producer_thread_re:
        producer = next((row for row in rows if row_matches_producer(row, producer_thread_re)), None)
        if producer is None:
            total_wait_thread_count = sum(1 for row in rows if int(row["p4_wait_keyword_hits"]) > 0)
            total_wait_hits = sum(int(row["p4_wait_keyword_hits"]) for row in rows)
            reason = "No thread matched --producer-thread-regex."
            if selection_source.startswith("pe-log-"):
                reason = (
                    "The PE log selector did not match any xctrace thread/tid. "
                    "PE thread_id is a Win32 thread id; correlate it to a native "
                    "Mach/pthread id before using it as an xctrace producer selector."
                )
            return {
                "status": "producer-thread-not-found",
                "producer_thread": "",
                "producer_tid": "",
                "producer_thread_info_name": "",
                "producer_is_main_thread": "",
                "producer_selection": producer_thread_regex,
                "producer_selection_source": selection_source,
                "producer_selection_required": str(int(producer_selection_required)),
                "producer_profile_weight_ms": "",
                "producer_wait_keyword_hits": "",
                "producer_wait_keyword_weight_ms": "",
                "producer_sample_rows": "",
                "producer_sample_running_rows": "",
                "producer_sample_blocked_rows": "",
                "wait_keyword_thread_count": str(total_wait_thread_count),
                "nonproducer_wait_keyword_hits": str(total_wait_hits),
                "reason": reason,
                **holder,
            }
    else:
        producer = rows[0]
    wait_hits = int(producer["p4_wait_keyword_hits"])
    holder_hits = int(producer["p4_holder_keyword_hits"])
    total_wait_thread_count = sum(1 for row in rows if int(row["p4_wait_keyword_hits"]) > 0)
    total_wait_hits = sum(int(row["p4_wait_keyword_hits"]) for row in rows)
    nonproducer_wait_hits = total_wait_hits - wait_hits
    nonproducer_holder_hits = int(holder["holder_keyword_hits"]) - holder_hits
    sample_rows = int(producer["time_sample_rows"])
    running_rows = int(producer["time_sample_running_rows"])
    blocked_rows = int(producer["time_sample_blocked_rows"])
    running_ratio = (running_rows / sample_rows) if sample_rows else 0.0

    producer_label = "selected producer thread" if producer_thread_re else "highest-weight producer thread"

    if wait_hits:
        status = "producer-wait-stack-positive"
        reason = f"The {producer_label} has OnMainThread/wait/candidate macdrv stack samples."
    elif sample_rows and running_ratio >= 0.9 and blocked_rows == 0:
        status = "producer-running-negative-scout"
        reason = f"The {producer_label} is sampled as mostly Running with no P4 wait keywords."
    elif sample_rows and blocked_rows:
        status = "producer-state-inconclusive"
        reason = f"The {producer_label} has blocked/waiting states but no matching wait stack keyword."
    else:
        status = "producer-stack-negative-inconclusive"
        reason = "No P4 wait keyword hit was sampled, but thread-state evidence is insufficient."

    return {
        "status": status,
        "producer_thread": producer["thread"],
        "producer_tid": producer.get("tid", ""),
        "producer_thread_info_name": producer.get("thread_info_name", ""),
        "producer_is_main_thread": producer.get("is_main_thread", ""),
        "producer_selection": selection,
        "producer_selection_source": selection_source,
        "producer_selection_required": str(int(producer_selection_required)),
        "producer_profile_weight_ms": producer["time_profile_weight_ms"],
        "producer_wait_keyword_hits": producer["p4_wait_keyword_hits"],
        "producer_wait_keyword_weight_ms": producer["p4_wait_keyword_weight_ms"],
        "producer_holder_keyword_hits": producer["p4_holder_keyword_hits"],
        "producer_holder_keyword_weight_ms": producer["p4_holder_keyword_weight_ms"],
        "producer_sample_rows": producer["time_sample_rows"],
        "producer_sample_running_rows": producer["time_sample_running_rows"],
        "producer_sample_blocked_rows": producer["time_sample_blocked_rows"],
        "wait_keyword_thread_count": str(total_wait_thread_count),
        "nonproducer_wait_keyword_hits": str(nonproducer_wait_hits),
        "nonproducer_holder_keyword_hits": str(nonproducer_holder_hits),
        "reason": reason,
        **holder,
    }


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "thread",
        "pid",
        "tid",
        "thread_info_name",
        "is_main_thread",
        "thread_info_rows",
        "time_profile_rows",
        "time_profile_weight_ms",
        "time_profile_states",
        "time_sample_rows",
        "time_sample_states",
        "top_frames",
        "top_binaries",
        "keyword_hits",
        "keyword_weight_ms",
        "p4_wait_keyword_hits",
        "p4_wait_keyword_weight_ms",
        "p4_holder_keyword_hits",
        "p4_holder_keyword_weight_ms",
        "time_sample_running_rows",
        "time_sample_blocked_rows",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_verdict_json(path: Path, verdict: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(verdict, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_md(
    path: Path,
    *,
    run_label: str,
    trace: str,
    process_regex: str,
    rows: list[dict[str, str]],
    keyword_samples: Counter[str],
    keyword_weight_ms: Counter[str],
    time_profile_rows: int,
    time_sample_rows: int,
    thread_info_rows: int,
    top: int,
    producer_thread_regex: str,
    producer_selection_source: str,
    producer_selection_required: bool,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    producer_thread_re = re.compile(producer_thread_regex) if producer_thread_regex else None
    verdict = p4_scout_verdict(
        rows,
        producer_thread_re=producer_thread_re,
        producer_thread_regex=producer_thread_regex,
        producer_selection_source=producer_selection_source,
        producer_selection_required=producer_selection_required,
    )
    lines = [
        "# xctrace CPU Thread Summary",
        "",
        f"- Run: `{run_label}`",
        f"- Trace: `{trace}`",
        f"- Process filter: `{process_regex}`",
        f"- Producer selector: `{verdict['producer_selection']}`",
        f"- Producer selector source: `{verdict['producer_selection_source']}`",
        f"- Producer selector required: `{verdict['producer_selection_required']}`",
        f"- Matched `time-profile` rows: `{time_profile_rows}`",
        f"- Matched `time-sample` rows: `{time_sample_rows}`",
        f"- Matched `thread-info` rows: `{thread_info_rows}`",
        "",
        "## P4 Scout Verdict",
        "",
        "| Field | Value |",
        "|---|---|",
        f"| Status | `{verdict['status']}` |",
        f"| Producer thread | `{verdict['producer_thread']}` |",
        f"| Producer tid | `{verdict.get('producer_tid', '')}` |",
        f"| Producer thread-info name | `{verdict.get('producer_thread_info_name', '')}` |",
        f"| Producer is main thread | `{verdict.get('producer_is_main_thread', '')}` |",
        f"| Producer profile weight ms | `{verdict.get('producer_profile_weight_ms', '')}` |",
        f"| Producer wait keyword hits | `{verdict.get('producer_wait_keyword_hits', '')}` |",
        f"| Producer wait keyword weight ms | `{verdict.get('producer_wait_keyword_weight_ms', '')}` |",
        f"| Producer holder keyword hits | `{verdict.get('producer_holder_keyword_hits', '')}` |",
        f"| Producer holder keyword weight ms | `{verdict.get('producer_holder_keyword_weight_ms', '')}` |",
        f"| Producer sample rows | `{verdict.get('producer_sample_rows', '')}` |",
        f"| Producer sample running rows | `{verdict.get('producer_sample_running_rows', '')}` |",
        f"| Producer sample blocked rows | `{verdict.get('producer_sample_blocked_rows', '')}` |",
        f"| Wait keyword thread count | `{verdict.get('wait_keyword_thread_count', '')}` |",
        f"| Non-producer wait keyword hits | `{verdict.get('nonproducer_wait_keyword_hits', '')}` |",
        f"| Holder status | `{verdict.get('holder_status', '')}` |",
        f"| Holder keyword thread count | `{verdict.get('holder_keyword_thread_count', '')}` |",
        f"| Holder keyword hits | `{verdict.get('holder_keyword_hits', '')}` |",
        f"| Main-thread holder keyword thread count | `{verdict.get('main_thread_holder_keyword_thread_count', '')}` |",
        f"| Main-thread holder keyword hits | `{verdict.get('main_thread_holder_keyword_hits', '')}` |",
        f"| Non-producer holder keyword hits | `{verdict.get('nonproducer_holder_keyword_hits', '')}` |",
        f"| Reason | {verdict['reason']} |",
        f"| Holder reason | {verdict.get('holder_reason', '')} |",
        "",
        "## Keyword Hits",
        "",
        "| Keyword | Samples | Weight ms |",
        "|---|---:|---:|",
    ]
    if keyword_samples:
        for keyword, samples in keyword_samples.most_common():
            lines.append(f"| `{keyword}` | {samples} | {keyword_weight_ms[keyword]:.3f} |")
    else:
        lines.append("| none | 0 | 0.000 |")
    lines += [
        "",
        "## Top Threads",
        "",
        "| Thread | TID | Main | Profile weight ms | Profile rows | Profile states | Sample rows | Sample states | Top frames | Keyword hits |",
        "|---|---|---|---:|---:|---|---:|---|---|---|",
    ]
    for row in rows[:top]:
        lines.append(
            "| {thread} | {tid} | {main} | {weight} | {profile_rows} | {profile_states} | {sample_rows} | "
            "{sample_states} | {top_frames} | {keyword_hits} |".format(
                thread=row["thread"].replace("|", "\\|"),
                tid=row["tid"],
                main=row["is_main_thread"],
                weight=row["time_profile_weight_ms"],
                profile_rows=row["time_profile_rows"],
                profile_states=row["time_profile_states"].replace("|", "\\|"),
                sample_rows=row["time_sample_rows"],
                sample_states=row["time_sample_states"].replace("|", "\\|"),
                top_frames=row["top_frames"].replace("|", "\\|"),
                keyword_hits=row["keyword_hits"].replace("|", "\\|"),
            )
        )
    lines += [
        "",
        "## Notes",
        "",
        "- `time-profile` reports sampled running stacks, so absence of a wait symbol there does not prove no blocking.",
        "- `time-sample` adds thread-state distribution, but its exported call stacks may be raw PCs on this Xcode version.",
        "- For the winemac `OnMainThread` hypothesis, a positive sample is a strong clue; a negative sample is only a scout result unless the trace is aligned with PE milestones.",
        "- When `--producer-thread-regex-from-pe-log` is used, the parser first looks for unix-side `unix_commit_chunk_entry native_tid=0x...` rows and then falls back to PE `thread_id=0x...` rows.",
        "- `producer-thread-selector-missing` means the direct log did not contain a usable native or PE thread id; do not read the auto producer thread as a same-run PE producer proof.",
        "- When a PE-log selector is present but the verdict is `producer-thread-not-found`, the PE `thread_id` likely came from the Win32 thread-id namespace, not xctrace's native Mach thread-id namespace.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize xctrace CPU time-profile/time-sample XML by process thread"
    )
    parser.add_argument("--time-profile", type=Path, required=True)
    parser.add_argument("--time-sample", type=Path)
    parser.add_argument("--thread-info", type=Path)
    parser.add_argument("--process-regex", default=r"3DMark05\.exe")
    parser.add_argument("--keyword", action="append", default=[])
    parser.add_argument("--output-csv", type=Path)
    parser.add_argument("--output-md", type=Path)
    parser.add_argument("--output-verdict-json", type=Path)
    parser.add_argument("--run-label", default="unknown")
    parser.add_argument("--trace", default="")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument(
        "--producer-thread-regex",
        default="",
        help=(
            "Optional regex for the thread that should be treated as the "
            "producer in the P4 verdict. Defaults to highest sampled weight."
        ),
    )
    parser.add_argument(
        "--producer-thread-regex-from-pe-log",
        type=Path,
        help=(
            "Optional direct/wrapper log. When --producer-thread-regex is unset, "
            "prefer native_tid=0x... from unix_commit_chunk_entry rows, then "
            "fall back to thread_id=0x... from pe_present_* milestone lines."
        ),
    )
    args = parser.parse_args()

    keywords = args.keyword or DEFAULT_KEYWORDS
    process_re = re.compile(args.process_regex)
    producer_thread_regex = args.producer_thread_regex
    producer_selection_source = "explicit-regex" if producer_thread_regex else "auto-highest-weight"
    producer_selection_required = bool(producer_thread_regex)
    if not producer_thread_regex and args.producer_thread_regex_from_pe_log:
        producer_thread_regex, producer_selection_source = producer_thread_regex_from_pe_log(
            args.producer_thread_regex_from_pe_log
        )
        producer_selection_required = True
    producer_thread_re = re.compile(producer_thread_regex) if producer_thread_regex else None
    threads, keyword_samples, keyword_weight_ms, profile_count = parse_time_profile(
        args.time_profile,
        process_re=process_re,
        keywords=keywords,
    )
    sample_count = 0
    if args.time_sample:
        sample_count = parse_time_sample(args.time_sample, process_re=process_re, threads=threads)
    thread_info_count = 0
    if args.thread_info:
        thread_info_count = parse_thread_info(args.thread_info, process_re=process_re, threads=threads)
    rows = rows_for_output(threads)
    verdict = p4_scout_verdict(
        rows,
        producer_thread_re=producer_thread_re,
        producer_thread_regex=producer_thread_regex,
        producer_selection_source=producer_selection_source,
        producer_selection_required=producer_selection_required,
    )
    if args.output_csv:
        write_csv(args.output_csv, rows)
    if args.output_verdict_json:
        write_verdict_json(args.output_verdict_json, verdict)
    if args.output_md:
        write_md(
            args.output_md,
            run_label=args.run_label,
            trace=args.trace,
            process_regex=args.process_regex,
            rows=rows,
            keyword_samples=keyword_samples,
            keyword_weight_ms=keyword_weight_ms,
            time_profile_rows=profile_count,
            time_sample_rows=sample_count,
            thread_info_rows=thread_info_count,
            top=args.top,
            producer_thread_regex=producer_thread_regex,
            producer_selection_source=producer_selection_source,
            producer_selection_required=producer_selection_required,
        )
    if not args.output_csv and not args.output_md:
        for row in rows[: args.top]:
            print(
                f"{row['thread']}\t{row['time_profile_weight_ms']}ms\t"
                f"tid={row['tid']}\t"
                f"profile={row['time_profile_rows']}\tsample={row['time_sample_rows']}\t"
                f"running={row['time_sample_running_rows']}\t"
                f"blocked={row['time_sample_blocked_rows']}\t"
                f"p4_wait={row['p4_wait_keyword_hits']}\t"
                f"p4_holder={row['p4_holder_keyword_hits']}\t"
                f"keywords={row['keyword_hits']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
