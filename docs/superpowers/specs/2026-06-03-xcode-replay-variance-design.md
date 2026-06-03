# 2026-06-03 — Xcode Replay Variance Characterization Design

Status: design approved, awaiting user spec review before plan handoff.

## Context

The TVB pressure mechanism proof closed in
`docs/superpowers/specs/2026-06-03-tvb-mechanism-proof-design.md`. The
remaining open work is Path 1: cached LRU32 index reorder applied to a
full GT1 frame must pass `--require-stable-frame-proof`, but currently
fails because non-target rows (`50/0`, `50/2`, `50/4`, `50/11`) drift
upward by `+20%` or more even though only `50/1` and `50/3` are
mutated.

The drift is observed only on the **Xcode replay** side. The matching
no-gputrace run produced negligible run-level counter movement
(`encode_draw_cpu` `-0.55%`, GPU command-buffer time `-4.29%`), no map
or queue waits, and no pass-shape changes. This raises the possibility
that the gputrace-based comparison reports drift the actual application
does not exhibit.

Hypothesis space (from brainstorming):

| H | Origin | Verification | Cost |
|---|---|---|---|
| N | Xcode replay non-determinism (same `.gputrace` replays differ across counter exports) | Reopen one `.gputrace`, export counters N times, compute per-row CV | Low |
| R | Capture-to-capture non-determinism (same probe command produces different `.gputrace` bundles) | Capture same command 5 times, export each once, compute CV | Medium |
| S | Systematic perturbation (mutating 50/1,3 actually changes 50/11 etc.) | Eliminate N and R first; residual drift is S | High |
| M | gputrace-based gate semantics are wrong (no-gputrace is the truth) | Define new gate, accept on no-gputrace counters | Low |

The user picked **N first** because it is the cheapest investigation
and either rules out replay noise or quantifies it before R/S work is
considered.

## Goal

Quantify Xcode replay variance by re-exporting encoder counters from
the same already-captured `.gputrace` bundle multiple times. Compare
the per-encoder, per-metric coefficient of variation against the
observed `~20%` drift in non-target rows. Use the result to decide:

- If non-target row CV is below 5% on the key metrics (`gpu_ms`,
  `vs_buffer_write_mib`, `vs_invocations`,
  `tiled_vertex_buffer_mib`, `tiled_primitive_block_mib`), reject
  hypothesis N. Drift is real; proceed to hypothesis R or S.
- If non-target row CV is at the same order of magnitude as the
  observed drift, hypothesis N is supported. The gputrace-based
  comparison gate is unreliable for this workload. Production
  acceptance should fall back to no-gputrace counters (hypothesis M
  follow-up).

## Non-goals

- No new GPU capture. Disk is at 98% used; only existing `.gputrace`
  bundles are reused.
- No `src/` runtime or probe code change.
- No `xcrun xctrace` GUI-bypass automation. That belongs to a
  follow-up if N is unresolved by manual replay.
- No mini-replay binary or manifest format change.
- No change to `--require-stable-frame-proof` or
  `--require-tvb-mechanism-proof`. The new variance tool is a
  parallel diagnostic.
- Hypothesis R, S, M characterization is out of scope. They become
  candidate next steps once N is resolved.

## External grounding

- Apple's Metal "GPU counters and counter sample buffers" doc
  (https://developer.apple.com/documentation/metal/gpu-counters-and-counter-sample-buffers)
  describes counter sample buffers as snapshots of real GPU execution
  state at the configured stage boundary. Counters reported by Xcode
  for a `.gputrace` replay therefore reflect a real-GPU run, not a
  stored value, so each replay can in principle produce different
  numbers.
- Apple's "Measuring the GPU's use of memory bandwidth" guide
  (https://developer.apple.com/documentation/xcode/measuring-the-gpus-use-of-memory-bandwidth)
  notes that bandwidth counter values "show momentary values" and may
  vary; this is one mechanism through which N could be nonzero.

## Architecture

Existing pipeline:

```
.gputrace  →  Xcode (manual: Show Performance, Export Encoder Counters)
              →  counters-xcode.csv  (single-run snapshot)
                  ↓
              summarize_xcode_encoder_counters.py
                  →  joined CSV + bottleneck report
                  ↓
              compare_xcode_dxmt_bottlenecks.py
                  →  comparison report + gate verdict
```

Added pipeline (this design):

```
.gputrace (same bundle, reopened N=5 times by the operator)
              ↓  manual export per iteration
counters-xcode-run1.csv ... counters-xcode-run5.csv
              ↓
analyze_xcode_replay_variance.py
              ↓
replay-variance-report.md   replay-variance-summary.csv
(per-encoder per-metric mean / stddev / CV)
              ↓
operator inspects:
  - all non-target rows: max CV across key metrics
  - decision: reject N (proceed to R), accept N (pivot to M), or
    inconclusive (request N=10)
```

The new tool is data-in, data-out. No coupling to summarize or
compare. It reads raw Xcode counter CSVs directly because the
question being answered (does Xcode's GPU replay produce stable
counter values) is upstream of any dxmt join.

## Components

### C1. New script `scripts/tools/analyze_xcode_replay_variance.py`

Single-purpose CLI tool.

**Inputs:**

- Positional: `N >= 3` Xcode counter CSV paths (`counters-xcode-runK.csv`).
- `--output <path>` — Markdown report path (required).
- `--summary-output <path>` — optional reduced per-row variance CSV.
- `--metric <name>` — repeatable; defaults to a built-in set:
  `gpu_ms`, `vs_buffer_write_mib`, `vs_invocations`,
  `tiled_vertex_buffer_mib`, `tiled_primitive_block_mib`,
  `buffer_write_mib`. Source column names are the raw Xcode CSV
  columns; the script normalizes byte-valued columns to MiB to
  match other tooling.
- `--row-label-column <name>` — defaults to `Encoder Label`. Each
  matching label across the N CSVs is one row in the report.
- `--max-cv-pct <float>` — optional gate: exit nonzero if any
  (row, metric) pair has CV > limit (expressed as percent of mean).
  Default: gate disabled (informational mode).

**Outputs:**

- Markdown report: one section per row, table with metric / mean /
  stddev / CV%; a final summary table sorted by max CV.
- Optional summary CSV: one row per (encoder, metric), columns
  `encoder, metric, mean, stddev, cv_pct, n`.
- Exit code: `0` on success / informational mode, `1` if
  `--max-cv-pct` is set and any (row, metric) violates the limit.

**Numerical contract:**

- `mean` is arithmetic mean.
- `stddev` is sample standard deviation (`n-1` denominator).
- `cv_pct` is `100 * stddev / mean` when `mean > 0`; blank when
  `mean == 0` (do not produce NaN, do not substitute 0).
- Missing cells (column absent or empty string in some CSVs) reduce
  `n` for that (row, metric) pair but never substitute defaults.
- When `n < 2` for a (row, metric), CV is blank.

**Failure modes:**

- Fewer than 3 input CSVs → argparse error before reading any file.
- Mismatched row label sets across CSVs → report includes a "rows
  missing from one or more inputs" section and continues with the
  intersection. Each missing-data instance is logged but is not a
  hard error.
- Column not found in any CSV → metric reported as "not present" in
  report, not in the CSV. Not a hard error.

### C2. Tests `tests/scripts/test_analyze_xcode_replay_variance.py`

- Identical inputs (3 same CSVs) → all CV = 0, gate PASS.
- Synthetic distributions: 3 inputs with known values per metric, CV
  computed independently, equality within `1e-6`.
- Missing metric in one CSV → `n=2` for that (row, metric), report
  shows reduced sample, gate operates on whatever is present.
- Missing row label in one CSV → row excluded from variance table,
  appended to "rows missing from one or more inputs" section.
- `--max-cv-pct 5` PASS case: CVs all `<= 5%`.
- `--max-cv-pct 5` FAIL case: one metric at `7%`, exit code `1`,
  stderr names the violating (row, metric).

### C3. Rule update `agents/rules/metal_debugging.rules.md`

One short paragraph inside the existing "GPU performance counters"
section. Content:

- When investigating a counter delta that is small or close to
  observed Xcode replay variance, use
  `analyze_xcode_replay_variance.py` to verify the delta is larger
  than the noise floor before treating it as a real signal.
- Recommend N >= 5 replay exports; N = 3 is the minimum statistically
  meaningful sample.
- Link to the design doc.

### C4. No changes to other tools

- `summarize_xcode_encoder_counters.py` is untouched.
- `compare_xcode_dxmt_bottlenecks.py` is untouched. The existing
  gate semantics stay; the variance tool runs upstream, not as part
  of the gate.
- `finalize_3dmark05_perf_probe.sh` is untouched. The variance tool
  is invoked manually because it requires multiple manual Xcode
  exports anyway.

## Data flow

1. Operator confirms an existing `.gputrace` bundle is available
   (e.g., `traces/app-d3d9-3dmark05-cache-opt-apply-cached-rows1-3-gputrace-r1/frame50.gputrace`).
2. Operator opens the bundle in Xcode, runs Show Performance >
   Counters, waits for draw-counter profiling to complete, exports
   encoder counters to
   `traces/<run>/analysis/replay-variance/run1-counters-xcode.csv`,
   then closes the bundle.
3. Steps 2 are repeated for `run2-counters-xcode.csv` through
   `run5-counters-xcode.csv`. Closing-and-reopening the bundle between
   exports forces Xcode to re-replay the GPU work; without that the
   second export may reuse the first replay's cached counters.
4. Operator runs:

   ```bash
   python3 scripts/tools/analyze_xcode_replay_variance.py \
     traces/<run>/analysis/replay-variance/run1-counters-xcode.csv \
     traces/<run>/analysis/replay-variance/run2-counters-xcode.csv \
     traces/<run>/analysis/replay-variance/run3-counters-xcode.csv \
     traces/<run>/analysis/replay-variance/run4-counters-xcode.csv \
     traces/<run>/analysis/replay-variance/run5-counters-xcode.csv \
     --output traces/<run>/analysis/replay-variance-report.md \
     --summary-output traces/<run>/analysis/replay-variance-summary.csv \
     --max-cv-pct 5
   ```

5. Operator reads `replay-variance-report.md` and applies the
   decision rule in the Goal section.

## Decision rule (binding)

After the report is generated, the next investigation step is
determined by these rules in order:

1. If `--max-cv-pct 5` gate PASSES on the non-target rows
   (`50/0`, `50/2`, `50/4`, `50/11`) for `gpu_ms` and
   `vs_buffer_write_mib`: hypothesis N is rejected. Continue with
   hypothesis R (capture-to-capture variance) as the next probe.
2. If the same rows have `cv_pct > 10%` on either metric:
   hypothesis N is supported. The gputrace-based gate cannot
   reliably certify a `~20%` change of this magnitude. The next
   step is hypothesis M: define a production proof on no-gputrace
   `result.json` counters instead.
3. If CVs land between `5%` and `10%`: N is partially supported but
   not decisive. Re-run with N=10 (10 manual Xcode exports). Do
   not change probes or gates until N=10 is in hand.

These thresholds are conservative and intentionally avoid splitting
hairs between "drift" and "noise" at small absolute deltas.

## Verification gates (design self-acceptance)

| Gate | Pass condition |
|---|---|
| Unit-test coverage | All test cases in C2 pass; existing tests untouched. |
| CV numerical correctness | Independent hand-computed CV for a 3-sample synthetic case matches the tool output within `1e-6`. |
| Missing-data behavior | Missing column / row never substitutes a default; never produces NaN; reports `n` honestly. |
| Gate semantics | `--max-cv-pct` returns nonzero only when at least one (row, metric) violates; FAIL message names the violating pair. |
| No new GPU capture | The data flow uses only existing `.gputrace` bundles. |

## Anti-goals (out of scope)

- No `xcrun xctrace` programmatic counter extraction.
- No probe code, mini-replay, runtime, encoder, recorder, or PE/unix
  bridge change.
- No new env vars, no new perf counters.
- No change to the cached IB reorder mechanism itself.
- No change to existing gates.
- No follow-up hypothesis R/S/M implementation. They become
  candidates only after this design's results land.

## Sources

- Apple — GPU counters and counter sample buffers
  (https://developer.apple.com/documentation/metal/gpu-counters-and-counter-sample-buffers)
- Apple — Measuring the GPU's use of memory bandwidth
  (https://developer.apple.com/documentation/xcode/measuring-the-gpus-use-of-memory-bandwidth)
- TVB Pressure Mechanism Proof design (immediate predecessor):
  `docs/superpowers/specs/2026-06-03-tvb-mechanism-proof-design.md`
- Performance investigation plan (Path 1 owns the open problem):
  `specs/perfomance.plan.md` § Current-Head Xcode Recheck And
  Identity-Scout Drift
