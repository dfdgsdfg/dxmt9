---
type: "Spec"
title: "Harness Gate Spec — Comparison and Proof"
description: "Script inventory, proof-gate flag families, the degenerate-input false-pass reproductions, and the noise-floor/baseline rules, verified."
tags: [specs, experiments, harness, gate, spec]
---

# Harness Gate Spec — Comparison and Proof

Implements `specs/experiments/harness/gate/requirements.md`
(`R-HARN-GATE-*`). Instantiates the `gate` row of the domain map in
`specs/experiments/harness/spec.md` §1 and the `log-reduce →
compare-gate` and `external-join → compare-gate` boundaries in that
spec's §2. Stage names, boundary names, and envelope fields are cited
from the parent spec rather than redefined here.

Facts below were verified against the six scripts named in
requirements.md §1, at their line counts and line numbers on
2026-07-27 (`wc -l`, and direct `Read`/`grep -n` against the working
tree at commit `3835ca00`), and by actually running two of them
against freshly generated degenerate fixtures (§4) rather than
assuming their behavior from source alone. Flag lists were derived by
invoking each script's own `--help` and counting with `grep -c`, not by
hand-counting prose, following
`specs/experiments/harness/reduce/spec.md`'s and
`specs/experiments/harness/join/spec.md`'s own verification discipline
(R-HARN-REDUCE-5.2/R-HARN-JOIN-5.3), applied here to flag surfaces
instead of CSV columns.

---

## 1. Script Inventory

| Script | Lines (`wc -l`) | Role |
|---|---:|---|
| `compare_3dmark05_p4_pair.py` | 155 | Judges one paired P4 deferred-boundary scout (`result.json` before/candidate) against four always-on gates (correctness, P4 wait shift, command-buffer/render-pass locality, semantics) plus an FPS delta, emitting a three-way `WIN`/`LOSE`/`REPEAT` verdict. |
| `compare_3dmark05_perf_counters.py` | 4,374 | The domain's largest run-level comparator. Reads two `result.json` (or `dxmt9.log`-fallback) counter snapshots directly, optionally augmented by the `reduce`-domain `3dmark05-perf-encoders.csv` sidecar for a small `encoder_sidecar_*` subset, and exposes 58 `--require-*` plus 2 `--max-*` proof-gate flags (63 flags total) over render-pass, draw-run, argbuf, and P4-completion-wait CPU counters. |
| `compare_attachment_dumps.py` | 200 | Byte-exact comparator for raw dxmt9 attachment dumps (a binary payload plus a `.json` metadata sidecar); one gate, `--require-exact`. |
| `compare_experiment_images.py` | 583 | Pixel comparator for two screenshots (full frame, optional bottom crop, optional ROIs); exposes named `exact`/`lsb1` policy presets plus individually settable thresholds (19 flags total). |
| `compare_xcode_dxmt_bottlenecks.py` | 2,046 | Comparator for two `join`-domain `frame<N>-xcode-dxmt-joined-summary.csv` files; exposes 18 `--require-*` plus 12 `--max-*` proof-gate flags (35 flags total), including composite proof gates that expand into several individual checks. |
| `analyze_xcode_replay_variance.py` | 310 | Not a before/after comparator — reads N >= 3 raw Xcode counter exports of the *same* `.gputrace` and reports per-(encoder, metric) coefficient of variation, i.e. this domain's own noise-floor measurement tool. |

`155 + 4374 + 200 + 583 + 2046 + 310 = 7668` total lines across the six
scripts, matching the sum `wc -l` itself reports for the six paths in
one invocation. Per R-HARN-GATE-1.1, this is exactly the six scripts
the parent domain map's `gate` row names (`scripts/tools/compare_*`
plus `scripts/tools/analyze_xcode_replay_variance.py`); `ls
scripts/tools/compare_*.py` returns exactly these five `compare_*`
paths and no others.

---

## 2. Input Origin, Verified Against the Parent's Own Example

Parent spec.md §2's `log-reduce → compare-gate` boundary text uses
`compare_3dmark05_perf_counters.py` as its own example of a consumer
of "`reduce`-domain summary CSV/Markdown output." Read against the
actual script, this is imprecise in one respect worth recording
plainly rather than restating uncritically: `compare_3dmark05_perf_counters.py`'s
*primary* input, per `counter_source_path`/`load_counters`
(`:493-524`), is `result.json` directly (or `dxmt9.log` as a fallback
for an interrupted run) — both are `runner`-domain artifacts, not
`reduce`-domain CSV output. The `reduce`-domain
`3dmark05-perf-encoders.csv` is read only as an optional *secondary*
sidecar, inside `augment_with_encoder_sidecar_metrics` (`:664-...`),
gated on the file's existence (`if not csv_path.exists(): return
counters`, `:669-670`) and contributing only the
`encoder_sidecar_*` counter family, not the counters the bulk of this
script's 58 `--require-*` flags check. This mirrors the same kind of
correction `specs/experiments/harness/reduce/spec.md` §2.2 already made
for `summarize_index_cache_runtime.py`'s second-order input — a gap in
the parent's illustrative "for example" wording, not a disagreement
with which domain owns the script. `compare_xcode_dxmt_bottlenecks.py`
is the clean fit for the boundary as literally described: its sole
input (`load_rows`, `:46-50`) is a `join`-domain joined CSV, read via
two required positional arguments (`before`, `after`).

---

## 3. Proof-Gate Flag Families — What Each Proves

### 3.1 `compare_xcode_dxmt_bottlenecks.py`

18 `--require-*` flags (verified:
`python3 scripts/tools/compare_xcode_dxmt_bottlenecks.py --help | grep -cE '^  --require-'`
→ `18`) plus 12 `--max-*` regression-tolerance flags (same method,
`--max-` → `12`), for 30 gate-shaping flags out of 35 total (the
remaining 5 are `--before-label`/`--after-label`/`--top`/`--output`/
`--target-row-key`).

**Named-mechanism proof gates**, each named after what it proves
rather than only the numeric direction (R-HARN-GATE-2.1):

| Flag | Mechanism it proves |
|---|---|
| `--require-tvb-mechanism-proof` | Top-N hidden backend write, `VS Buffer Device Memory Bytes Written`, `vs_invocations`, and `gpu_ms` all strictly decrease together — the specific TVB/parameter-buffer-pressure claim `agents/rules/metal_debugging.rules.md`'s TVB section describes, not a generic GPU-time win. |
| `--require-stable-frame-proof` | Composite (below): top row-key set unchanged, top GPU/VS-buffer-write/unexplained-write all decrease, top draw/vertex/triangle drift within a tolerance that defaults to 5% when the caller does not override it (`:1994-1999`). |
| `--require-cache-opt-apply-proof` | Composite: `--require-stable-frame-proof` plus target-row actual LRU32 miss decrease, VS buffer-write decrease, and VS-invocation decrease — proof that a specific target row's index-cache reorder was actually *applied*, not merely that some row somewhere improved. |
| `--require-target-index-cache-miss32-decrease` | Generic actual indexed LRU32 miss-estimate telemetry decreased on the target row(s) — the diagnostic-probe shape of cache-locality proof. |
| `--require-target-index-cache-opt-miss32-decrease` | The *production* cache-opt path's own candidate/effective LRU32 telemetry decreased — a distinct mechanism from the row above because the production path binds cached reordered prelookup buffers and does not emit the generic indexed-cache fields the diagnostic-probe flag checks. |
| `--require-target-reordered-index-cache-hits` | The submitted reordered-buffer path is accounted for by cache-hit counters, a third distinct mechanism from the two rows above, used for production cached-IB proofs specifically. |

The three index-cache flags above are the concrete evidence for
R-HARN-GATE-2.1's "more than one mechanism could produce the same
numeric direction" case: all three could be described loosely as
"index-cache locality improved," but each checks a different counter
family (generic actual telemetry vs. production candidate/effective
telemetry vs. cache-hit counters) and the script's own `--help` text
states the distinction explicitly for each (quoted verbatim above from
the real `--help` output), rather than collapsing them into one flag.

**Composite flags name their failing constituent (R-HARN-GATE-2.2,
verified compliant).** `args.require_cache_opt_apply_proof` sets
`args.require_stable_frame_proof = True` plus three target-specific
flags (`:1983-1987`); `args.require_stable_frame_proof` in turn sets
`require_top_gpu_decrease`, `require_top_vs_buffer_write_decrease`,
`require_top_unexplained_buffer_write_decrease`,
`require_top_row_key_match`, and defaults the three
`max_top_*_delta_ratio` tolerances to `0.05` only if the caller left
them unset (`:1989-1999`) — this is the literal source of the
`--help` text's "top draw/vertex/triangle drift defaults to <= 5%
unless overridden." Because these are ordinary flag assignments
evaluated *before* `failed_requirements()` runs (`:2001-2005`), and
that function appends one independently labeled string per failing
individual check (`require_decrease`/`require_increase`/
`require_delta_ratio_at_most` closures, `:1381-1467`, each taking its
own `label` argument), a failing composite gate's diagnostic still
names exactly which constituent failed — for example "top_gpu_ms did
not decrease (...)" — not a bare "stable frame proof failed." Verified
by reading the closures' call sites; not reproduced with a live failing
run in this pass.

### 3.2 `compare_3dmark05_perf_counters.py`

58 `--require-*` flags plus 2 `--max-*` flags (same `--help | grep -c`
method), 63 total. Unlike §3.1, this script has no composite proof
flag — every flag is atomic, each mapped to one named CPU-cost bucket
(for example `--require-snapshot-cache-lookup-cpu-per-present-decrease`
vs. `--require-snapshot-cache-uniform-build-cpu-per-present-decrease`
vs. `--require-batch-miss-uniform-hash-cpu-per-present-decrease`) so a
caller proves a specific cost center moved rather than "CPU per present
went down" generically. Its own docstring (`:2-9`) states its
complementary relationship to `compare_xcode_dxmt_bottlenecks.py`
directly: "Xcode encoder counters prove GPU-frame effects, while this
report verifies the run-level mechanisms that a candidate change
intended to move." `failed_requirements()` (`:3421-...`) uses the same
per-check-labeled-failure shape as §3.1's underlying closures
(`require_derived_decrease`/`require_derived_not_increase`/
`require_available_derived_not_increase`, `:3431-3460`).
`agents/rules/metal_debugging.rules.md`'s own text states this
domain's mechanism-naming intent directly: "use gates such as
`--require-draw-run-records-increase`, ...,  and
`--require-encode-draw-cpu-decrease` so the intended mechanism is
proven by `result.json` counters before interpreting Xcode frame
counters" — a literal instance of R-HARN-GATE-2.1's rule, quoted
rather than paraphrased.

### 3.3 `compare_3dmark05_p4_pair.py`

Four gates, none opt-in — `run_correct`, the P4 wait-shift check, the
locality check, and the semantics check (`:47-134`) always run and are
all required for a `WIN`/`LOSE` verdict to be reached at all (`:140`:
`hard_gates = correct and p4 and locality and semantics`); only the
FPS delta is compared against a tolerance (`--noise-pct`, default
`5.0`). This is the one script in this domain whose "did this
mechanism actually happen" checks (the `semantics` row: "candidate
present_boundary_applied > 0 and present_boundary_deferred > 0",
`:125-134`) are unconditional rather than caller-requested — a design
this domain's other two comparators do not share, and this document
does not generalize it into a requirement the other two must adopt.

### 3.4 `compare_experiment_images.py` and `compare_attachment_dumps.py`

Neither exposes a named-mechanism proof gate in the §3.1/3.2 sense;
both compare raw pixel/byte content against similarity thresholds.
Their flag surfaces are covered in §8.4/§8.5; §4 covers their behavior
on identical, degenerate inputs, which is what this domain's own
requirements.md §3 is about.

### 3.5 `analyze_xcode_replay_variance.py`

Not a proof gate in the before/after sense — it reports variance
within one artifact's own repeated re-export, gated only by an optional
`--max-cv-pct`. Covered in §5.

---

## 4. Degenerate-Input Behavior, Reproduced Live

Per R-HARN-GATE-3.4, both reproductions below were run on 2026-07-27
against freshly generated fixtures, not inferred from source alone.

### 4.1 `compare_experiment_images.py --policy exact`

Two bit-identical 8x8 all-black (RGB `(0,0,0)`) PNGs:

```sh
$ python3 scripts/tools/compare_experiment_images.py \
    --before black1.png --after black2.png \
    --policy exact --output report.md
$ echo "exit=$?"
exit=0
```

`report.md`'s body (verbatim):

```
- Passed: all requested image gates were satisfied.
```

with the metrics row showing `changed_pct=0.000000%`,
`before_active_pct=0.000000%`, `after_active_pct=0.000000%`,
`ssim=1.000000`. **Cause, verified against source:**
`POLICY_PRESETS["exact"]` (`:46-49`) sets only `max_changed_pct=0.0`
and `min_ssim=1.0`; `gates_requested` (`:528-536`) is `True` here
because `args.policy is not None`, but `min_before_active_pct`/
`min_after_active_pct` fall through `policy_value()`'s fallback
(`:298-308`) to their own CLI defaults of `0.0` (`:503-514`) because
neither key exists in the `"exact"` preset dict — so the one check
that could catch an all-black frame (`item.after_active_pct <
min_after_active_pct`, `requirement_failures`, `:273-277`) never
fires, since `0.0 < 0.0` is false. Adding
`--min-after-active-pct 1` to the same command reproduces a correct
failure instead: `requirement failed: full: after_active_pct 0.000000
< 1.000000`, exit `1` — the mechanism exists in this script; it is
simply not part of either named policy preset (`POLICY_PRESETS`,
`:45-57`, neither `"exact"` nor `"lsb1"` sets either
`min_*_active_pct` key).

### 4.2 `compare_attachment_dumps.py --require-exact`

Two byte-identical 64-byte all-zero dumps with matching metadata
sidecars (`format`/`metalPixelFormat`/`width`/`height`/`rowBytes`/
`byteCount` all equal):

```sh
$ python3 scripts/tools/compare_attachment_dumps.py \
    --before zero1.bin --after zero2.bin --require-exact \
    --output report.md --summary-output summary.csv
$ echo "exit=$?"
exit=0
```

`report.md`'s body (verbatim):

```
- Passed: all attachment dumps are byte-exact and metadata-compatible.
```

**Cause, verified against source:** `write_report`'s failure predicate
(`:139-142`) is `row["changed_bytes"] != "0" or row["metadata_status"]
!= "compatible"` — both are false for two identical all-zero buffers
with matching metadata, so `--require-exact`'s own exit check
(`:190`) also reports success. Unlike §4.1, this script has **no**
active-content, non-zero-byte, or non-degeneracy check anywhere in its
source — `CSV_FIELDS` (`:13-36`) even carries
`before_active_pixels`/`after_active_pixels`/`before_active_pct`/
`after_active_pct` columns (the same names `compare_experiment_images.py`
populates), but `compare_bytes` (`:69-120`) always writes them as
empty strings (`:104-107`) and never computes them. There is no flag,
opt-in or otherwise, that would have caught this case.

### 4.3 What this demonstrates for R-HARN-GATE-3.1/3.4

Both reproductions are the identical shape of the parent's four-black-
image incident — two degenerate, content-free artifacts compare as
equal and the comparator reports success — but found live in this
domain's own current scripts rather than only in the historical
mini-replay case. §4.1's gap is narrower (an existing check is simply
excluded from the named presets); §4.2's is total (no such check
exists in the script at all).

### 4.4 The envelope's `validity` field is not consulted anywhere in this domain

`grep -n "os.environ\|getenv" scripts/tools/compare_*.py
scripts/tools/analyze_xcode_replay_variance.py` returns no match (§7),
and a separate check for the envelope field itself —
`grep -n "\"validity\"\|'validity'\|envelope" scripts/tools/compare_*.py
scripts/tools/analyze_xcode_replay_variance.py` — also returns no
match on 2026-07-27. This is consistent with, not contradictory to,
parent spec.md §5's own statement that "No migration step has been
performed": R-HARN-GATE-3.2's envelope-consultation mechanism cannot
exist in this domain's scripts yet because no upstream producer this
domain reads (`result.json`, the `reduce`-domain CSVs, the `join`-domain
joined CSV) writes a `validity` field for this domain to consult. §4.1
and §4.2 above are exactly the gap R-HARN-GATE-3.2/3.3 describe filling
once that field exists; today, both scripts' own ad-hoc
active-pixel/byte checks are the only mechanism available, and §4.1/4.2
record precisely how incomplete that mechanism is.

---

## 5. Noise-Floor / Inconclusive Verdict, Per Script

Per R-HARN-GATE-4.2:

| Script | Distinct inconclusive state? | Verified shape |
|---|---|---|
| `compare_3dmark05_p4_pair.py` | **Yes.** | `fps_win`/`fps_lose` are computed against `±args.noise_pct` (default `5.0`, `:80-84`); when the four hard gates in `hard_gates` (`:140`) all pass and neither `fps_win` nor `fps_lose` fires, the script prints `"VERDICT: REPEAT (FPS inside noise band)"` and returns exit code `2` (`:150-151`), distinct from `WIN` (`0`, `:144-146`) and `LOSE` (`1`, `:141-143,147-149`). |
| `compare_xcode_dxmt_bottlenecks.py` | No. | `--max-*-regression-*` flags (§3.1) define a tolerance a delta must stay within, but clearing the tolerance and having a wide safety margin both report as the same pass; there is no third exit code or report state distinguishing them. |
| `compare_3dmark05_perf_counters.py` | No. | Same shape as the row above: `--max-gpu-command-buffer-regression-ms`/`--max-const-upload-break-count-ratio` are tolerances, not a reported inconclusive state. |
| `compare_experiment_images.py` / `compare_attachment_dumps.py` | No. | Pixel/byte similarity thresholds; no noise-floor concept at all. |
| `analyze_xcode_replay_variance.py` | N/A — not a before/after comparator; its own noise-floor behavior is described below. | |

**`analyze_xcode_replay_variance.py`'s `--max-cv-pct` default is
`None`, not 5%.** Verified directly from its own `argparse` definition
(`:269-271`): `parser.add_argument("--max-cv-pct", type=float,
default=None, ...)`, and `main()` only evaluates the gate "if
args.max_cv_pct is not None" (`:291`) — omitting the flag makes this
script report-only, with no default noise-floor threshold enforced.
The "5" that appears in
`agents/rules/metal_debugging.rules.md`'s own worked example
(`--max-cv-pct 5`) is that runbook's own recommended value, passed
explicitly on the command line; it is not a value this script falls
back to on its own. This document does not attribute a "typically
under 5%" claim to that rules file because it does not contain that
sentence — the literal text it does contain is: "Use this tool
whenever an A/B comparison reports a sub-10% delta or when only some
encoders move and others appear noisy." That sentence, not a specific
percentage, is the actual documented trigger for running this script.

**The link between variance measurement and the directional gates is
procedural, not mechanical (R-HARN-GATE-4.3).** No script in this
domain invokes `analyze_xcode_replay_variance.py`, and no script reads
its `--summary-output` CSV or its report Markdown before computing a
`compare_xcode_dxmt_bottlenecks.py`/`compare_3dmark05_perf_counters.py`
verdict — verified by `grep -rl "replay_variance"
scripts/tools/compare_*.py`, which returns no match. A human follows
`agents/rules/metal_debugging.rules.md`'s documented five-step replay
procedure and judges the CV report by eye before deciding whether to
trust a small `compare_xcode_dxmt_bottlenecks.py` delta; nothing in
this domain enforces that ordering in code.

**A broken citation, noted rather than repeated.** Both this script's
own docstring (`:8`, "See
docs/superpowers/specs/2026-06-03-xcode-replay-variance-design.md.")
and `agents/rules/metal_debugging.rules.md`'s "Design:" line cite a
design document at that path. `find . -iname
'*replay-variance*'` under `docs/superpowers/` returns no match, and
`ls docs/superpowers/specs/` lists only three unrelated
2026-07-2x-dated files — the cited design document does not exist in
the working tree on 2026-07-27. This document does not restate any
content from that missing file; it records the citation as broken so a
reader does not go looking for it as if it were merely unread.

---

## 6. Baseline-Required Enforcement, Verified

### 6.1 This domain's own two-required-positional-argument shape (R-HARN-GATE-5.1)

`compare_xcode_dxmt_bottlenecks.py` (`before`/`after`, `:1758-1759`)
and `compare_3dmark05_perf_counters.py` (`before`/`after`, `:4023-4024`)
both declare their two inputs as required positional arguments, not
optional flags. Verified: invoking either with zero or one argument
fails at `argparse`'s own level before any file is opened —
`compare_xcode_dxmt_bottlenecks.py` with no arguments: `error: the
following arguments are required: before, after`, exit `2`; with one
argument: `error: the following arguments are required: after`, exit
`2`. Invoking either with a nonexistent path for `before` fails with a
named diagnostic once execution reaches `load_rows`/`load_counters`:
`compare_xcode_dxmt_bottlenecks.py /tmp/nonexistent-before.csv
/tmp/nonexistent-after.csv` → `missing joined summary:
/tmp/nonexistent-before.csv` (`:47-48`), exit `1`. This domain's other
four scripts likewise take `--before`/`--after` as `required=True`
flags (`compare_experiment_images.py:423-424`,
`compare_attachment_dumps.py:174-175`) or `--baseline`/`--candidate`
(`compare_3dmark05_p4_pair.py:67-68`), so none of the six scripts in
this domain can be invoked with only one side of a comparison.

### 6.2 The wrapper-level baseline-presence check (R-HARN-GATE-5.2/5.3) belongs to `probe`/`join`, not to this domain

The rule the task brief and `agents/rules/metal_debugging.rules.md`
state — "run-level gates require `--baseline-output`" / "Xcode
comparison gates require `--baseline-joined`" — is implemented in two
wrapper scripts outside this domain, each with its own flag spelling,
verified directly rather than assumed identical:

| Wrapper (domain) | Run-level flag | Xcode-joined flag | Check site |
|---|---|---|---|
| `run_3dmark05_perf_probe.sh` (`probe`) | `--compare-baseline-output` | `--baseline-joined` | `:3971-3994` — `"run-level comparison gates require --compare-baseline-output <output-dir>"` / `"Xcode comparison gates require --baseline-joined <joined.csv>"`, both `exit 2` |
| `finalize_3dmark05_perf_probe.sh` (`join`) | `--baseline-output` | `--baseline-joined` | `:1039-1096` — `"run-level comparison gates require --baseline-output <output-dir>"` / `"Xcode comparison gates require --baseline-joined <joined.csv>"`, both `exit 2` |

The two wrappers agree on `--baseline-joined` but disagree on the
run-level flag's own name (`--compare-baseline-output` vs.
`--baseline-output`); `specs/experiments/harness/probe/spec.md` §7.2
already records this exact mismatch, including that
`run_3dmark05_perf_probe.sh` translates its own
`--compare-baseline-output` into `finalize_3dmark05_perf_probe.sh`'s
`--baseline-output` spelling when it prints the follow-up
`finalize_cmd` (`run_3dmark05_perf_probe.sh:5305-5309`) — this
document cites that fact rather than re-deriving it, per
R-HARN-GATE-1.2/5.3. Both checks run *before* the corresponding
wrapper invokes any `gate`-domain script (`run_3dmark05_perf_probe.sh`
only assembles `counter_compare_cmd` when `$compare_baseline_output` is
non-empty, `:5104-5108`; `finalize_3dmark05_perf_probe.sh`'s checks at
`:1039-1096` run ahead of its own `compare_*` invocations at `:1661+`,
per `specs/experiments/harness/join/spec.md` §3), so a caller who
requests a proof gate without supplying its baseline is stopped with a
named diagnostic before this domain's scripts run at all — not left
with a wrapper that silently skips the gate and still exits zero.

### 6.3 What this domain does not itself guarantee

Per R-HARN-GATE-5.3, §6.1/6.2 together stop a caller from omitting a
baseline path entirely; neither stops a caller from pointing
`--baseline-output`/`--baseline-joined`/`--compare-baseline-output` at
the *wrong* run. That is `R-HARN-JOIN-3.1`/`3.2`'s concern
(`summarize_xcode_encoder_counters.py`'s opt-in
`--require-dxmt-join-coverage` gate, per
`specs/experiments/harness/join/spec.md` §5.1) and is out of scope for
this document.

---

## 7. Environment Variables

Per R-HARN-GATE-6.1, verified `grep -n "os.environ\|getenv"
scripts/tools/compare_3dmark05_p4_pair.py
scripts/tools/compare_3dmark05_perf_counters.py
scripts/tools/compare_attachment_dumps.py
scripts/tools/compare_experiment_images.py
scripts/tools/compare_xcode_dxmt_bottlenecks.py
scripts/tools/analyze_xcode_replay_variance.py` returns no match
across all six files on 2026-07-27 — this domain reads and sets no
`DXMT9_*`/`DXMT_*` environment variable. A `DXMT_3DMARK05_MAX_CONST_UPLOAD_BREAK_COUNT_RATIO`-
shaped variable documented in
`agents/rules/environment_variables_perf.rules.md` and forwarded as
this domain's own `--max-const-upload-break-count-ratio` flag is read
and resolved entirely by the `probe`/`join` wrapper scripts before this
domain's script ever starts, per R-HARN-GATE-6.2.

---

## 8. Mode Table

Per R-HARN-GATE-7.1. §3 above already covers the proof-gate families
in depth (`--require-*`/`--max-*` across `compare_xcode_dxmt_bottlenecks.py`
and `compare_3dmark05_perf_counters.py`, 18+12 and 58+2 respectively);
this table covers the remaining flags and the two smaller scripts.

### 8.1 `compare_xcode_dxmt_bottlenecks.py` (35 flags total)

| Flag | Effect |
|---|---|
| `before` / `after` (positional, required) | The two joined CSVs to compare (§6.1). |
| `--before-label` / `--after-label` | Report labels only. |
| `--top` | Row count ranked by `gpu_ms` descending that every top-N gate operates over (default `3`). |
| `--output` | Markdown report path. |
| `--target-row-key` (repeatable) | Defines the target set for target-row gates (§3.1) and excludes those rows from non-target regression gates. |
| 18 `--require-*` / 12 `--max-*` | §3.1. |

### 8.2 `compare_3dmark05_perf_counters.py` (63 flags total)

| Flag | Effect |
|---|---|
| `before` / `after` (positional, required) | The two `result.json`/output-dir counter sources (§6.1). |
| `--before-label` / `--after-label` | Report labels only. |
| `--output` | Markdown report path. |
| 58 `--require-*` / 2 `--max-*` | §3.2. |

### 8.3 `compare_3dmark05_p4_pair.py` (4 flags total)

| Flag | Effect |
|---|---|
| `--baseline` / `--candidate` (required) | The two run output directories (§6.1). |
| `--noise-pct` | FPS band (default `5.0`) inside which the verdict is `REPEAT` rather than `WIN`/`LOSE` (§5). |
| `--locality-slack-pct` | Allowed relative growth (default `2.0`) for the four `LOCALITY_KEYS` counters before the locality gate fails. |

### 8.4 `compare_experiment_images.py` (19 flags total)

| Flag | Effect |
|---|---|
| `--before` / `--after` (required) | The two screenshots (§6.1). |
| `--label-before` / `--label-after` | Report labels only. |
| `--crop-bottom` | Also compares a bottom-cropped region (extra HUD/overlay strip exclusion). |
| `--roi` (repeatable, `L,T,R,B[:name]`) | Also compares one or more named regions of interest. |
| `--active-threshold` | RGB max-value cutoff above which a pixel counts as active/non-black for the `*_active_pct` metrics (§4.1). |
| `--output` / `--summary-output` / `--diff-output` | Report/CSV/heat-map-diff-image paths. |
| `--policy {exact,lsb1}` | Named threshold preset (§4.1); does not set `min_before_active_pct`/`min_after_active_pct`. |
| `--require-similar` | Forces `gates_requested = True` without a policy, using the flag defaults below. |
| `--max-changed-pct` / `--min-ssim` / `--max-delta` / `--max-mean-abs-delta` / `--max-rms-delta` | Individually settable thresholds; explicit values win over a `--policy` preset (`policy_value`, `:298-308`). |
| `--min-before-active-pct` / `--min-after-active-pct` | The non-degeneracy check (§4.1); default `0.0` (off) and absent from both named policies. |

### 8.5 `compare_attachment_dumps.py` (6 flags total)

| Flag | Effect |
|---|---|
| `--before` / `--after` (required) | The two dump files, each requiring a sibling `<path>.json` metadata sidecar (§4.2). |
| `--area` | Report row label only. |
| `--output` / `--summary-output` (required) | Report/CSV paths. |
| `--require-exact` | The only gate this script has (§4.2): fails if any byte differs or metadata is incompatible; never checks for degenerate (all-zero) content. |

### 8.6 `analyze_xcode_replay_variance.py` (1 required positional argument, 5 `--` flags)

| Flag | Effect |
|---|---|
| `csvs` (positional, >= 3 required, enforced at `:275-276`) | The repeated exports of one `.gputrace` to compare against each other. |
| `--output` (required) | Markdown variance report path. |
| `--summary-output` | Optional per-(encoder, metric) CSV. |
| `--metric` (repeatable) | Restricts to named metrics from `METRIC_MAPPING` (`:21-28`); defaults to `DEFAULT_METRICS` (`:31-38`, 6 metrics). |
| `--max-cv-pct` | Off (`None`) by default (§5); when set, fails with a named `encoder`/`metric`/`cv_pct` diagnostic per violation (`:298-305`). |
| `--row-label-column` | Overrides the CSV column used as the per-row join key (default `"Encoder Label"`). |
