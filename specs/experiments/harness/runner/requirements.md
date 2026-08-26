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
`require_positive_timeout = true` — currently `app-d3d9-3dmark05` and
`app-d3d9-3dmark06` — a `run_experiment.py run <app> --timeout <value>`
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

**R-HARN-RUN-5.3** Not every `DXMT_EXPERIMENT_*`-prefixed variable is
owned by this domain: `DXMT_EXPERIMENT_PROFILE` and
`DXMT_EXPERIMENT_CAPTURE_DIR` carry the prefix but are set only by the
`probe` domain (`scripts/tools/run_3dmark05_perf_probe.sh`, which
assigns both directly before invoking `run_experiment.py`) and never
by `run_experiment.py` itself, whose subprocess environment starts
from `os.environ.copy()` and therefore forwards whatever value the
caller already set, unchanged. This domain's own
`experiments/launchers/common.sh` reads `DXMT_EXPERIMENT_PROFILE` to
select validation/logging/offload defaults but does not set it, and
the dxmt9 runtime reads `DXMT_EXPERIMENT_CAPTURE_DIR` directly without
this domain ever inspecting it. R-HARN-RUN-5.1's single-setter claim
governs only the 16 variables it lists; it must not be read as a
claim that this domain owns the whole `DXMT_EXPERIMENT_*` namespace.
The `probe` domain's own `requirements.md` is where ownership of
`DXMT_EXPERIMENT_PROFILE` and `DXMT_EXPERIMENT_CAPTURE_DIR` belongs.
Instantiates R-HARN-1.3.

---

## 6. Canonical 3DMark Lanes

**R-HARN-RUN-6.1** The 3DMark05 and 3DMark06 launchers must resolve named
benchmark lanes through the shared
`experiments/launchers/3dmark_lane_presets.sh` table. Each product defaults to
the bounded `gt1` lane. A named lane must produce a fixed test-selection stream
followed by `-nosplash -nosysteminfo -noscreens`; an unknown name must fail
before Wine is launched.

**R-HARN-RUN-6.2** `DXMT_3DMARK05_ARGS` and `DXMT_3DMARK06_ARGS` remain
complete raw argument overrides for compatibility and experimental selections.
When a raw override is non-empty it must take precedence over the corresponding
`DXMT_3DMARK05_LANE` or `DXMT_3DMARK06_LANE`, and the resolved lane identity
must be `custom` with source `args`. The launcher must not append the standard
headless arguments to a raw override.

**R-HARN-RUN-6.3** Every launched 3DMark preset or custom stream must emit one
machine-readable `[3dmark-lane]` identity containing product, resolved lane,
and selection source. `run_experiment.py` must preserve that identity as
`result.json:benchmark_lane`. A non-3DMark experiment must not gain a
`benchmark_lane` field.

**R-HARN-RUN-6.4** Scene lanes (`gt*`, `hdr*`) may be used as graphics
performance and correctness workloads. CPU lanes are diagnostic workloads for
application/Wine/Rosetta/PE-bridge scheduling and must not be treated as direct
renderer throughput scores. `feature`, `batch`, and `all` are microbenchmark or
coverage suites, not replacements for the scene-lane promotion gates.

**R-HARN-RUN-6.5** A catalogue 3DMark05 or 3DMark06 run must request one
run-unique `.3dr` basename when the caller did not set the product-specific
`DXMT_3DMARK*_RESULT_FILE`. Before spawning the launcher, the runner must
snapshot regular `.3dr` files under the benchmark working directory, the Wine
prefix user tree, and the resolved requested-file parent. After the child has
settled or been terminated, it must consider only newly created or
content/metadata-modified regular files. Pre-existing unchanged files and
symlinks must not be attributed to the run.

**R-HARN-RUN-6.6** Every discovered result file must be copied through a
temporary file and atomically published under
`experiments/output/<app-runid>/benchmark-results/`. `result.json` must record
the requested environment/value/mode/resolved source, search roots, capture
status, requested-file absence, and each file's source, artifact-relative path,
change class, byte count, source modification time, and SHA-256 digest. A
missing `.3dr` is an observed `not_emitted` disposition rather than a renderer
failure because some qualified editions or manually selected lanes do not emit
one; a discovered file that cannot be copied is an artifact-capture failure.
