---
type: "Spec"
title: "Harness Join Spec — External Tool Joins"
description: "Script inventory, the manual Xcode export contract, coverage-gate behavior, and the joined CSV column contract."
tags: [specs, experiments, harness, join, spec]
---

# Harness Join Spec — External Tool Joins

Implements `specs/experiments/harness/join/requirements.md`
(`R-HARN-JOIN-*`). Instantiates the `join` row of the domain map in
`specs/experiments/harness/spec.md` §1 and the `offline-replay →
external-join` and `external-join → compare-gate` boundaries in that
spec's §2. Stage names, boundary names, and envelope fields are cited
from the parent spec rather than redefined here.

Facts below were verified against
`scripts/tools/finalize_3dmark05_perf_probe.sh` (1,780 lines, `wc -l`),
`scripts/tools/summarize_xcode_encoder_counters.py` (2,892 lines),
`scripts/tools/summarize_xctrace_metal_intervals.py` (742 lines), and
`scripts/tools/summarize_xctrace_cpu_threads.py` (798 lines) at their
line numbers on 2026-07-27, cross-checked against real trace
directories that still exist on disk at that date:
`traces/app-d3d9-3dmark05-gt2-order-store-control-phasealigned-frame255-xcode-r1-20260724/analysis/`
(current script version, verified column counts below) and
`traces/app-d3d9-3dmark05-capture-layer-current-r2-20260619/analysis/`
(an older run whose column counts differ from the current script,
cited in §4 as evidence that counts drift over time and must be
re-derived, not hardcoded). Column counts were derived by importing
each script as a module (`exec`'ing its source into a fresh namespace
whose `__name__` does not resolve to `"__main__"`, so no argument
parsing or file write fires) and reading its column-list constant's
length, then cross-checked against real CSV headers in the directories
above — following
`specs/experiments/harness/reduce/spec.md`'s own verification method
(R-HARN-REDUCE-5.2), applied to this domain's artifacts.

---

## 1. Script Inventory

| Script | Role |
|---|---|
| `scripts/tools/finalize_3dmark05_perf_probe.sh` | The domain's orchestrator. Checks that the human-exported Xcode CSV exists, regenerates `reduce`-domain summaries, invokes `summarize_xcode_encoder_counters.py` to join them, then optionally invokes `gate`-domain comparison scripts and updates a shared trace-artifacts manifest. |
| `scripts/tools/summarize_xcode_encoder_counters.py` | Reads the human-exported Xcode "Export Encoder Counters" CSV, reduces it to `SUMMARY_FIELDS`, joins it against a `reduce`-domain dxmt encoder CSV (or dxmt9 log) by `RenderPass[seq=...,enc=...]` label, and writes a joined CSV plus a Markdown bottleneck report. |
| `scripts/tools/summarize_xctrace_metal_intervals.py` | Sidecar/fallback for runs where Xcode `.gputrace` capture is blocked: reads an `xctrace`-exported `metal-gpu-intervals` XML table, joins it against the same dxmt encoder CSV by the same label, and writes a joined CSV plus Markdown summary. |
| `scripts/tools/summarize_xctrace_cpu_threads.py` | Reads `xctrace`-exported `time-profile`/`time-sample` XML tables (CPU thread samples, not encoder counters) for present-pacing producer-thread attribution, and writes a CSV/Markdown/verdict-JSON summary. |

Per R-HARN-JOIN-1.1, this table covers exactly the four scripts the
parent domain map names for `join`.

---

## 2. Reduce-Versus-Join Classification, Verified

Per parent spec.md §1: a summariser belongs to `join` when its
*primary* input is an external tool's export, not a dxmt9-produced
log.

### 2.1 `summarize_xcode_encoder_counters.py`

Primary input: the positional `xcode_csv` argument (`:2795`), read by
`read_csv_rows` (`:485-490`) and reduced by `summarize_xcode`
(`:564-...`). Its own docstring states this directly: "The script
consumes the CSV produced by Xcode's 'Export Encoder Counters'"
(`:2-7`). A `reduce`-domain CSV (`--dxmt-encoders-csv`) or a dxmt9 log
(`--dxmt-log`) is read only as a *secondary* input for attribution
(`load_dxmt`, `:556-561`) — confirmed optional: if neither flag is
given, `load_dxmt` returns `{}` and every row simply carries zero dxmt
attribution rather than failing. Clean `join` fit per the parent's
origin test.

### 2.2 `summarize_xctrace_metal_intervals.py`

Primary input: `--gpu-intervals`, an XML file (`:665-666`), parsed by
`parse_xctrace_rows`. Its own docstring: "It consumes XML exported
from xctrace's `metal-gpu-intervals` table and the dxmt9
`3dmark05-perf-encoders.csv`" (`:4-7`), the latter being the same kind
of secondary `reduce`-domain input as §2.1's `--dxmt-encoders-csv`
(here required, `:667-668`, unlike §2.1's optional one). Clean `join`
fit.

### 2.3 `summarize_xctrace_cpu_threads.py`

Primary input: `--time-profile`, a required XML file (`:701`), an
`xctrace`-exported symbolicated sampled-stack table. Its own
docstring: "This consumes XML tables exported from Instruments /
xctrace" (`:2-7`). Unlike the other two scripts in this domain, it
reads no `reduce`-domain secondary input at all — its optional
`--thread-info`/`--time-sample` inputs (`:702-703`) are also
`xctrace` exports, not dxmt9 output. Still a clean `join` fit under
the parent's origin test: the deciding question is whether the
*primary* input is dxmt9's own log, and here it plainly is not, even
though the counters this script attributes (CPU thread pacing) are a
different concern from the other two scripts' GPU encoder counters.

### 2.4 `finalize_3dmark05_perf_probe.sh`

Not itself a reducer; it is the domain's orchestrator (§3). Its
primary role — checking for and consuming the human-exported Xcode CSV
(§5) and invoking §2.1's script — is unambiguously an `external-join`
responsibility. Per R-HARN-JOIN-1.3, it also invokes `gate`-domain and
undetermined-domain scripts inline (§3), which does not change this
script's own domain, mirroring how `run_3dmark05_perf_probe.sh`
(`probe`) invokes `reduce`-domain scripts inline without becoming a
`reduce` script itself (parent spec.md §1, "Why the domain axis is
harness families, not stages").

---

## 3. `finalize_3dmark05_perf_probe.sh`'s Own Multi-Domain Invocation Chain

Verified inline invocations, in the order the script runs them
(`:1661-1678`), together with each invoked script's own domain per
the parent domain map (§1) or, where the parent domain map does not
name the script, left unasserted:

| Invoked script | Domain (verified) |
|---|---|
| `summarize_3dmark05_perf.py` | `reduce` (parent spec.md §1 names it directly) |
| `summarize_index_cache_runtime.py` | `reduce` (parent spec.md §1 names it directly) |
| `summarize_xcode_encoder_counters.py` | `join` (this document, §1) |
| `analyze_indexed_probe_classes.py` | not asserted by this document (not named in any domain row) |
| `analyze_shader_dumps.py` | not asserted by this document (not named in any domain row) |
| `compare_3dmark05_perf_counters.py` | `gate` (parent spec.md §1: `scripts/tools/compare_*`) |
| `compare_xcode_dxmt_bottlenecks.py` | `gate` (parent spec.md §1: `scripts/tools/compare_*`) |
| `compare_experiment_images.py` | `gate` (parent spec.md §1: `scripts/tools/compare_*`) |

This is stated here, rather than left implicit, because a reader who
opens this one 1,780-line script and sees it call `compare_*` scripts
directly could otherwise conclude — incorrectly — that this document
also owns those scripts' contracts. It does not; R-HARN-JOIN-1.3
forbids that reading.

At the end of a real (non-dry-run) invocation, `finalize_3dmark05_perf_probe.sh`
also updates `<output-dir>/3dmark05-trace-artifacts.json` directly via
an inline Python heredoc (`:1680-1752`), merging in the paths to the
Xcode performance `.gputrace`, the raw counters CSV, the reduced
counters-summary CSV, the joined CSV, and the bottleneck report,
plus a per-path `exists` map. Parent spec.md §5 attributes this same
manifest file to "the `probe` domain
(`scripts/tools/run_3dmark05_perf_probe.sh` and
`scripts/tools/finalize_3dmark05_perf_probe.sh`)" — this document does
not resolve that domain-attribution wording; it records, as a verified
fact independent of that attribution, that this `join`-domain script is
one of the two writers of that shared manifest file, each updating a
disjoint set of fields (the `probe`-domain script writes capture-stage
paths; this script adds the `external-join`-stage paths listed above).

---

## 4. The Human Export Step's Declared Contract

Per R-HARN-JOIN-2.1/2.2/2.3, the GUI procedure itself is specified in
`agents/rules/metal_debugging.rules.md` §2b and is not restated here.
What this section states is the artifact contract that procedure must
satisfy, which this domain's own scripts check.

**Expected path.** `finalize_3dmark05_perf_probe.sh` computes the
expected Xcode CSV path as (`:1129-1131`):

```
${trace_dir}/analysis/frame${frame}-counters-xcode.csv
```

where `trace_dir` defaults to `$repo_root/traces/$run_id` (`:1125-1127`)
and `frame` defaults to `60` (`:6`, overridable by
`DXMT_3DMARK05_PROBE_FRAME` or `--frame`). This is the exact path
`agents/rules/metal_debugging.rules.md` §2b's step 6 instructs a human
to save the export to.

**Expected column shape.** `summarize_xcode_encoder_counters.py`
declares `REQUIRED_XCODE_COUNTER_COLUMNS`, verified length **24**
(`:24-49`, confirmed by import as described in the header above):

```
Index, Encoder Index, CommandBuffer Index, CommandBuffer Label,
Encoder Label, GPU Time, Partial Render Count,
Bytes Written To Device Memory, Buffer Device Memory Bytes Written,
VS Buffer Device Memory Bytes Written, VS Invocations, FS Invocations,
Primitives, Pixels Rasterized, Tiled Vertex Buffer Bytes,
Tiled Vertex Buffer Primitive Blocks Bytes, VS Buffer L1 Bytes Written,
VS Last Level Cache Bytes Written, Vertex Stage Time,
VS Buffer Write Limiter, VS ALU Limiter, Buffer Write Limiter,
Last Level Cache Limiter, MMU Limiter
```

Verified against a real export,
`traces/app-d3d9-3dmark05-gt2-order-store-control-phasealigned-frame255-xcode-r1-20260724/analysis/frame255-counters-xcode.csv`
(226 raw columns total): all 24 required names are present in the
header (checked with a `csv.reader` header scan against the exact list
above; zero missing).

**The join key.** Both the Xcode route
(`summarize_xcode_encoder_counters.py:20-22`) and the `xctrace` route
(`summarize_xctrace_metal_intervals.py:22-24`) pattern-match the
identical regex against the encoder label:

```
RenderPass\[seq=(\d+),enc=(\d+),rt=(0x[0-9a-fA-F]+),depth=(0x[0-9a-fA-F]+)\]
```

Verified in the real export above: `Encoder Label` column values for
the first three data rows are literally
`RenderPass[seq=255,enc=0,rt=0x300007f00000018,depth=0x300000100000001]`,
`RenderPass[seq=255,enc=1,...]`, `RenderPass[seq=255,enc=2,...]` — the
exact shape the regex expects, and the same label format
`dxmt9_draw_encoder.mm:2226` emits per
`agents/rules/metal_debugging.rules.md` §2's resource-labeling table.

**Missing export is a hard failure naming the human action.**
`finalize_3dmark05_perf_probe.sh` checks for the file after the
dry-run early exit (`:1635-1637`) and before doing any work
(`:1654-1658`):

```sh
if [[ ! -f "$xcode_csv" ]]; then
  echo "missing Xcode encoder counters CSV: $xcode_csv" >&2
  echo "export it from Xcode Counters > Export Encoder Counters first" >&2
  exit 2
fi
```

This is the literal two-line diagnostic; it names both the missing
path and the specific GUI action (`agents/rules/metal_debugging.rules.md`
§2b's steps 3-6) that produces it. `--dry-run` (`:1635-1637`) exits
`0` *before* this check runs — a dry-run therefore proves the derived
paths and commands are correct, but does **not** prove the human
export has happened; R-HARN-JOIN-2.5's "must not be confused with"
wording is stated precisely because a caller could otherwise read a
clean `--dry-run` exit as evidence the export step is done, when it is
not.

**What the path/save-panel caveat means for this contract.**
`agents/rules/metal_debugging.rules.md` §2b warns that Xcode's save
panel can retain a previous run's `analysis` folder, so a human export
can land at the *wrong* `frame<N>-counters-xcode.csv` path — a
different run's `traces/<run>/analysis/` directory — while this
domain's own path computation (above) still looks in the *current*
run's directory. When that happens, this domain's own missing-file
check (above) fires correctly (the current run's expected path is
genuinely empty), so the failure mode is "loud and correctly
attributed," not silent. The failure mode this domain's checks cannot
by themselves distinguish is the *inverse* case — a human copies or
saves the correct-looking filename into the correct directory, but the
export it contains is actually of the wrong frame or run. §5 states
which existing checks catch that case and which do not.

---

## 5. Detecting a Wrong-Run or Malformed Export, As Implemented

### 5.1 `summarize_xcode_encoder_counters.py` — two opt-in gates, not default-on

`check_xcode_counter_coverage` (`:2766-2790`) is invoked only when
`--require-xcode-counter-coverage` is passed (`:2854`). It checks,
against the *already-loaded* CSV: (a) `missing_required_xcode_columns`
against the 24-name list in §4 (`:493-495`), reporting each missing
name by exact string; (b) at least one summarized row exists; (c) the
summed `gpu_ms` across rows is strictly positive; (d) at least one row
carries a parsed integer `(seq, enc)` from the `RenderPass[...]` label.
A failure prints, per check, `"requirement failed: " + <message>` to
stderr and the process exits `1` (`:2856-2859`).

`check_top_dxmt_join_coverage` (`:2738-2763`) is invoked only when
`--require-dxmt-join-coverage` is passed (`:2881`), together with
`--min-top-dxmt-joined-fraction` (default `1.0`, `:2818-2823`). Over
the top-`N` rows by `gpu_ms` descending (`--top`, default `3`,
`:2830-2835`), it computes `joined_fraction = joined_rows / labeled_rows`
where `labeled_rows` require a parsed `(seq, enc)` and `joined_rows`
additionally require `dxmt_draw_calls > 0` from the join against the
*current run's* `--dxmt-encoders-csv`. This is precisely the mechanism
that catches the "correct path, wrong-run content" case §4 identifies:
an export from a different run's `.gputrace` will not, in general,
carry `(seq, enc)` pairs matching rows in the current run's dxmt
encoder CSV, so `joined_fraction` drops and, at the default
`min_joined_fraction=1.0`, any unmatched top row fails the gate. The
failure message names the exact fraction:
`"top encoder dxmt join coverage is too low ({joined_fraction:.3f}, ...
joined=N, labeled=M)"` (`:2759-2761`).

**Neither check runs unless its flag is passed.**
`finalize_3dmark05_perf_probe.sh` defaults both
`require_xcode_counter_coverage` and `require_dxmt_join_coverage` to
`0` (`:109-110`) and only forwards `--require-xcode-counter-coverage`
/ `--require-dxmt-join-coverage` to the underlying Python invocation
when the caller passes the matching wrapper flag (`:1201-1209`). A
caller who runs the standard recipe without either flag gets a
"joined" CSV and bottleneck report from a wrong-run or malformed
export with **no diagnostic and a zero exit code** — this is a real,
verified gap against R-HARN-JOIN-3.1, stated here rather than implied
closed. `agents/rules/metal_debugging.rules.md` §9's `--require-*`
gate table already steers proof-oriented runs toward passing these
flags (for example `--require-stable-frame-proof` forces both on,
`:955-975` of the finalize script), but a scout/no-gates invocation of
`finalize_3dmark05_perf_probe.sh` does not get this protection by
default.

### 5.2 `summarize_xctrace_metal_intervals.py` — same shape, one already-unconditional coverage line

`--require-xctrace-render-rows` (`:697-701`) fails hard (`SystemExit`)
if zero rows carry a `RenderPass[seq=...,enc=...]` label. `--min-dxmt-
join-coverage` (default `0.0`, meaning off unless raised; `:702-709`)
computes `coverage = matches / len(rows)` and fails with:

```
"dxmt encoder join coverage below required threshold: "
f"{matches}/{len(rows)} ({coverage:.2%}) < {args.min_dxmt_join_coverage:.2%}; "
"check xctrace label coverage and use the same-run 3DMark05 encoder CSV"
```

— a diagnostic that, unlike §5.1's, names the wrong-run failure mode
explicitly ("use the same-run 3DMark05 encoder CSV"). Verified caller:
`scripts/tools/run_3dmark05_system_trace_sidecar.sh:433-446` hardcodes
`--require-xctrace-render-rows --min-dxmt-join-coverage 0.99
--require-route-verdicts` on every invocation — unlike §5.1's
opt-in-by-flag pattern, this `probe`-domain caller of a `join`-domain
script does not let a caller silently skip the coverage gate.

### 5.3 `summarize_xctrace_cpu_threads.py`

This script attributes CPU thread pacing, not GPU encoder counters, so
R-HARN-JOIN-3.1's "wrong-run join" concern does not apply the same
way: it does not join against a per-encoder dxmt CSV by label at all
(§2.3). It is out of scope for this section.

---

## 6. Join Coverage As a Reported Number, Verified Per Script

Per R-HARN-JOIN-4.1/4.2:

**`summarize_xcode_encoder_counters.py` — gap.** The `joined_fraction`
value §5.1 describes is computed only inside
`check_top_dxmt_join_coverage` (`:2738-2763`), and the `total_gpu_ms`
value is computed only inside `check_xcode_counter_coverage`
(`:2766-2790`, specifically `:2779`); both are printed **only when
the check they belong to fails** (`:2856-2859` for the xcode-counter
check's failure branch, `:2884-2887` for the join-coverage check's).
Neither the joined CSV
(`SUMMARY_FIELDS + JOINED_EXTRA_FIELDS`, §7) nor `write_report`'s
Markdown output (`:1395-...`, verified by `grep -n "coverage"` across
the whole script returning no report-body match) carries a "how many
of the top rows joined" figure when the gate is not requested, or when
it is requested and passes. A caller who never passes
`--require-dxmt-join-coverage` gets a joined CSV with no way to read
join coverage back out short of computing it themselves from
`dxmt_draw_calls` columns. **This does not satisfy R-HARN-JOIN-4.1**
for this script as written today; stated as a verified gap, not a
compliant implementation, matching
`specs/experiments/harness/reduce/spec.md` §4's own gap-reporting
style.

**`summarize_xctrace_metal_intervals.py` — positive example.**
`write_markdown` (`:553-...`) unconditionally emits, in every
invocation regardless of whether `--min-dxmt-join-coverage` is passed
or what its value is:

```
f"- Joined dxmt attribution coverage: `{dxmt_matches}/{len(rows)}`"
```

(`:566`). This is a real, already-implemented instance of R-HARN-JOIN-4.1:
the number survives into the Markdown report even on a passing or
gate-free run, not only inside a failure-path diagnostic. This
asymmetry between the two GPU-counter join scripts — one domain, two
different levels of compliance with the same requirement — is exactly
why R-HARN-JOIN-4.2 requires a per-script accounting rather than a
domain-wide claim.

---

## 7. Emitted Artifacts and Column Contract

`summarize_xcode_encoder_counters.py --joined-output` writes the
column list `SUMMARY_FIELDS + JOINED_EXTRA_FIELDS`
(`:51-113`, `:115-440`), verified lengths **61** and **324**
respectively (import-derived per the method in this document's
header), for a joined-CSV total of **385** columns. Verified against
the current-script real capture
`traces/app-d3d9-3dmark05-gt2-order-store-control-phasealigned-frame255-xcode-r1-20260724/analysis/frame255-xcode-dxmt-joined-summary.csv`:
header has exactly 385 fields, and a name-by-name comparison against
the derived `SUMMARY_FIELDS + JOINED_EXTRA_FIELDS` list is an exact
match (all 385 names identical in order). The reduced (non-joined)
`--summary-output` CSV uses `SUMMARY_FIELDS` alone (61 columns),
likewise an exact match against the real
`frame255-counters-summary.csv` header.

**Column counts drift across script versions; do not hardcode them.**
An older real capture at the same path shape,
`traces/app-d3d9-3dmark05-capture-layer-current-r2-20260619/analysis/frame60-xcode-dxmt-joined-summary.csv`
(this file is also gitignored, so its date comes from its own
directory name, `-r2-20260619`, cross-checked against filesystem
mtime `2026-06-19 04:28:23` — the two agree — roughly five weeks
before the frame255 capture above), has a joined-CSV header of **384**
fields and a
`frame60-counters-summary.csv` header of **60** fields — each exactly
one column short of the counts verified above against the current
script. This is not a defect in either file; it is direct evidence
that this domain's column lists have changed at least once since that
capture was produced, and it is exactly the reason R-HARN-JOIN-5.3
requires re-deriving the count from the current script rather than
citing a number this document (or any downstream consumer) hardcodes
from a single historical run.

`summarize_xcode_encoder_counters.py --report-output` writes a
Markdown bottleneck report (§6's "Top Encoders", "Hot Set Aggregate",
"DXMT Encoder Writer/State Breakdown", and related sections,
`:1882-2500`). `finalize_3dmark05_perf_probe.sh` additionally produces,
per real-run evidence in both trace directories above: the reduced
`frame<N>-counters-summary.csv`, the joined
`frame<N>-xcode-dxmt-joined-summary.csv`, and the
`frame<N>-xcode-dxmt-bottleneck-report.md` — all three present
together in both directories, confirming the artifact set
`agents/rules/metal_debugging.rules.md` §2b's step 7 describes is
still what the current wrapper produces.

`summarize_xctrace_metal_intervals.py --output-csv`/`--output-md`
write a per-encoder joined CSV. `write_csv`'s local `fields` tuple
(`:478-517`) has verified length **38** (counted by parsing the
tuple's literal string members out of the function source, since the
tuple is local rather than a module-level constant). Verified against
a real sidecar capture,
`traces/app-d3d9-3dmark05-phase-aligned-gt1-current-v2-r1/analysis/xctrace-metal-gpu-intervals-summary.csv`
(3,795 lines; the whole `traces/` tree is gitignored per
`.gitignore:25`, so this file carries no git history of its own — its
only available date is filesystem mtime, `2026-07-19 16:11:32` per
`stat -f "%Sm"`, checked 2026-07-27): header has exactly 38 fields, and a name-by-name
comparison against the derived `fields` tuple is an exact match (all
38 names identical in order). Three more matching captures exist at
`traces/app-d3d9-3dmark05-p4-native-producer-current-r2-20260618/analysis/`,
`traces/app-d3d9-3dmark05-managed-versioned-gt2-systemtrace-20260719/analysis/`,
and
`traces/app-d3d9-3dmark05-gt2-phase-latency1-systemtrace-20260719/analysis/`,
so this document's initial verification pass (which searched for a
different filename pattern and wrongly concluded no capture existed)
was corrected before this document was finalized — recorded here so a
reader does not have to take "verified" claims in this document on
faith alone.

Downstream consumers that read the joined CSV by exact path or
dedicated flag (verified: `grep -rl` over `scripts/tools/*.py` for
`xcode-dxmt-joined-summary`): `compare_xcode_dxmt_bottlenecks.py`
(`gate` domain per parent spec.md §1's `scripts/tools/compare_*` rule),
invoked inline by `finalize_3dmark05_perf_probe.sh` (§3) as positional
`before`/`after` arguments when `--baseline-joined` is given
(`:1418-1552` of the finalize script, `compare_xcode_dxmt_bottlenecks.py:1758-1759`
for the positional argparse definition). Two more scripts also read
the joined CSV by a dedicated flag —
`analyze_indexed_probe_classes.py --joined-summary`
(`:909` of that script) and `analyze_xcode_dxmt_encoder_attribution.py`
(whose own docstring names `frame<N>-xcode-dxmt-joined-summary.csv` as
its input, `:4`) — but per R-HARN-JOIN-1.3 this document does not
assert either script's domain; they are named here only so a reader
searching for "who reads the joined CSV" gets the complete verified
list, not just the one `gate`-domain reader.

---

## 8. Environment Variables

Per R-HARN-JOIN-6.1, verified `grep -n "os.environ\|getenv"` across
all three Python scripts in this domain returns no match on
2026-07-27 — none of them reads or sets a `DXMT9_*`/`DXMT_*` variable.
`finalize_3dmark05_perf_probe.sh` reads a family of its own
`DXMT_3DMARK05_*` variables (`:6-22`, `:84-128`) purely to source this
domain's own CLI-flag defaults (frame number, output paths, gate
thresholds, comparison labels); verified `grep -n "^export\|os.environ\["
scripts/tools/finalize_3dmark05_perf_probe.sh` returns no match — this
script never forwards a `DXMT9_*`/`DXMT_*` variable into a subprocess
it launches. None of these `DXMT_3DMARK05_*` variables changes what a
dxmt9-produced counter, image, or geometry payload means; per the
parent spec.md §4 "contract-relevant, defined" test, they select which
of this domain's own paths, gates, or thresholds apply to one
invocation, which is the same class of non-contract-relevant knob
`specs/experiments/harness/reduce/requirements.md`'s R-HARN-REDUCE-6.1
describes for that domain — mirrored here rather than restated in
full.

---

## 9. Mode Table

Per R-HARN-JOIN-7.1, every flag across this domain's scripts that
alters output or gating behavior. `finalize_3dmark05_perf_probe.sh`
accepts **123** flags in total (counted from its own `case "$1" in`
dispatch block, `:341-840`: every `--xxx)`/`-h|--help)` arm); most of
them are forwarded verbatim to the `gate`-domain comparison scripts it
invokes per §3 and are out of scope for this document. This table
covers only the flags that control this domain's own `external-join`
behavior.

### 9.1 `finalize_3dmark05_perf_probe.sh`

| Flag | Effect |
|---|---|
| `--suffix` / `--run-id` / `--frame` | Select which run's artifacts to join; derive `output_dir`/`trace_dir`/`xcode_csv` defaults (`:1098-1131`). |
| `--output-dir` / `--trace-dir` / `--xcode-csv` | Override the derived paths in §4 directly. |
| `--top` | GPU-time-ranked row count used by `--require-top-pso-attribution` / `--require-dxmt-join-coverage` (forwarded as `summarize_xcode_encoder_counters.py --top`, default `3`). |
| `--hot-gpu-share` | Forwarded as `summarize_xcode_encoder_counters.py --hot-gpu-share` (report-only Hot Set Aggregate target, default `95.0`). |
| `--require-top-pso-attribution` / `--min-top-pso-samples-per-draw` | Forwarded gate: top rows' `dxmt_pso_state_samples`/`dxmt_draw_calls` must meet the threshold (`:2717-2735` of the Python script). |
| `--require-xcode-counter-coverage` | Forwarded gate; see §5.1. |
| `--require-dxmt-join-coverage` / `--min-top-dxmt-joined-fraction` | Forwarded gate; see §5.1. |
| `--require-shader-dump-matches` | Forwarded to `analyze_shader_dumps.py` (domain not asserted, §1); listed here only because this wrapper's own flag surface includes it. |
| `--dry-run` | Prints every derived path and every command this script would run, then exits `0` **before** the Xcode-CSV existence check (`:1635-1658`); see R-HARN-JOIN-2.5. |
| `--require-result-json` | Fail instead of falling back to partial `dxmt9.log` counters when `result.json` is absent (`:1639-1653`). |

### 9.2 `summarize_xcode_encoder_counters.py`

| Flag | Effect |
|---|---|
| `xcode_csv` (positional) | The human-exported CSV to reduce and join (§4). |
| `--dxmt-log` / `--dxmt-encoders-csv` | Secondary dxmt attribution source (§2.1); `--dxmt-encoders-csv` wins if both given (`:556-561`). |
| `--dxmt-streams-csv` | Optional per-stream dxmt breakdown, used only in the Markdown report's per-stream section. |
| `--summary-output` / `--joined-output` / `--report-output` | Output path overrides; default derivation in `default_output_paths` (`:2701-2714`) matches the `-counters-xcode.csv` → `-counters-summary.csv` / `-xcode-dxmt-joined-summary.csv` / `-xcode-dxmt-bottleneck-report.md` naming `finalize_3dmark05_perf_probe.sh` itself expects. |
| `--require-top-pso-attribution` / `--min-top-pso-samples-per-draw` | See table above. |
| `--require-xcode-counter-coverage` | See §5.1. |
| `--require-dxmt-join-coverage` / `--min-top-dxmt-joined-fraction` | See §5.1. |
| `--top` / `--hot-gpu-share` | See table above. |

### 9.3 `summarize_xctrace_metal_intervals.py`

| Flag | Effect |
|---|---|
| `--gpu-intervals` (required) | The `xctrace`-exported `metal-gpu-intervals` XML (§2.2). |
| `--dxmt-encoders` (required) | Secondary dxmt attribution CSV (§2.2, required unlike §9.2's optional equivalent). |
| `--indexed-probe-draws` | Optional route-attribution source, joined by the same label. |
| `--output-csv` / `--output-md` | Output paths (required). |
| `--require-xctrace-render-rows` | See §5.2. |
| `--min-dxmt-join-coverage` | See §5.2; default `0.0` means off unless raised. |
| `--require-route-verdicts` / `--require-indexed-probe-routes` | Fail if zero rows joined a route verdict / indexed-probe route (`:711-726`). |

### 9.4 `summarize_xctrace_cpu_threads.py`

| Flag | Effect |
|---|---|
| `--time-profile` (required) | The `xctrace`-exported symbolicated sampled-stack XML (§2.3). |
| `--time-sample` / `--thread-info` | Optional secondary `xctrace` exports (thread-state distribution, thread metadata). |
| `--process-regex` | Process-name filter for attributing samples (default `3DMark05\.exe`). |
| `--keyword` (repeatable) | Extends the default P4-wait/holder keyword lists (`:21-58`) used to classify sampled stacks. |
| `--output-csv` / `--output-md` / `--output-verdict-json` | Output paths. |
