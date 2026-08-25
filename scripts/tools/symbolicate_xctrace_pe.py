#!/usr/bin/env python3
"""Turn dxmt9 PE-side samples into a per-module CPU breakdown.

Two input modes, mutually exclusive:

`--time-profile` (Tier 1) joins an xctrace `time-profile` export against a
`DXMT9_PE_MODULE_MAP=1` log. xctrace samples of the Wine game thread are
largely opaque because the 32-bit PE code (game exe, our PE `d3d9.dll`,
`winemetal_dxmt9.dll`, and Wine's own PE-side DLLs) has no dyld image xctrace can
attribute to; the module map (`[dxmt9-pe-module-map] module=... base=...
size=...`, logged once at device creation) lets the top frame of each sample be
classified by address range.

`--sampler-log` (Tier 2) reads the in-process sampler's own emission instead.
On Apple Silicon Tier 1 hits a wall that no module map can fix: PE x86 code
runs Rosetta-translated and Instruments only maps translated PCs back to origin
images for dyld images, so ~93% of game-thread samples arrive as unmapped
64-bit addresses. `DXMT9_PE_THREAD_SAMPLER=1` sidesteps it by reading the game
thread's true Win32 program counter from inside the process and classifying it
against the same module map, emitting cumulative `[dxmt9-pe-sampler]` groups.
This mode parses the LAST such group (the counters are cumulative, so the last
group is the whole run) and, when the same log also carries the module map,
turns the self-module PC buckets into RVAs.

Parsing approach for the xctrace XML mirrors
scripts/tools/summarize_xctrace_cpu_threads.py: rows carry either an inline
element (`id="..."`) or a `ref="..."` back-reference into an interned pool of
previously-seen elements with the same tag name, so every lookup resolves refs
against an id->element map built once over the whole document.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Iterable, Optional


MODULE_LINE_RE = re.compile(
    r"\[dxmt9-pe-module-map\].*?\bmodule=(?P<module>\S+)\s+base=0x(?P<base>[0-9a-fA-F]+)\s+size=0x(?P<size>[0-9a-fA-F]+)"
)
PROBE_LINE_RE = re.compile(
    r"\[dxmt9-pe-module-map\].*?\bprobe=(?P<probe>\S+)\s+addr=0x(?P<addr>[0-9a-fA-F]+)"
    r"(?:\s+contained=(?P<contained>[01]))?"
)

# 4 GiB boundary used to separate "plausibly 32-bit PE address space" from
# "definitely 64-bit host/unix address space" when a top frame address falls
# outside every logged module range.
FOUR_GIB = 1 << 32

TID_IN_THREAD_FMT_RE = re.compile(r"\(0x([0-9a-fA-F]+)\)")

# A raw, unsymbolicated top frame from xctrace shows up as a bare hex address
# in the `name` attribute (with or without a leading "0x"). Anything else is
# treated as an already dyld-symbolicated frame.
HEX_ADDR_RE = re.compile(r"^(0x)?[0-9a-fA-F]+$")


class ModuleMap:
    """Sorted, non-overlapping-by-construction module base/size ranges."""

    def __init__(self) -> None:
        self._ranges: list[tuple[int, int, str]] = []  # (base, end, name)

    def add(self, name: str, base: int, size: int) -> None:
        if size <= 0:
            return
        self._ranges.append((base, base + size, name))

    def finalize(self) -> None:
        # Deterministic order: by base address, then name, so classify() is
        # reproducible and ties in overlapping (stale/reused) ranges resolve
        # the same way on every run.
        self._ranges.sort(key=lambda r: (r[0], r[2]))

    def classify(self, addr: int) -> str:
        for base, end, name in self._ranges:
            if base <= addr < end:
                return name
        if addr < FOUR_GIB:
            return "unknown_32bit"
        return "unknown_64bit"

    def contains_in_module_matching(self, name_substring: str, addr: int) -> bool:
        """True if `addr` falls inside a logged module whose name contains
        `name_substring` (case-insensitive). Used for the probe-address
        validation: the probe is our own d3d9.dll, so this checks containment
        by module identity rather than requiring an exact name match against
        the probe symbol's own name (which is unrelated to any module name)."""
        needle = name_substring.lower()
        for base, end, name in self._ranges:
            if needle in name.lower() and base <= addr < end:
                return True
        return False

    def ranges(self) -> list[tuple[int, int, str]]:
        return list(self._ranges)

    def __len__(self) -> int:
        return len(self._ranges)


def parse_module_map_log(path: Path) -> tuple[ModuleMap, Optional[tuple[str, int]], Optional[bool]]:
    """Returns (module_map, (probe_name, probe_addr) or None, self_reported_contained or None)."""
    modules = ModuleMap()
    probe: Optional[tuple[str, int]] = None
    self_reported: Optional[bool] = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[dxmt9-pe-module-map]" not in line:
            continue
        probe_match = PROBE_LINE_RE.search(line)
        if probe_match and "probe=" in line:
            probe = (probe_match.group("probe"), int(probe_match.group("addr"), 16))
            contained_text = probe_match.group("contained")
            if contained_text is not None:
                self_reported = contained_text == "1"
            continue
        module_match = MODULE_LINE_RE.search(line)
        if module_match:
            modules.add(
                module_match.group("module"),
                int(module_match.group("base"), 16),
                int(module_match.group("size"), 16),
            )
    modules.finalize()
    return modules, probe, self_reported


# --- in-process PE thread-sampler log parsing ------------------------------
# Emitted by DXMT9_PE_THREAD_SAMPLER as cumulative groups every 60 presents,
# plus a final group at device teardown. A group is one header line followed by
# its module / selfpc lines. Counters never reset, so the LAST group is the
# whole run and every earlier group is a strict prefix of it.

SAMPLER_TAG = "[dxmt9-pe-sampler]"
SAMPLER_HEADER_RE = re.compile(r"\bpresents=(?P<presents>\d+)\b.*?\bsamples=(?P<samples>\d+)\b")
SAMPLER_KV_RE = re.compile(r"\b(?P<key>[a-z_]+)=(?P<value>0x[0-9a-fA-F]+|\d+)\b")
SAMPLER_MODULE_RE = re.compile(r"\bmodule=(?P<module>\S+)\s+samples=(?P<samples>\d+)")
SAMPLER_SELF_MODULE_RE = re.compile(r"\bselfpc_module=(?P<module>\S+)")
SAMPLER_BUCKET_RE = re.compile(
    r"\bselfpc\s+bucket=0x(?P<bucket>[0-9a-fA-F]+)\s+samples=(?P<samples>\d+)"
)
SAMPLER_OVERFLOW_RE = re.compile(r"\bselfpc_overflow=(?P<overflow>\d+)")


class SamplerReport:
    def __init__(self) -> None:
        self.header: dict[str, int] = {}
        self.modules: list[tuple[str, int]] = []
        self.self_module: Optional[str] = None
        self.buckets: list[tuple[int, int]] = []
        self.selfpc_overflow: int = 0

    @property
    def samples(self) -> int:
        return self.header.get("samples", 0)

    @property
    def hz(self) -> int:
        return self.header.get("hz", 0)


def parse_sampler_log(path: Path) -> Optional[SamplerReport]:
    """Parse the LAST cumulative [dxmt9-pe-sampler] group, or None if the log
    has no group at all (sampler never started, or DXMT_LOG_LEVEL hid it)."""
    lines = [
        line
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
        if SAMPLER_TAG in line
    ]
    header_index: Optional[int] = None
    for index, line in enumerate(lines):
        if SAMPLER_HEADER_RE.search(line):
            header_index = index
    if header_index is None:
        return None

    report = SamplerReport()
    header_text = lines[header_index]
    # Header carries presents/samples/*_failures/hz/module_table as key=value.
    for match in SAMPLER_KV_RE.finditer(header_text.split(SAMPLER_TAG, 1)[1]):
        value_text = match.group("value")
        report.header[match.group("key")] = int(value_text, 0)

    for line in lines[header_index + 1 :]:
        if SAMPLER_HEADER_RE.search(line):
            break  # defensive: a later group means our scan missed the last one
        self_module_match = SAMPLER_SELF_MODULE_RE.search(line)
        if self_module_match:
            report.self_module = self_module_match.group("module")
            continue
        overflow_match = SAMPLER_OVERFLOW_RE.search(line)
        if overflow_match:
            report.selfpc_overflow = int(overflow_match.group("overflow"))
            continue
        bucket_match = SAMPLER_BUCKET_RE.search(line)
        if bucket_match:
            report.buckets.append(
                (int(bucket_match.group("bucket"), 16), int(bucket_match.group("samples")))
            )
            continue
        module_match = SAMPLER_MODULE_RE.search(line)
        if module_match:
            report.modules.append(
                (module_match.group("module"), int(module_match.group("samples")))
            )
    return report


def sampler_module_rows(report: SamplerReport) -> list[dict[str, object]]:
    """Same four-column shape as the time-profile mode. `weight_ms` is derived
    from the sampling rate (samples * 1000/hz), not measured, because the
    sampler counts stops rather than timing them; with no hz it stays 0."""
    total = report.samples or sum(samples for _, samples in report.modules)
    per_sample_ms = (1000.0 / report.hz) if report.hz else 0.0
    rows: list[dict[str, object]] = []
    for module, samples in report.modules:
        share = (samples / total) if total else 0.0
        rows.append(
            {
                "module": module,
                "samples": samples,
                "weight_ms": round(samples * per_sample_ms, 3),
                "share_of_thread": round(share, 6),
            }
        )
    rows.sort(key=lambda r: (-int(r["samples"]), str(r["module"])))
    return rows


def selfpc_base(modules: ModuleMap, self_module: Optional[str]) -> tuple[Optional[int], str]:
    """Resolve the module-map base the self-PC buckets are relative to.
    Prefers the sampler's own `selfpc_module=` name; falls back to the first
    logged module whose name contains "d3d9"."""
    if self_module:
        needle = self_module.lower()
        for base, _end, name in modules.ranges():
            if name.lower() == needle:
                return base, name
    for base, _end, name in modules.ranges():
        if "d3d9" in name.lower():
            return base, name
    return None, self_module or ""


def selfpc_rows(
    report: SamplerReport, base: Optional[int]
) -> list[dict[str, object]]:
    total = sum(samples for _, samples in report.buckets)
    rows: list[dict[str, object]] = []
    for bucket, samples in report.buckets:
        rows.append(
            {
                "bucket": f"0x{bucket:x}",
                "rva": f"0x{bucket - base:x}" if base is not None and bucket >= base else "",
                "samples": samples,
                "share_of_selfpc_top": round((samples / total) if total else 0.0, 6),
            }
        )
    rows.sort(key=lambda r: (-int(r["samples"]), str(r["bucket"])))
    return rows


def write_selfpc_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(
            fh, fieldnames=["bucket", "rva", "samples", "share_of_selfpc_top"]
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def selfpc_csv_path(output_csv: Path) -> Path:
    return output_csv.with_name(output_csv.stem + "-selfpc" + output_csv.suffix)


def write_sampler_md(
    path: Path,
    *,
    report: SamplerReport,
    module_rows: list[dict[str, object]],
    self_rows: list[dict[str, object]],
    self_module_name: str,
    self_base: Optional[int],
    module_count: int,
    probe_name: str,
    probe_addr: Optional[int],
    probe_verdict: str,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# PE in-process thread sampler (Tier 2)")
    lines.append("")
    lines.append(f"- Presents at last emission: {report.header.get('presents', 0)}")
    lines.append(f"- Samples: {report.samples}")
    lines.append(f"- Sampling rate: {report.hz} Hz")
    lines.append(
        f"- Suspend failures: {report.header.get('suspend_failures', 0)}"
        f" / context failures: {report.header.get('ctx_failures', 0)}"
        f" / resume failures: {report.header.get('resume_failures', 0)}"
    )
    lines.append(f"- Modules in map: {module_count}")
    lines.append(
        f"- Probe `{probe_name}` addr=0x{probe_addr:x} containment: **{probe_verdict}**"
        if probe_addr is not None
        else "- Probe: not found in module-map log (RVA column is unvalidated)"
    )
    lines.append("")
    lines.append(
        "`weight_ms` is derived as `samples * 1000/hz`, not measured: the "
        "sampler counts thread stops, it does not time them."
    )
    lines.append("")
    lines.append("| module | samples | weight_ms | share_of_thread |")
    lines.append("|---|---|---|---|")
    for row in module_rows:
        lines.append(
            f"| {row['module']} | {row['samples']} | {row['weight_ms']} | "
            f"{float(row['share_of_thread']):.1%} |"
        )
    lines.append("")
    if self_base is None:
        lines.append(
            f"## Self-module PC buckets (`{self_module_name or 'unknown'}`, RVA unresolved)"
        )
    else:
        lines.append(
            f"## Self-module PC buckets (`{self_module_name}` base=0x{self_base:x})"
        )
    lines.append("")
    lines.append(f"- Buckets dropped to overflow: {report.selfpc_overflow}")
    lines.append("")
    lines.append("| bucket | rva | samples | share_of_selfpc_top |")
    lines.append("|---|---|---|---|")
    for row in self_rows:
        lines.append(
            f"| {row['bucket']} | {row['rva'] or '-'} | {row['samples']} | "
            f"{float(row['share_of_selfpc_top']):.1%} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# --- xctrace time-profile XML parsing -------------------------------------
# Mirrors summarize_xctrace_cpu_threads.py's id/ref resolution: build an
# id -> element map over the whole document once, then every row element
# either carries its own attributes (id="...") or points at a previously
# interned element of the same tag (ref="...").


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


def parse_duration_ms(fmt_text: str, fallback_ns_text: str) -> float:
    text = (fmt_text or "").strip().replace(",", "")
    if text:
        parts = text.split()
        try:
            amount = float(parts[0])
        except (ValueError, IndexError):
            amount = 0.0
        unit = parts[1] if len(parts) > 1 else "ms"
        if unit.startswith("ns"):
            return amount / 1_000_000.0
        if unit.startswith("us") or unit.startswith("µs"):
            return amount / 1_000.0
        if unit.startswith("ms"):
            return amount
        if unit.startswith("s"):
            return amount * 1_000.0
        if amount:
            return amount
    try:
        return float((fallback_ns_text or "").strip() or 0.0) / 1_000_000.0
    except ValueError:
        return 0.0


def top_frame(row: ET.Element, ids: dict[str, ET.Element]) -> Optional[tuple[str, str]]:
    """Returns (name, binary) for the first frame of the row's tagged
    backtrace, or None if the row has no backtrace at all."""
    bt = child(row, "tagged-backtrace")
    if bt is None:
        return None
    resolved_bt = resolve(bt, ids)
    for frame in resolved_bt.iter("frame"):
        actual = resolve(frame, ids)
        name = actual.get("name") or ""
        binary = ""
        binary_elem = child(actual, "binary")
        if binary_elem is not None:
            binary_actual = resolve(binary_elem, ids)
            binary = binary_actual.get("name") or binary_actual.get("path") or ""
        return name, binary
    return None


def frame_address(name: str) -> Optional[int]:
    """A raw unsymbolicated xctrace frame shows its address as the `name`
    attribute, formatted as a bare hex string (with or without "0x"). Return
    None if `name` does not look like an address (i.e. it is an already
    dyld-symbolicated function name)."""
    text = name.strip()
    if not text:
        return None
    if not HEX_ADDR_RE.match(text):
        return None
    try:
        return int(text, 16)
    except ValueError:
        return None


class ThreadKey:
    __slots__ = ("fmt",)

    def __init__(self, fmt: str) -> None:
        self.fmt = fmt or "<unknown-thread>"

    def tid(self) -> Optional[int]:
        m = TID_IN_THREAD_FMT_RE.search(self.fmt)
        if not m:
            return None
        return int(m.group(1), 16)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, ThreadKey) and self.fmt == other.fmt

    def __hash__(self) -> int:
        return hash(self.fmt)


def select_thread(
    path: Path,
    ids: dict[str, ET.Element],
    root: ET.Element,
    *,
    process_re: re.Pattern[str],
    explicit_tid: Optional[int],
) -> Optional[str]:
    """Auto-select the top-CPU (highest summed weight) thread of the matched
    process, or the thread whose embedded tid matches --thread-tid."""
    weight_by_thread: dict[str, float] = {}
    for row in root.iter("row"):
        process = value(child(row, "process"), ids)
        thread = value(child(row, "thread"), ids)
        if not (process_re.search(process) or process_re.search(thread)):
            continue
        weight_elem = child(row, "weight")
        weight_actual = resolve(weight_elem, ids) if weight_elem is not None else None
        weight_ms = parse_duration_ms(
            weight_actual.get("fmt") if weight_actual is not None else "",
            weight_actual.text if weight_actual is not None else "",
        )
        weight_by_thread[thread] = weight_by_thread.get(thread, 0.0) + weight_ms

    if explicit_tid is not None:
        for thread in weight_by_thread:
            if ThreadKey(thread).tid() == explicit_tid:
                return thread
        return None

    if not weight_by_thread:
        return None
    # Deterministic tie-break: highest weight first, then lexical thread fmt.
    return max(weight_by_thread.items(), key=lambda kv: (kv[1], kv[0]))[0]


def summarize(
    time_profile: Path,
    modules: ModuleMap,
    *,
    process_re: re.Pattern[str],
    explicit_tid: Optional[int],
) -> tuple[str, dict[str, dict[str, float]], int, int]:
    root, ids = load_xml(time_profile)
    selected_thread = select_thread(
        time_profile, ids, root, process_re=process_re, explicit_tid=explicit_tid
    )

    buckets: dict[str, dict[str, float]] = {}
    total_rows = 0
    no_backtrace_rows = 0

    if selected_thread is None:
        return "", buckets, total_rows, no_backtrace_rows

    for row in root.iter("row"):
        process = value(child(row, "process"), ids)
        thread = value(child(row, "thread"), ids)
        if not (process_re.search(process) or process_re.search(thread)):
            continue
        if thread != selected_thread:
            continue
        total_rows += 1

        weight_elem = child(row, "weight")
        weight_actual = resolve(weight_elem, ids) if weight_elem is not None else None
        weight_ms = parse_duration_ms(
            weight_actual.get("fmt") if weight_actual is not None else "",
            weight_actual.text if weight_actual is not None else "",
        )

        frame = top_frame(row, ids)
        if frame is None:
            no_backtrace_rows += 1
            bucket_name = "no_backtrace"
        else:
            name, binary = frame
            addr = frame_address(name)
            if addr is None:
                # Already dyld-symbolicated: name is a real symbol, not a
                # bare address. Bucket by the owning image.
                bucket_name = f"host:{binary or '<unknown-image>'}"
            else:
                bucket_name = modules.classify(addr)

        bucket = buckets.setdefault(bucket_name, {"samples": 0, "weight_ms": 0.0})
        bucket["samples"] += 1
        bucket["weight_ms"] += weight_ms

    return selected_thread, buckets, total_rows, no_backtrace_rows


def rows_for_output(buckets: dict[str, dict[str, float]], total_rows: int) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for module, stats in buckets.items():
        samples = int(stats["samples"])
        weight_ms = float(stats["weight_ms"])
        share = (samples / total_rows) if total_rows else 0.0
        rows.append(
            {
                "module": module,
                "samples": samples,
                "weight_ms": round(weight_ms, 3),
                "share_of_thread": round(share, 6),
            }
        )
    # Deterministic ordering: highest weight first, then module name.
    rows.sort(key=lambda r: (-float(r["weight_ms"]), str(r["module"])))
    return rows


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(
            fh, fieldnames=["module", "samples", "weight_ms", "share_of_thread"]
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_md(
    path: Path,
    *,
    selected_thread: str,
    total_rows: int,
    no_backtrace_rows: int,
    probe_name: str,
    probe_addr: Optional[int],
    probe_verdict: str,
    module_count: int,
    rows: list[dict[str, object]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# PE module-map symbolication (Tier 1)")
    lines.append("")
    lines.append(f"- Selected thread: `{selected_thread}`")
    lines.append(f"- Total samples on thread: {total_rows}")
    no_bt_share = (no_backtrace_rows / total_rows) if total_rows else 0.0
    lines.append(
        f"- Samples with no backtrace: {no_backtrace_rows} ({no_bt_share:.1%})"
    )
    lines.append(f"- Modules in map: {module_count}")
    lines.append(
        f"- Probe `{probe_name}` addr=0x{probe_addr:x} containment: **{probe_verdict}**"
        if probe_addr is not None
        else "- Probe: not found in module-map log"
    )
    lines.append("")
    lines.append("| module | samples | weight_ms | share_of_thread |")
    lines.append("|---|---|---|---|")
    for row in rows:
        lines.append(
            f"| {row['module']} | {row['samples']} | {row['weight_ms']} | "
            f"{float(row['share_of_thread']):.1%} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def check_probe(
    modules: ModuleMap,
    probe: Optional[tuple[str, int]],
    self_reported: Optional[bool],
) -> tuple[bool, str]:
    """Re-derive the module-map probe verdict. The probe is a marker function
    inside our own PE d3d9.dll, so containment means the address falls inside a
    module whose name matches "d3d9" -- not a match against the probe symbol's
    own name."""
    if probe is None:
        return False, "FAIL"
    probe_ok = modules.contains_in_module_matching("d3d9", probe[1])
    # Cross-check against the PE side's own self-reported verdict when it
    # logged one -- disagreement means either this tool's re-derivation or the
    # PE-side DXMT_ASSERT-guarded check disagree with the module ranges, which
    # is itself worth surfacing.
    if self_reported is not None and self_reported != probe_ok:
        print(
            "warning: PE-reported probe containment "
            f"({self_reported}) disagrees with recomputed containment "
            f"({probe_ok})",
            file=sys.stderr,
        )
    return probe_ok, ("PASS" if probe_ok else "FAIL")


def run_sampler_mode(
    args: argparse.Namespace,
    *,
    modules: ModuleMap,
    probe: Optional[tuple[str, int]],
    probe_ok: bool,
    probe_verdict: str,
) -> int:
    report = parse_sampler_log(args.sampler_log)
    if report is None:
        print(
            "error: no [dxmt9-pe-sampler] emission group found in "
            f"{args.sampler_log} (was DXMT9_PE_THREAD_SAMPLER=1 set, with "
            "DXMT_LOG_LEVEL=info?)",
            file=sys.stderr,
        )
        return 1

    module_rows = sampler_module_rows(report)
    write_csv(args.output_csv, module_rows)

    self_base, self_module_name = selfpc_base(modules, report.self_module)
    self_rows = selfpc_rows(report, self_base)
    selfpc_path = selfpc_csv_path(args.output_csv)
    write_selfpc_csv(selfpc_path, self_rows)

    write_sampler_md(
        args.output_md,
        report=report,
        module_rows=module_rows,
        self_rows=self_rows,
        self_module_name=self_module_name,
        self_base=self_base,
        module_count=len(modules),
        probe_name=probe[0] if probe else "",
        probe_addr=probe[1] if probe else None,
        probe_verdict=probe_verdict,
    )

    print(f"samples={report.samples} hz={report.hz}")
    print(
        "suspend_failures={} ctx_failures={} resume_failures={}".format(
            report.header.get("suspend_failures", 0),
            report.header.get("ctx_failures", 0),
            report.header.get("resume_failures", 0),
        )
    )
    print(
        f"modules={len(modules)} module_rows={len(module_rows)} "
        f"selfpc_rows={len(self_rows)} selfpc_overflow={report.selfpc_overflow}"
    )
    print(f"selfpc_csv={selfpc_path}")
    print(f"probe_verdict={probe_verdict}")

    if report.buckets and self_base is None:
        # The buckets are raw VAs; without a base they cannot be turned into
        # anything a disassembler can be pointed at.
        print(
            "warning: self-PC buckets present but no module-map base resolved "
            "-- rva column is empty",
            file=sys.stderr,
        )

    if probe is None:
        # Only the RVA column depends on the module map here: the per-module
        # classification was done in-process against the sampler's own
        # enumeration, so a missing probe downgrades to a warning rather than
        # the hard failure it is in --time-profile mode.
        print(
            "warning: no probe= line in the module-map log; self-PC RVAs are "
            "unvalidated",
            file=sys.stderr,
        )
        return 0
    if not probe_ok and not args.allow_probe_failure:
        print(
            "error: probe address containment FAILED (pass --allow-probe-failure "
            "to continue anyway)",
            file=sys.stderr,
        )
        return 1
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--time-profile",
        type=Path,
        default=None,
        help="Tier 1: xctrace time-profile XML export to join. Mutually "
        "exclusive with --sampler-log.",
    )
    parser.add_argument(
        "--sampler-log",
        type=Path,
        default=None,
        help="Tier 2: a run log carrying DXMT9_PE_THREAD_SAMPLER's cumulative "
        "[dxmt9-pe-sampler] groups. The last group is used. Mutually "
        "exclusive with --time-profile.",
    )
    parser.add_argument(
        "--module-map-log",
        type=Path,
        default=None,
        help="DXMT9_PE_MODULE_MAP=1 log. Required with --time-profile; with "
        "--sampler-log it defaults to the sampler log itself (both are Info "
        "lines from the same run) and supplies the self-PC RVA base.",
    )
    parser.add_argument(
        "--process-regex",
        default=r"\.exe\b",
        help="Regex matched against the xctrace process/thread fmt text "
        "(default matches a typical Wine game process by its .exe name).",
    )
    parser.add_argument(
        "--thread-tid",
        default=None,
        help="Explicit tid (decimal or 0x-prefixed hex) to select instead of "
        "auto-selecting the top-CPU thread of the matched process.",
    )
    parser.add_argument("--output-csv", required=True, type=Path)
    parser.add_argument("--output-md", required=True, type=Path)
    parser.add_argument(
        "--allow-probe-failure",
        action="store_true",
        help="Do not fail (exit nonzero) when the probe address is not "
        "contained in its own module's logged range.",
    )
    args = parser.parse_args(argv)

    if (args.time_profile is None) == (args.sampler_log is None):
        print(
            "error: pass exactly one of --time-profile or --sampler-log",
            file=sys.stderr,
        )
        return 2
    module_map_log = args.module_map_log
    if module_map_log is None:
        if args.time_profile is not None:
            print(
                "error: --module-map-log is required with --time-profile",
                file=sys.stderr,
            )
            return 2
        module_map_log = args.sampler_log

    modules, probe, self_reported = parse_module_map_log(module_map_log)
    probe_ok, probe_verdict = check_probe(modules, probe, self_reported)

    if args.sampler_log is not None:
        return run_sampler_mode(
            args,
            modules=modules,
            probe=probe,
            probe_ok=probe_ok,
            probe_verdict=probe_verdict,
        )

    process_re = re.compile(args.process_regex)

    explicit_tid: Optional[int] = None
    if args.thread_tid is not None:
        text = str(args.thread_tid).strip()
        explicit_tid = int(text, 16) if text.lower().startswith("0x") else int(text)

    selected_thread, buckets, total_rows, no_backtrace_rows = summarize(
        args.time_profile,
        modules,
        process_re=process_re,
        explicit_tid=explicit_tid,
    )

    if not selected_thread:
        print(
            f"error: no thread matched --process-regex {args.process_regex!r} "
            "in the time-profile export",
            file=sys.stderr,
        )
        return 1

    rows = rows_for_output(buckets, total_rows)
    write_csv(args.output_csv, rows)

    probe_name = probe[0] if probe else ""
    probe_addr = probe[1] if probe else None

    write_md(
        args.output_md,
        selected_thread=selected_thread,
        total_rows=total_rows,
        no_backtrace_rows=no_backtrace_rows,
        probe_name=probe_name,
        probe_addr=probe_addr,
        probe_verdict=probe_verdict,
        module_count=len(modules),
        rows=rows,
    )

    print(f"selected_thread={selected_thread}")
    print(f"total_samples={total_rows} no_backtrace={no_backtrace_rows}")
    print(f"modules={len(modules)} probe_verdict={probe_verdict}")

    if probe is None:
        print("error: no probe= line found in --module-map-log", file=sys.stderr)
        return 1 if not args.allow_probe_failure else 0

    if not probe_ok and not args.allow_probe_failure:
        print(
            "error: probe address containment FAILED (pass --allow-probe-failure "
            "to continue anyway)",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
