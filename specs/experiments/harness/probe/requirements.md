---
type: "Spec Requirements"
title: "Harness Probe Requirements — Capture Orchestration"
description: "Requirements for the probe domain: run-capture launch orchestration and dump-extract artifact production for wild 3DMark05 GT1 experiments."
tags: [specs, experiments, harness, probe, requirements]
---

# Harness Probe Requirements — Capture Orchestration

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `probe` domain
named in `specs/experiments/harness/spec.md` §1. The probe domain
participates in the `run-capture` and `dump-extract` pipeline stages
(parent spec.md §0): it launches a supervised Wine/3DMark05 process on
top of a `runner`-domain run, applies its own preflights and
watchdog, and extracts `.gputrace`, geometry, shader, and depth/color
sidecar artifacts that downstream `replay`, `reduce`, `join`, and
`gate` domains consume. Requirement IDs in this file use the prefix
`R-HARN-PROBE-`.

---

## 1. Scope and Stage Participation

**R-HARN-PROBE-1.1** The probe domain comprises exactly the scripts
the parent domain map (`specs/experiments/harness/spec.md` §1)
assigns to it: `scripts/tools/run_3dmark05_perf_probe.sh`,
`scripts/tools/run_with_wine_metal_capture_layer.sh`, and
`scripts/tools/run_3dmark05_system_trace_sidecar.sh`. This domain's
own `spec.md` names only the `run-capture` and `dump-extract`
pipeline stages (parent spec.md §0) as stages it participates in; it
must not describe `offline-replay`, `log-reduce`, `external-join`, or
any later stage as this domain's own responsibility, even though its
core script invokes `summarize_3dmark05_perf.py`-family output inline
and prints a suggested `finalize_3dmark05_perf_probe.sh` command line
for the `join` domain to run afterward. Instantiates R-HARN-1.1.

**R-HARN-PROBE-1.2** `scripts/tools/run_3dmark05_perf_probe.sh`
invokes `python3 scripts/run_apps/run_experiment.py run
app-d3d9-3dmark05 --output-suffix "$suffix" --timeout "$timeout"` as a
subprocess of its own `run-capture` work; this domain therefore sits
strictly on top of a `runner`-domain run rather than replacing it. A
`probe`-domain change must not reimplement `run_experiment.py`'s own
`build-stage`/`run-capture` responsibilities (staging, prefix
bootstrap, `result.json` writing) inside this domain's scripts.
Instantiates R-HARN-1.1 (parent spec.md §1, "Why the domain axis is
harness families, not stages").

---

## 2. Preflights Fail Before Launching Wine

**R-HARN-PROBE-2.1** Before this domain's core script spawns the
supervised `caffeinate ... run_experiment.py run app-d3d9-3dmark05
...` subprocess, it evaluates, in order, every precondition whose
failure would make the demanded capture invalid: macOS session-lock
state (after any configured `--wait-unlocked-sec` poll), the gputrace
free-space guard, the file `.gputrace` capture-layer preflight (direct
or via `--with-wine-capture-layer`), the
`--require-xcode-attach-preflight` Xcode attach check when a
`developerTools` capture destination is selected, and the general
free-space guard. Any one of
these failing exits non-zero (`exit 2`) with a diagnostic naming the
failed precondition, and no `result.json`, `.gputrace`, or geometry
artifact for the run is produced. Instantiates R-HARN-2.1 (no valid
artifact was written and no launch begins) and R-HARN-3.1 (the
capture's non-degeneracy is knowable in advance for these conditions,
so it is asserted before launch rather than deferred to a
post-hoc check of a degenerate artifact — the defect 4 rationale
R-HARN-3.1 exists to prevent).

**R-HARN-PROBE-2.2** `--dry-run` performs the same free-space and
file-capture-layer preflight evaluations as R-HARN-PROBE-2.1 (printed
as `dry-run: ...` diagnostics) and exits zero without spawning Wine.
A `--dry-run` invocation that instead silently skips a preflight it
would otherwise run is a violation of R-HARN-6.1: a diagnostic mode is
bound by the same validity discipline as the primary path, and here
that means "preview what the primary path's preflights would decide,"
not "skip evaluating them."

**R-HARN-PROBE-2.3** A gputrace-capturing invocation whose resolved
free-space guard falls under the recommended gputrace minimum
(`DXMT_3DMARK05_MIN_TRACE_FREE_MB` default, distinct from the smaller
no-gputrace default) exits non-zero unless
`DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1` is explicitly set; the
opt-out is a named escape hatch, not a default fallback, satisfying
R-HARN-2.3.

---

## 3. Positive Timeout and Outer Watchdog Are Mandatory

**R-HARN-PROBE-3.1** This domain's core script rejects a resolved
`--timeout` value that is not a positive number, independent of and in
addition to the `runner`-domain's own `require_positive_timeout`
catalogue-level enforcement
(`specs/experiments/harness/runner/requirements.md`
R-HARN-RUN-3.1/3.2). A `probe`-domain change must not
remove this check on the theory that the `runner` domain already
enforces it, because this domain's own watchdog (R-HARN-PROBE-3.2)
depends on a positive `--timeout` to compute a positive watchdog
duration. Instantiates R-HARN-2.1 (rationale: 3DMark05 can hang on its
final frame after emitting all useful capture data; an unbounded wait
never produces a finalized artifact).

**R-HARN-PROBE-3.2** This domain wraps its entire supervised
subprocess — the `runner`-domain `run_experiment.py` invocation and
any `--with-wine-capture-layer` wrapper around it — in a second, outer
watchdog whose timeout is strictly greater than the inner
`run_experiment.py --timeout`: `effective watchdog seconds = resolved
--timeout + effective capture-delay seconds + timeout slack`. This
outer watchdog terminates the whole process group (`SIGTERM` then
`SIGKILL` after a grace period) if the inner, already-supervised
invocation fails to exit on its own. A `probe`-domain change must not
collapse this to a single timeout layer, because the outer watchdog's
entire purpose is to recover from the case where the inner runner's
own termination path does not fully kill the Wine process tree.
Instantiates R-HARN-2.1.

**R-HARN-PROBE-3.3** The outer watchdog's own timeout and slack
arguments are validated as positive/non-negative before use; an
invalid `DXMT_3DMARK05_PROBE_TIMEOUT_SLACK` fails the invocation with
a diagnostic naming the variable rather than silently coercing it to a
default. Instantiates R-HARN-2.2.

---

## 4. A Mutated External Runtime Is Always Restored

**R-HARN-PROBE-4.1** `--with-wine-capture-layer` (via
`scripts/tools/run_with_wine_metal_capture_layer.sh`) temporarily
replaces a Wine root's `bin/wine.real` and `bin/wine-preloader` with
capture-enabled copies. This domain restores both files to their
original bytes when the wrapped command exits — normally, non-zero, or
on `INT`/`TERM` — before the wrapper script itself exits, and verifies
the restored files are byte-identical to the pre-replacement backup
before reporting success. A change to this script must not replace
the restore-and-verify step with a best-effort restore that can leave
a mutated Wine root behind after a signal. Instantiates R-HARN-2.1 (a
signal-interrupted run must not silently leave the shared external
Wine runtime in an altered state that the next run treats as
"restored") and R-HARN-3.1 (byte-identity with the backup is the
validity assertion for the restore itself).

**R-HARN-PROBE-4.2** The file-replacement primitive used by
R-HARN-PROBE-4.1 writes its temporary copy into the same directory as
the file being replaced and publishes it with an atomic rename, never
an in-place overwrite of the live file. Rationale:
`agents/rules/metal_debugging.rules.md` records that an in-place `cp`
onto `wine.real` reproduced `SIGKILL (Code Signature Invalid)` /
"Taskgated Invalid Signature" even when `codesign --verify` passed
afterward — the live binary was observably corrupt during the copy
window. Instantiates R-HARN-2.1 (an in-place overwrite that Wine
launches mid-write is a wrong-shape artifact, not a valid restored
runtime).

---

## 5. Geometry Dump Coordinate System Is Declared, Not Inferred

**R-HARN-PROBE-5.1** Every geometry payload this domain writes under
`analysis/geometry/*.bin` carries a sidecar `.meta` file whose
`stream0_start_byte` field names the byte offset, in the *source*
D3D9 stream0 vertex buffer's coordinate system, at which the `.bin`
payload's byte 0 begins; the payload contains exactly
`stream0_byte_count` bytes starting at that source offset.
Instantiates R-HARN-4.1/4.2 (parent spec.md's `dump-extract →
offline-replay` boundary): a consumer must not need an assumed
convention to know which coordinate system `stream0_start_byte`
belongs to.

**R-HARN-PROBE-5.2** The same `.meta` sidecar's `stream0_offset` field
is the D3D9 stream binding's own byte offset into the app's original
vertex buffer — a field of the *source* coordinate system, not a
payload-relative offset — and this domain must not, now or in a future
change, repurpose `stream0_offset` to mean an offset already relative
to the sliced `.bin` payload. Instantiates R-HARN-4.2 (a numeric field
without one fixed, named coordinate system is not a complete
interpretation rule).

**R-HARN-PROBE-5.3** This domain must not assume, and must not cause a
downstream consumer to assume, that `stream0_start_byte ==
stream0_offset` holds for every future dump. Today's slicing choice
(slice from the stream binding's own offset) makes the two fields
numerically equal in every currently captured `.meta` file, but a
future producer change that slices from a different origin (e.g. a
fixed page boundary) is not itself a violation of this requirement as
long as `stream0_start_byte` continues to name the true slice origin
per R-HARN-PROBE-5.1. Instantiates R-HARN-4.3/4.4 (defect 2's
rationale: the coincidence, not the declared field, is what a past
consumer wrongly relied on).

---

## 6. Environment Variables Forwarded to the Runtime Appear in `env_snapshot`

**R-HARN-PROBE-6.1** This domain is the sole setter, for a run it
launches, of `DXMT_EXPERIMENT_PROFILE` and `DXMT_EXPERIMENT_CAPTURE_DIR`
among the `DXMT_EXPERIMENT_*`-prefixed variables — both carry that
prefix but are excluded from the `runner` domain's own single-setter
list per that domain's R-HARN-RUN-5.3. No `runner`, `replay`,
`reduce`, `join`, `gate`, or `audit` domain script may set either
variable for a run this domain launches. Instantiates R-HARN-1.3.

**R-HARN-PROBE-6.2** This domain is the sole setter, for a run it
launches, of `DXMT_3DMARK05_PREFIX`, `DXMT_3DMARK05_WINE_ROOT`,
`DXMT_3DMARK05_WINESERVER`, `DXMT_3DMARK05_RESULT_FILE`, and
`DXMT_3DMARK05_LOG`. Instantiates R-HARN-1.3.

**R-HARN-PROBE-6.3** `DXMT_3DMARK05_DIRECT` is a genuine dual-owner
deviation from parent spec.md §4 Rule 1, not a documentation gap: this
domain's core script sets it (alongside the five variables in
R-HARN-PROBE-6.2) on the probe-driven invocation path, and the
`runner`-domain `scripts/run_apps/run_app-d3d9-3dmark05-verify_direct.sh`
independently sets it on the direct-verify invocation path. The two
setters are mutually exclusive within any one run — a probe-driven run
never goes through the verify-direct wrapper and vice versa — but
neither domain may claim sole ownership of this one variable. A future
change must not resolve this by having one domain silently stop
setting it without updating both this file and the `runner` domain's
own R-HARN-RUN-5.3-adjacent documentation of the same deviation.
Instantiates R-HARN-1.3.

**R-HARN-PROBE-6.4** Every contract-relevant `DXMT9_*`/`DXMT_*`
variable this domain's core script resolves and forwards into the
supervised subprocess's environment — whether from an explicit
`--flag`, from a `DXMT_3DMARK05_*`-prefixed override, or from a
compiled-in default the script itself pins for recipe determinism
(e.g. its pinned `DXMT9_OFFLOAD_COMMIT_REPLAY` /
`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE` values) — is a candidate for
that run's `env_snapshot` (parent spec.md §3) once the envelope is
adopted for this domain's artifacts; a resolved value must not be
tracked only as "a flag was passed" without recording what value was
actually forwarded. Instantiates R-HARN-1.3 (spec.md §4 Rule 2) and
R-HARN-2.4.

---

## 7. Diagnostic Paths Carry the Primary Contract

**R-HARN-PROBE-7.1** Every flag accepted by this domain's core script
that alters its output — the captured artifacts, the forwarded runtime
environment, or the printed finalize-command suggestion — appears in
this domain's own `spec.md` mode table, whether the flag is a routine
capture-shape control (`--frame`, `--no-gputrace`,
`--metal-capture-destination`) or a diagnostic bisection probe
(`--probe-*`, `--split-*`, `--optimize-*`). Instantiates R-HARN-6.2.

**R-HARN-PROBE-7.2** A diagnostic flag that this domain forwards as an
environment variable into the supervised subprocess is bound by the
same no-silent-degradation discipline as any primary-path flag: this
domain must not special-case a diagnostic flag to swallow an
unrecognized value (a bad cull-mode string, a malformed row selector)
without a diagnostic naming the unrecognized value. Instantiates
R-HARN-6.1 and R-HARN-2.2.

**R-HARN-PROBE-7.3** This domain's own diagnostic flags are a distinct
code path from any `replay`-domain flag of the same spelling. In
particular, this domain's `--force-fragment-color` sets
`DXMT_DEBUG_FORCE_FRAGMENT_COLOR` for the live supervised Wine/3DMark05
run; it is not the same flag as `run_3dmark05_mini_replay.py`'s
own `--force-fragment-color`, whose compile failure is the `replay`
domain's own R-HARN-6.3 defect (parent requirements.md rationale for
defect 5). A reader must not attribute one domain's diagnostic-flag
defect to the other domain's flag of the same name. Instantiates
R-HARN-6.1 (scope discipline for what this domain's own mode table
claims).
