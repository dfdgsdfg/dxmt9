---
type: "Spec Requirements"
title: "Harness Runner Requirements — Catalogue Runs"
description: "Requirements for the runner domain: build-stage staging and run-capture execution of catalogued dxmt9 experiments."
tags: [specs, experiments, harness, runner, requirements]
---

# Harness Runner Requirements — Catalogue Runs

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `runner` domain
named in `specs/experiments/harness/spec.md` §1. The runner domain is
the producer side of the `build-stage → run-capture` and
`run-capture → dump-extract` boundaries defined in that spec's §2:
every downstream domain's evidence begins with a run this domain
launched. Requirement IDs in this file use the prefix `R-HARN-RUN-`.

---

## 1. Scope and Stage Participation

**R-HARN-RUN-1.1** The runner domain comprises exactly the scripts the
parent domain map (`specs/experiments/harness/spec.md` §1) assigns to
it: `scripts/run_apps/run_experiment.py`, the catalogue launchers
under `experiments/launchers/`, `scripts/run_apps/*.sh`, and
`scripts/run_suites/*`. This domain's own `spec.md` names only the
`build-stage` and `run-capture` pipeline stages (parent spec.md §0) as
stages it participates in; it must not describe `dump-extract` or any
later stage as this domain's own responsibility. Instantiates
R-HARN-1.1.

**R-HARN-RUN-1.2** A `run_experiment.py run <app>` invocation performs
`build-stage` work — resolving the Wine manifest entry, bootstrapping
or reusing the prefix, and staging dxmt9 via `stage_dxmt9()` — before
`run-capture` work begins, unless the app's catalogue entry sets
`skip_stage = true` or the caller passes `--skip-stage`. In the
skip-stage case, `run-capture` still runs, so this domain always
participates in `run-capture` and only conditionally in `build-stage`.
Instantiates R-HARN-1.1 (parent spec.md §2, `build-stage →
run-capture` boundary).

---

## 2. Output Directory and Result Artifact

**R-HARN-RUN-2.1** For any `run_experiment.py run <app>` invocation
that passes the launcher-exists and binary-exists precondition
checks, this domain creates `experiments/output/<app-runid>/` — where
`<app-runid>` is `<app.name>` or `<app.name>-<output-suffix>` when
`--output-suffix` is given — before performing any Wine-root
resolution or dxmt9 staging. The directory therefore exists even when
a later `build-stage` step fails. Instantiates R-HARN-2.1.

**R-HARN-RUN-2.2** Whenever this domain reaches `run-capture` — i.e.
the catalogue launcher subprocess has been spawned — it writes
`result.json` into that run's output directory before the invocation
exits, regardless of whether the subprocess times out, exits
non-zero, or fails a downstream validity check (black-screen,
missing-capture, SSIM, counter-range). A downstream `probe` or
`dump-extract` consumer reads `result.json`'s presence, not the
process exit code alone, to distinguish "ran and failed" from "never
reached run-capture." Instantiates R-HARN-2.1.

**R-HARN-RUN-2.3** A `build-stage` failure that prevents `run-capture`
from ever starting must not produce a `result.json` whose `status`
field claims `pass` or `fail` for that invocation; the absence of
`result.json` is how a downstream consumer distinguishes "never
reached run-capture" from "reached run-capture and failed"
(R-HARN-RUN-2.2). Instantiates R-HARN-2.2.

---

## 3. Positive Timeout for Apps That Can Hang

**R-HARN-RUN-3.1** For a catalogue app whose `[[app]]` entry sets
`require_positive_timeout = true` — `app-d3d9-3dmark05` is the only
such entry today — a `run_experiment.py run <app> --timeout <value>`
invocation with `value <= 0` exits non-zero before any Wine process is
launched, with a diagnostic naming the app and the violated
constraint. Instantiates R-HARN-2.1 (an unbounded hang that never
produces an artifact is the degenerate case this rule prevents).

**R-HARN-RUN-3.2** `require_positive_timeout = true` combined with a
catalogue `run_timeout_sec <= 0` is rejected at catalogue-load time,
not only at CLI-invocation time, so the positive-timeout invariant
holds for the catalogue default value as well as for any `--timeout`
override; a catalogue entry cannot silently opt an app out of the
invariant by fixing only the CLI-argument case. Instantiates
R-HARN-2.1.

---

## 4. Counter Payload and Envelope Are Separate Artifacts

**R-HARN-RUN-4.1** `result.json`'s counter payload —
`dxmt9_perf_counters`, `dxmt9_bridge_counters`,
`dxmt9_pe_recorder_counters`, and `perf_probe_timings` — is a distinct
concern from the artifact envelope fields (`schema`, `producer`,
`stage`, `domain`, `inputs`, `env_snapshot`, `validity`) defined in
`specs/experiments/harness/spec.md` §3. When the envelope is adopted
for `result.json` per that spec's §5 migration, this domain must add
the envelope as a wrapper or sibling field set alongside the existing
counter payload, not merge envelope semantics into counter keys or
counter semantics into envelope fields. Instantiates R-HARN-3.2.

**R-HARN-RUN-4.2** The `expected_counters` L3 gate
(`evaluate_counter_ranges` in `run_experiment.py`) evaluates only
`result.json`'s counter payload against the per-app
`[apps.<name>.expected_counters]` bounds from
`experiments/CATALOGUE.toml`; it must not read, nor be redirected by,
an envelope field such as `validity`, so that adopting the envelope in
a later migration step cannot silently change what this gate already
evaluates. Instantiates R-HARN-3.2.

---

## 5. Environment Variable Ownership

**R-HARN-RUN-5.1** Within one `run_experiment.py run <app>`
invocation, this domain is the sole setter of every
`DXMT_EXPERIMENT_*` variable it assigns into the launched subprocess's
environment: `DXMT_EXPERIMENT_NAME`, `_BINARY`, `_PREFIX`,
`_WINE_ROOT`, `_WINE_BIN`, `_PE_BUILD_DIR`, `_RUNTIME_PE_BUILD_DIR`,
`_WOW64_PE_BUILD_DIR`, `_WOW64_RUNTIME_PE_BUILD_DIR`,
`_UNIX_BUILD_DIR`, `_OUTPUT_DIR`, `_LOG`, `_CAPTURE_PATH`,
`_SKIP_STAGE`, `_WINE_DLLOVERRIDES`, and `_CX_BOTTLE`. No `probe`,
`replay`, `reduce`, `join`, `gate`, or `audit` domain script may set
any of these variables for a run this domain launched; a downstream
script may only read the resolved values this domain forwarded
(directly, from its own process environment, or via `result.json`).
Instantiates R-HARN-1.3.

**R-HARN-RUN-5.2** `DXMT_CAPTURE_FRAME`, which this domain also sets
into the launched subprocess's environment from the catalogue's
`capture_frame` key, follows the same single-setter rule as
R-HARN-RUN-5.1 for a run this domain launched. Instantiates
R-HARN-1.3.
