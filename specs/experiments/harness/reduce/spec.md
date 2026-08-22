---
type: "Spec"
title: "Harness Reduce Spec — Log Reduction"
description: "Script inventory, reduce-versus-join classification, declared log-line prefixes, parsing behavior, emitted artifacts, column contract, and environment ownership for the reduce domain."
tags: [specs, experiments, harness, reduce, spec]
---

# Harness Reduce Spec — Log Reduction

Implements `specs/experiments/harness/reduce/requirements.md`
(`R-HARN-REDUCE-*`). Instantiates the `reduce` row of the domain map
in `specs/experiments/harness/spec.md` §1 and the `dump-extract →
log-reduce` and `log-reduce → compare-gate` boundaries in that spec's
§2. Stage names, boundary names, and envelope fields are cited from
the parent spec rather than redefined here.

Facts below were verified against
`scripts/tools/summarize_3dmark05_perf.py` (8,757 lines),
`scripts/tools/summarize_index_cache_runtime.py` (372 lines), and
`scripts/tools/summarize_framegraph_dag.py` (394 lines) at their line
numbers on 2026-07-27, and against the real output directories
`experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/` and
`traces/app-d3d9-3dmark05-gt2-passcoalesce-v2-tape-r1-20260723/analysis/`
as noted per section. Column counts were derived by importing each
script as a module and reading its column-list constant's length
(`python3 -c 'exec(open(path).read()); print(len(CONST))'` style, run
without invoking `main()`), not by hand-counting prose, then
cross-checked against the real CSV header field counts in those
directories.

---

## 1. Script Inventory

| Script | Role |
|---|---|
| `scripts/tools/summarize_3dmark05_perf.py` | The domain's core. Parses `result.json` and, when present, eight distinct `[dxmt9-perf*]`/`[dxmt9-bridge-perf]` stderr line families from `dxmt9.log`, and emits one Markdown summary plus seven per-family CSVs (§6). |
| `scripts/tools/summarize_index_cache_runtime.py` | Second-order reducer: aggregates the reordered-index-cache lookup/hit/creation/rejection counters already present in `3dmark05-perf-encoders.csv` and `3dmark05-perf-indexed-probe-draws.csv` (both produced by the script above) into a per-run Markdown report plus one CSV (§6), and assigns a `verdict` string. |
| `scripts/tools/summarize_framegraph_dag.py` | Reads `DXMT9_RENDERER_DUMP_DAG` JSON dumps (one file per chunk, `dag-frame<N>-chunk<S>-{pre-opt,post-opt}.json`) and emits a per-file summary CSV, a same-attachment-pair candidate CSV, and a Markdown report (§6). |

Per R-HARN-REDUCE-1.1, this table covers exactly the three scripts the
parent domain map names as examples of its own "the
`scripts/tools/summarize_*` that read dxmt9's own logs" rule. Other
`scripts/tools/summarize_*` scripts exist and are **not** covered by
this document —
see §2.3.

---

## 2. Reduce-Versus-Join Classification, Verified

Per parent spec.md §1: "A summariser belongs to `reduce` when its
input is a dxmt9-produced log ... A summariser belongs to `join` when
its input is an external tool's export ... The distinction is the
origin of the input artifact, not the shape of the output."

### 2.1 `summarize_3dmark05_perf.py`

Primary inputs, read directly from `main()` (`:8679-8709`):
`result.json` (`load_result`, `:2893-2917`) and `dxmt9.log` (all eight
`parse_*_lines` functions, `:2979-3043`). Both are dxmt9-produced —
`result.json` is written by the `runner` domain and `dxmt9.log` is the
process's own captured stdout/stderr. No external-tool export is read
anywhere in this script (verified: no `csv.DictReader` call over an
externally-named input file exists in this script; its only
`csv`-module use is the `write_csv`/`load_existing_csv` round-trip over
its own prior output, `:6661-6677`). Clean `reduce` fit.

### 2.2 `summarize_index_cache_runtime.py`

Primary inputs are `--run LABEL=ENCODERS_CSV[,PROBE_DRAWS_CSV]`
(`parse_run`, `:44-49`) — in production use, `$encoders_csv` and
`$probe_draws_csv` are `3dmark05-perf-encoders.csv` and
`3dmark05-perf-indexed-probe-draws.csv`, both produced by
`summarize_3dmark05_perf.py` above (confirmed call sites:
`run_3dmark05_perf_probe.sh:6032-6035` and
`finalize_3dmark05_perf_probe.sh:1176-1181`, both passing
`$encoders_csv,$probe_draws_csv`). This script's own docstring states
the same thing directly: "This script covers the complementary
runtime question from dxmt encoder CSVs" (`:5-6`).

This is **not** literally one of the three input shapes parent spec.md
§1 enumerates ("a `[dxmt9-perf*]` stderr line, a `result.json`, a
`DXMT9_RENDERER_DUMP_DAG` JSON file") — its immediate input is a CSV
already produced by another `reduce`-domain script, a second-order
reduction the parent rule's wording does not spell out. Per
R-HARN-REDUCE-2.2, the classification is resolved by the parent rule's
own stated test ("the origin of the input artifact"): tracing the
input's origin one hop back reaches `summarize_3dmark05_perf.py`'s own
dxmt9-log-derived CSV, not an external tool's export. Nothing in this
script or its callers ever hands it an Xcode- or `xctrace`-originated
file. The classification is therefore unambiguous by elimination, even
though the parent's literal enumeration does not name this exact case
— this is stated as a verified gap in the parent rule's phrasing, not
a disagreement with its outcome.

### 2.3 `summarize_framegraph_dag.py`

Primary input: `DXMT9_RENDERER_DUMP_DAG` JSON files, or a directory of
them expanded via `dag-frame*-chunk*-*.json` (`expand_inputs`,
`:81-88`) — the exact JSON-dump shape parent spec.md §1 names verbatim
as a `reduce`-domain input. Clean `reduce` fit; no external-tool input
path exists in this script at all (verified: the only file format it
reads is `json.loads` over its own positional `paths` argument,
`:68-78`).

### 2.4 Scripts this document does not cover

`scripts/tools/summarize_*` scripts not named in §1, found by listing
`scripts/tools/summarize_*.py` on 2026-07-27:
`summarize_3dmark05_cleanup_candidates.py`,
`summarize_3dmark05_perf_gates.py`,
`summarize_3dmark05_visual_target_gate.py`,
`summarize_capture_rois.py`, `summarize_color_attachment_dumps.py`,
`summarize_effect_geometry_roi.py`,
`summarize_fragmentless_depth_route_gate.py`,
`summarize_gt2_present_gpu_latency.py`,
`summarize_primitive_conflict_selectors.py`,
`summarize_semantic_payload_candidates.py`,
`summarize_visibility_scout.py`. Three more —
`summarize_xcode_encoder_counters.py`,
`summarize_xctrace_cpu_threads.py`, and
`summarize_xctrace_metal_intervals.py` — are already assigned to
`join` by the parent domain map's own `join` row and their own
docstrings confirm an external-tool primary input (Xcode "Export
Encoder Counters" CSV; `xctrace` XML tables), so this document treats
that classification as settled elsewhere rather than re-verifying it.
`summarize_gt2_present_gpu_latency.py`'s docstring likewise states an
`xctrace`-exported-XML primary input, matching the `join` shape, but it
is not part of the parent's `join` row's own script list, so this
document does not assert its domain either — it is out of scope here
per R-HARN-REDUCE-2.3, same as the remaining eleven. Per
R-HARN-REDUCE-2.3, none of the scripts in this section is asserted to
belong to `reduce`, to `join`, or to any other domain by this
document.

---

## 3. Declared Log-Line Prefixes and Their Gating Variables

`summarize_3dmark05_perf.py` binds nine literal prefix constants
(`:24-32`), each anchored at line start via `line.startswith(...)`.
§1 and §4 count these as **eight** distinct line families, not nine,
because `PERF_PREFIX` and `BRIDGE_PREFIX` — two separate constants in
the table below — are counted as one family: both are parsed only as
a `result.json`-absent fallback through the same generic
`parse_kv_line` call (§4.1's "eighth family" paragraph), rather than
each owning independent line-shape or validity behavior the way the
other seven constants do. Nine constants, eight families, is the
reconciled count throughout this document:

| Constant | Literal prefix | Emitting site (`src/`) | Gating variable | Set by |
|---|---|---|---|---|
| `PERF_PREFIX` | `[dxmt9-perf] ` | cumulative counter dump at process exit | `DXMT_PERF_COUNTERS` | `runner` (`experiments/launchers/common.sh:146-147,163-164`) |
| `BRIDGE_PREFIX` | `[dxmt9-bridge-perf] ` | `winemetal_bridge.cpp:521-542` `reportBridgePerfCounters()` | `DXMT_PERF_COUNTERS` (same variable; `winemetal_bridge.cpp:169-173` `bridgePerfEnabledFlag()` reads it directly) | `runner` |
| `ENCODER_PREFIX` | `[dxmt9-perf-encoder ` | `dxmt9_perf_counters.cpp:9390-9396` `emitEncoderBreakdown()` | `DXMT9_PERF_ENCODER_BREAKDOWN` (`dxmt9_perf_counters.cpp:9325` `encoderBreakdownEnabled()`) | `probe` (`run_3dmark05_perf_probe.sh:4215`) |
| `STREAM_PREFIX` | `[dxmt9-perf-encoder-stream ` | same function, per used stream (`dxmt9_perf_counters.cpp:10025`) | same, `DXMT9_PERF_ENCODER_BREAKDOWN` | `probe` |
| `PROBE_DRAW_PREFIX` | `[dxmt9-perf-indexed-probe-draw ` | `dxmt9_draw_encoder.mm:3070` | `DXMT9_MEASURE_INDEX_REUSE` combined with encoder-breakdown being active (`dxmt9_draw_encoder.mm:13690`: `debug::measureIndexReuse() && encoderBreakdownActive`) | `probe` (`--measure-index-reuse` → `DXMT9_MEASURE_INDEX_REUSE`, probe's own mode table) |
| `RENDER_PASS_REENTRY_PREFIX` | `[dxmt9-perf-render-pass-reentry ` | `dxmt9_draw_encoder.mm:5753` | `DXMT9_PERF_RENDER_PASS_REENTRY_TOP` (`dxmt9_draw_encoder.mm:5404` direct `std::getenv`) | `probe` (`run_3dmark05_perf_probe.sh:4229`) |
| `FRAME_PREFIX` | `[dxmt9-perf-frame ` | `dxmt9_perf_counters.cpp:10146` | `DXMT9_PERF_FRAME_SAMPLING` (`dxmt9_perf_counters.cpp:9315-9318` direct `std::getenv`) | `probe` (`run_3dmark05_perf_probe.sh:4233`) |
| `ARGBUF_DELTA_SOURCE_PREFIX` | `[dxmt9-perf-argbuf-payload-delta-source ` | `dxmt9_draw_encoder.mm:15064,15076` | `DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE` (`dxmt9_draw_encoder.mm:4995` direct `std::getenv`) | **no owner today** — see note below |
| `VS_CONST_SETTER_RANGE_PREFIX` | `[dxmt9-perf-vs-const-setter-range ` | `src/d3d9/d3d9_pe_device_diag.cpp` `logVsConstSetterRangePerf` | `DXMT9_PERF_VS_CONST_SETTER_RANGE` (`src/d3d9/d3d9_pe_device_impl.hpp` `dxmt9PerfVsConstSetterRangeEnabled`) | `probe` (`--probe-vs-const-setter-range` → `run_3dmark05_perf_probe.sh:4249`) |

**`DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE` has no `probe` wrapper flag
today.** Per R-HARN-REDUCE-6.3, this was checked rather than assumed:
`grep -n "ARGBUF_PAYLOAD_DELTA"
scripts/tools/run_3dmark05_perf_probe.sh` returns no match on
2026-07-27. This
domain's `summarize_3dmark05_perf.py` can parse
`[dxmt9-perf-argbuf-payload-delta-source]` lines and always writes
`3dmark05-perf-argbuf-payload-delta-sources.csv` (§5, §6), but nothing
in the standard probe recipe (`agents/rules/metal_debugging.rules.md`
§9) ever sets the one variable
(`agents/rules/environment_variables_perf.rules.md`'s
`DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE` row) that would make dxmt9
emit those lines; a caller who wants this family populated must set
that variable directly in the launched process's environment,
bypassing the `probe` domain's own flag surface. This is a gap in
`probe`'s flag coverage, not a `reduce`-domain defect — this domain
correctly parses whatever the prefix produces; it is simply never
produced by the documented recipe. §5 shows the resulting empty CSV in
a real run.

`summarize_index_cache_runtime.py` and `summarize_framegraph_dag.py`
read no dxmt9 stderr lines directly; they read the CSV/JSON artifacts
named in §2.2/§2.3.

---

## 4. Parsing Behavior, As Implemented

This section states what each parser actually does today against the
R-HARN-REDUCE-3.* contract, not what an idealized reducer would do.

### 4.1 `summarize_3dmark05_perf.py` — permissive by default

Six of the eight line families (`ENCODER_PREFIX`, `STREAM_PREFIX`,
`PROBE_DRAW_PREFIX`, `RENDER_PASS_REENTRY_PREFIX`, `FRAME_PREFIX`,
`ARGBUF_DELTA_SOURCE_PREFIX`) are parsed identically:
`if line.startswith(PREFIX): rows.append(parse_kv_line(line))`
(`:2984-3029`).
`parse_kv_line` (`:2920-2926`) is a generic `key=value` regex scan
(`KEY_VALUE_RE = re.compile(r"\b([A-Za-z0-9_]+)=([^\s]+)")`,
`:21`) that extracts whatever `key=value` tokens it finds and silently
ignores anything it does not match — there is no row-shape validation,
no required-field check, and no diagnostic distinguishing "this line
matched the prefix and had every expected field" from "this line
matched the prefix but most fields failed to parse and the row is
mostly empty." **This does not satisfy R-HARN-REDUCE-3.2** for these
six families as written today; it is stated here as a verified gap,
not a compliant implementation.

The seventh family, `VS_CONST_SETTER_RANGE_PREFIX`, is the one
exception with an actual validity filter:
`parse_vs_const_setter_range_lines` (`:3032-3043`) requires the line
to end with `]` (`:3038-3039`, `continue` — silently — if not) and
then calls
`valid_vs_const_setter_range_row` (`:2943-2976`), which rejects a row
whose `phase` is not `call`/`flush`, whose `overflow` value is not
`0`/`1`, or whose required integer/hex fields are missing, returning
`False` with **no diagnostic naming which check failed or how many
rows were rejected** — the row is simply dropped from the aggregate
(`:3040-3042`:
`if valid_vs_const_setter_range_row(row): rows.append(row)`). This is
closer to R-HARN-REDUCE-3.2's intent than the six
generic parsers (it does distinguish valid from invalid shape) but
still does not emit the required count/diagnostic of rejected rows —
a caller cannot tell "zero call/flush setter-range events happened"
from "N rows were seen and rejected" from this script's output alone.

The eighth family, `[dxmt9-bridge-perf]`, and the base `[dxmt9-perf]`
line, are parsed only as a `result.json`-absent fallback inside
`load_result` (`:2903-2917`) via the same generic `parse_kv_line`; a
missing or unparseable `[dxmt9-perf]` line after a missing
`result.json` is the one hard-failure path in this script (`SystemExit`,
`:2911`), because at that point there is no counter data of any kind
to report.

### 4.2 `summarize_index_cache_runtime.py` — fails hard on a missing input, silently defaults on a malformed one

`load_csv` (`:33-37`) raises `SystemExit(f"missing CSV: {path}")` when
the given `--run` CSV path does not exist — a genuine hard failure
naming the missing file, satisfying the spirit of R-HARN-REDUCE-3.2/
parent R-HARN-2.1 for that one failure mode. `parse_run` (`:44-49`)
similarly raises `argparse.ArgumentTypeError` for a malformed `--run`
value before any file I/O happens.

Once a CSV is loaded, however, every numeric field is read through
`as_float`/`as_int` (`:18-26`), which catch `TypeError`/`ValueError`
and return `0.0`/`0` silently for a missing or non-numeric column —
so a CSV with a renamed or absent expected column (for example,
`reordered_index_cache_lookups`) does not fail; every derived sum
using that column becomes `0` and the summary's `verdict` field
(`:118-125`) reports `"no-cache-runtime-activity"`, indistinguishable
from a run that genuinely never exercised the cache path. This is a
second, distinct instance of the R-HARN-REDUCE-3.2 gap: a missing
expected column degrades to a semantically meaningful-looking verdict
string rather than a parse failure.

### 4.3 `summarize_framegraph_dag.py` — the one script that fails hard on malformed shape

`read_dag` (`:68-78`) raises `ValueError` naming the file and the
specific problem for three distinct malformed-input cases: invalid
JSON (`f"{path}: invalid JSON: {exc}"`, `:71-72`), a non-object
top-level value (`f"{path}: expected a JSON object"`, `:73-74`), and a
missing required list field (`f"{path}: missing list field {key!r}"`,
`:75-77`, checked for each of `passes`/`resources`/`edges`). `main()`
does not catch this exception (`:364-390`; no `try`/`except` around
the `read_dag(path)` call at `:374`), so it propagates as an uncaught
Python exception — a non-zero exit with a traceback that includes the
`ValueError`'s message, not a clean `SystemExit`-style single-line
diagnostic, but it does satisfy R-HARN-REDUCE-3.3's substance: the
file and the exact missing/malformed field are named, and the process
does not proceed on the malformed input. This is the one parser in
this domain that meets the R-HARN-5.3-style "hard failure, not silent
fallback" bar for its top-level input shape.

Within a structurally valid file, however, individual malformed
elements are silently skipped rather than failing:
`resource_accesses_by_handle` (`:159-175`) does
`if not isinstance(resource, dict): continue` for a non-dict entry in
the `resources` list, and
`render_passes`/`edge_rows` (`:139-146,178-181`) filter non-dict/
non-`"Render"` entries the same way. A malformed individual pass or
resource entry (as opposed to a malformed top-level file) is dropped
without a diagnostic — the same shape of gap as §4.1/§4.2, at a finer
grain.

`main()` does distinguish one empty-input case explicitly: if
`expand_inputs` finds zero files at all, it prints `"no DAG JSON files
found"` to stderr and returns `1` (`:367-369`) — a real, verified
positive example of R-HARN-REDUCE-4.1's "reported, not silently
valid-looking" bar, for that one specific cause. It does **not**
extend this to the case where files exist but a `--stage` filter
matches none of them: `summary_rows`/`candidate_rows` stay empty,
`write_csv`/`write_markdown` still run, and the process still exits
`0` with header-only CSVs — the same emptiness gap as elsewhere in
this domain, just narrower in scope (one filter, not every family).

---

## 5. Empty Reduction, Verified in a Real Run

`experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/` (a real
`log-reduce` output directory, 2026-07-27) demonstrates R-HARN-
REDUCE-4.1/4.2 concretely. `3dmark05-perf-summary.md` reports:

```
- Encoder lines: `8`
- Indexed probe draw lines: `395`
- Render-pass re-entry lines: `0`
- Frame sampling lines: `0`
- Argbuf payload delta source rows: `0`
- VS const setter range rows: `0`
```

`3dmark05-perf-argbuf-payload-delta-sources.csv` and
`3dmark05-perf-vs-const-setter-ranges.csv` in that same directory are
each exactly one line long (header only, verified with `wc -l`);
`3dmark05-perf-render-pass-reentry.csv` and
`3dmark05-perf-frames.csv` are the same shape. All four files are
well-formed, valid CSVs — a consumer opening
any of them sees a syntactically correct, header-complete, zero-row
file, indistinguishable in file structure from "this run never
triggered a render-pass re-entry" (true here —
`--render-pass-reentry-top` was not passed) from "the gating variable
was never even offered a flag" (also true here for
`DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE`, §3) from a hypothetical
future parsing regression that dropped every matching line. `main()`
exits `0` in every case; nothing
in this script's own artifact distinguishes these causes beyond the
row-count line already quoted above, which is present but is a bare
count, not a validity assertion per R-HARN-REDUCE-4.2.

This is the concrete evidence behind R-HARN-REDUCE-4.1/4.2: the
row-count reporting that already exists is a partial, useful signal
(better than nothing — it is genuinely visible in the Markdown output)
but it does not meet the bar parent R-HARN-3.1/3.2/3.3 set, because it
is never gated on, asserted, or recorded as a `validity` field a
downstream consumer could branch on without re-deriving it from the
row count itself.

---

## 6. Emitted Artifacts and Column Contract

`summarize_3dmark05_perf.py --output_dir` writes eight artifacts,
every one unconditionally (each CSV write is guarded by
`log_path.exists() or not <csv>.exists()`, `:8711-8732` — on a fresh
run with a log present, every guard is true), confirmed present
together in `experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/`:

| Artifact | Column-list constant | Verified column count | Verified against real header |
|---|---|---|---|
| `3dmark05-perf-summary.md` | n/a (Markdown) | n/a | present, 228 KB |
| `3dmark05-perf-encoders.csv` | `ENCODER_CSV_KEYS` | 362 | 362 |
| `3dmark05-perf-encoder-streams.csv` | `STREAM_CSV_KEYS` | 23 | 23 |
| `3dmark05-perf-indexed-probe-draws.csv` | `PROBE_DRAW_CSV_KEYS` | 135 | 135 |
| `3dmark05-perf-render-pass-reentry.csv` | `RENDER_PASS_REENTRY_CSV_KEYS` | 37 | 37 |
| `3dmark05-perf-frames.csv` | `FRAME_CSV_KEYS` | 109 | 109 |
| `3dmark05-perf-argbuf-payload-delta-sources.csv` | `ARGBUF_DELTA_SOURCE_CSV_KEYS` | 10 | 10 |
| `3dmark05-perf-vs-const-setter-ranges.csv` | `VS_CONST_SETTER_RANGE_CSV_KEYS` | 15 | 15 |

Column counts were derived by importing the module (`exec`'ing its
source into a fresh namespace, whose `__name__` resolves away from
`"__main__"` so the `if __name__ == "__main__": sys.exit(main())`
guard never fires and no argument parsing or file write is
triggered) and reading `len(ENCODER_CSV_KEYS)` etc.; "verified against
real header" is
`head -1 <file>.csv | awk -F',' '{print NF}'` against the files in
`experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/`. All seven
counts matched exactly. `362 + 23 + 135 + 37 + 109 + 10 + 15 = 691`
distinct column names across the seven CSVs this one script owns.

Downstream `join`/`gate` consumers that read these exact CSV paths by
name (verified: `grep -rl` over `scripts/tools/*.py`/`*.sh` for each
CSV's basename): `compare_3dmark05_perf_counters.py` (`gate`),
`summarize_xcode_encoder_counters.py`/`finalize_3dmark05_perf_probe.sh`
(`join`), plus `run_3dmark05_perf_probe.sh` and this same `reduce`
script (both re-derive the same directory's file paths independently
rather than reading them from any shared manifest).

`summarize_index_cache_runtime.py --csv-output` writes one CSV keyed
by `CSV_FIELDS` (`:178-236`), verified length `57`, matching the real
header field count in
`experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/3dmark05-index-cache-runtime-summary.csv`
exactly.

`summarize_framegraph_dag.py --summary-csv`/`--csv` write
`SUMMARY_FIELDS` (11 columns) and `CANDIDATE_FIELDS` (23 columns)
respectively (`:21-33,35-59`), both verified against real headers in
`traces/app-d3d9-3dmark05-gt2-passcoalesce-v2-tape-r1-20260723/analysis/framegraph-dag-summary.csv`
(11) and `framegraph-dag-candidates.csv` (23) — a non-empty real
capture (2 and 6 data rows respectively), unlike the empty-CSV
examples in §5.

---

## 7. Environment Variables

Per R-HARN-REDUCE-6.1, this domain sets no environment variable:
verified
`grep -n "os.environ\|getenv" scripts/tools/summarize_3dmark05_perf.py scripts/tools/summarize_index_cache_runtime.py scripts/tools/summarize_framegraph_dag.py`
returns no match across all three files on 2026-07-27. Every gating
variable this domain's output
depends on (§3) is set, if at all, by the `runner` domain
(`DXMT_PERF_COUNTERS`) or the `probe` domain (the
`DXMT9_PERF_ENCODER_BREAKDOWN`/`DXMT9_MEASURE_INDEX_REUSE`/
`DXMT9_PERF_RENDER_PASS_REENTRY_TOP`/`DXMT9_PERF_FRAME_SAMPLING`/
`DXMT9_PERF_VS_CONST_SETTER_RANGE`/`DXMT9_RENDERER_DUMP_DAG*` family),
with the one documented exception
(`DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE`, §3) that has no `probe`
wrapper flag at all today and must be set directly by the caller. This
domain reads none of them; it reads
only the file paths it is given on its own command line.

---

## 8. Mode Table

Per R-HARN-REDUCE-7.1, every output-altering flag across the three
scripts:

### 8.1 `summarize_3dmark05_perf.py`

| Flag | Effect on output |
|---|---|
| `output_dir` (positional) | Selects which run's `result.json`/`dxmt9.log` to reduce. |
| `--output` | Overrides the Markdown output path; also relocates all seven CSV paths, which are derived from `output.parent` (`:8684-8690`). |
| `--require-uniform-compact-saved-bytes-present` | Adds the one validity assertion in this domain (`require_uniform_compact_saved_bytes_present`, `:3081-3096`): exits non-zero with a diagnostic naming `present_encoded` and `d3d9_snapshot_uniform_materialized_compact_saved_bytes` when the derived per-present saved-bytes value is not strictly positive. Instantiates R-HARN-REDUCE-4.3/7.2 — this is the one case in this domain where a specific counter's non-degeneracy is actually checked and gated on, contrasted with the general per-family emptiness gap in §4/§5. |

### 8.2 `summarize_index_cache_runtime.py`

| Flag | Effect on output |
|---|---|
| `--run LABEL=ENCODERS_CSV[,PROBE_DRAWS_CSV]` (repeatable, required) | Adds one summarized run row; the probe-draws half is optional and defaults to the "not-provided" `probe_draw_summary` shape (`:52-67`) when omitted. |
| `--output` (required) | Markdown report path. |
| `--csv-output` | Optional CSV path (§6). |

### 8.3 `summarize_framegraph_dag.py`

| Flag | Effect on output |
|---|---|
| `paths` (positional, one or more) | Files or directories; a directory is expanded to its `dag-frame*-chunk*-*.json` members (`:81-88`). |
| `--stage` | Filters to snapshots whose JSON `stage` field matches exactly (`:375-376`); can silently reduce `summary_rows`/`candidate_rows` to empty (§4.3). |
| `--csv` | Candidate-pair CSV path (`CANDIDATE_FIELDS`, §6). |
| `--summary-csv` | Per-file summary CSV path (`SUMMARY_FIELDS`, §6). |
| `--markdown` | Markdown report path; when none of `--csv`/`--summary-csv`/`--markdown` is given, the Markdown report is written to `/dev/stdout` instead (`:388-389`). |
| `--limit` | Markdown table row cap per section; `0` means unlimited (`:360`). |
