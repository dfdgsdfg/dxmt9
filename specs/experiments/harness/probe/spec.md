---
type: "Spec"
title: "Harness Probe Spec — Capture Orchestration"
description: "Script inventory, artifact layout, the geometry .meta slice contract, owned environment variables, and mode table for the probe domain."
tags: [specs, experiments, harness, probe, spec]
---

# Harness Probe Spec — Capture Orchestration

Implements `specs/experiments/harness/probe/requirements.md`
(`R-HARN-PROBE-*`). Instantiates the `probe` row of the domain map in
`specs/experiments/harness/spec.md` §1 and the `run-capture →
dump-extract` and `dump-extract → offline-replay` boundaries in that
spec's §2. Stage names, boundary names, and envelope fields are cited
from the parent spec rather than redefined here. Facts below were
verified against `scripts/tools/run_3dmark05_perf_probe.sh` (6,279
lines) and `scripts/tools/run_with_wine_metal_capture_layer.sh` (180
lines) at their line numbers on 2026-07-27, and against
`traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/` (229 geometry
`.meta` files) and
`traces/app-d3d9-3dmark05-vertexremap-dump-r1/analysis/geometry/`
(cited by the parent spec, 156 files) as noted per section.

---

## 1. Script Inventory

| Script | Role |
|---|---|
| `scripts/tools/run_3dmark05_perf_probe.sh` | The domain's core. Runs preflights, wraps a supervised `run_experiment.py run app-d3d9-3dmark05` invocation in an outer watchdog, sets `DXMT9_*`/`DXMT_*` capture and diagnostic env for the subprocess, and invokes `reduce`-domain summarizers inline after the run. |
| `scripts/tools/run_with_wine_metal_capture_layer.sh` | Temporarily replaces a Wine root's `bin/wine.real` / `bin/wine-preloader` with `MetalCaptureEnabled` copies for a file `.gputrace` diagnostic, then restores the originals even on signal. Invoked by the core script when `--with-wine-capture-layer` is passed. |
| `scripts/tools/run_3dmark05_system_trace_sidecar.sh` | Wraps the core script's dry-run preflight, then records an `xctrace` Metal System Trace alongside a no-gputrace probe run and joins it against dxmt encoder attribution. Refuses a locked session before recording. |

`scripts/tools/finalize_3dmark05_perf_probe.sh` is **not** part of this
domain — the parent domain map assigns it to `join`
(`external-join` stage) — even though it updates the same
`3dmark05-trace-artifacts.json` sidecar this domain first writes (§2).

---

## 2. Artifact Directory Layout

Two roots are involved for one probe run, both keyed by
`run_id="app-d3d9-3dmark05-${suffix}"` (`run_3dmark05_perf_probe.sh:4104`):

- `experiments/output/<run-id>/` — the `runner`-domain output
  directory (`specs/experiments/harness/runner/spec.md` §2), into
  which this domain additionally writes its own summary CSV/MD files
  (`3dmark05-perf-summary.md`, `3dmark05-perf-encoders.csv`,
  `3dmark05-perf-encoder-streams.csv`,
  `3dmark05-perf-indexed-probe-draws.csv`,
  `3dmark05-index-cache-runtime-summary.{md,csv}`,
  `3dmark05-direct.log`) and the trace-artifacts manifest below.
- `traces/<run-id>/` — this domain's own trace root
  (`run_3dmark05_perf_probe.sh:4106-4107`, `trace_dir` /
  `analysis_dir="$trace_dir/analysis"`), holding the capture and dump
  artifacts this section describes.

| Path | Present when | Written by |
|---|---|---|
| `traces/<run-id>/frame<N>.gputrace` | gputrace capture on (default) and `--metal-capture-destination` is a file destination (`gpuTraceDocument`/`gputrace`/`file`, not `developerTools`/`xcode`) | dxmt9 runtime, via `DXMT_METAL_CAPTURE_FRAME`/`PATH` this domain sets (`run_3dmark05_perf_probe.sh:4175`: `capture_path="$trace_dir/frame${frame}.gputrace"`) |
| `traces/<run-id>/analysis/geometry/seq<S>-enc<E>-draw<D>-slot<K>.bin` + `.meta` | `--dump-indexed-geometry` | dxmt9 runtime via `DXMT9_DUMP_INDEXED_GEOMETRY_DIR` |
| `traces/<run-id>/analysis/shaders/msl/*.metal`, `analysis/shaders/bytecode/*` | `--dump-shaders` | dxmt9 runtime via `DXMT_DUMP_SHADER_DIR` / `_BYTECODE_DIR` (`run_3dmark05_perf_probe.sh:4112-4113,5079-5080`) |
| `traces/<run-id>/analysis/frame<N>-depth.bin` + `.bin.json` | `--dump-depth-attachment-handle` | dxmt9 runtime via `DXMT9_DUMP_DEPTH_ATTACHMENT_PATH` (`run_3dmark05_perf_probe.sh:4136-4138`) |
| `traces/<run-id>/analysis/frame<N>-color.bin` or `analysis/color-s<S>-e<E>-after-draw-d<N>.bin` | `--dump-color-attachment-*` | dxmt9 runtime via `DXMT9_DUMP_COLOR_ATTACHMENT_*` |
| `traces/<run-id>/analysis/textures/*` | `--dump-draw-texture-*` | dxmt9 runtime via `DXMT9_DUMP_DRAW_TEXTURE_DIR` (`run_3dmark05_perf_probe.sh:4164-4166`) |
| `traces/<run-id>/analysis/captures/*.bmp` | `--capture-frames`/`--capture-range` without `--capture-dir` | dxmt9 runtime via `DXMT_CAPTURE_FRAMES`/`_RANGE` + `DXMT_EXPERIMENT_CAPTURE_DIR` (`run_3dmark05_perf_probe.sh:4130`) |
| `traces/<run-id>/analysis/dag/*.{json,dot,mmd}` | `--dump-framegraph-dag` | dxmt9 runtime via `DXMT9_RENDERER_DUMP_DAG` (`run_3dmark05_perf_probe.sh:4115`: `framegraph_dag_dir="$analysis_dir/dag"`) |
| `experiments/output/<run-id>/3dmark05-trace-artifacts.json` | always, once `dump-extract` completes | this domain's own script writes the initial manifest (`run_3dmark05_perf_probe.sh:6114-6188`); the `join`-domain `finalize_3dmark05_perf_probe.sh` reads and rewrites the same file in place once Xcode exports exist (`finalize_3dmark05_perf_probe.sh:1680-1758`), refreshing its `paths`/`exists` fields without changing its schema |

Verified against
`traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/`, which
contains `frame60-depth.bin`, `frame60-depth.bin.json`, `geometry/`
(229 `.meta` files), and `shaders/{msl,bytecode}/`. That same
directory also contains `lanes/` and
`frame60-mini-replay-manifest-enc1.json` — those are **not** written
by this domain (neither name appears in
`run_3dmark05_perf_probe.sh`); they are `replay`-domain output written
into this domain's own trace directory by
`scripts/tools/build_3dmark05_mini_replay_manifest.py` /
`scripts/tools/run_3dmark05_mini_replay.py`, mirroring how the
`runner` domain's own spec lists per-app extras it does not write.

`3dmark05-trace-artifacts.json`'s initial write (this domain) already
includes `paths.xcode_performance_gputrace` /
`xcode_encoder_counters_csv` / `xcode_counters_summary_csv` /
`xcode_dxmt_joined_summary_csv` / `xcode_dxmt_bottleneck_report`
entries with `exists: false`, because those files do not exist until
the `join`-domain finalizer runs; only the later in-place rewrite can
mark them `true`. A consumer that reads this manifest before the
finalizer has run must not treat `exists: false` for those keys as a
missing-artifact error — it is the expected pre-`external-join` state.

---

## 3. Preflight Ordering and the Outer Watchdog

Per R-HARN-PROBE-2.1, this domain's core script evaluates the
following before spawning Wine (source order, all before the first
`mkdir -p "$output_dir" "$trace_dir"` at
`run_3dmark05_perf_probe.sh:5976`). Line numbers below were read
directly from the script on 2026-07-27 (re-verified after an earlier
draft of this section drifted by several lines); re-check them
against the live file before citing further, since any subsequent
edit to the script can move them again.

1. `--dry-run` early exit (prints preflight previews, `exit 0`;
   `run_3dmark05_perf_probe.sh:5886-5903`).
2. macOS session-lock wait/fail
   (`DXMT_3DMARK05_REQUIRE_UNLOCKED`, default on; wait loop
   `:5905-5922`, hard-exit check `:5924-5931`).
3. Low free-space gputrace guard
   (`DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB`; `:5933-5939`).
4. File `.gputrace` capture-layer preflight, direct
   (`run_file_capture_layer_preflight`, `:403-429`) or via
   `--with-wine-capture-layer`
   (`run_wine_capture_layer_wrapper_preflight`, `:431-453`); the
   call site selecting between them is `:5941-5959`.
5. `--require-xcode-attach-preflight` Xcode attach check, only for a
   `developerTools`/`xcode` capture destination (`:5961-5967`).
6. General free-space guard (`--min-free-mb` /
   `DXMT_3DMARK05_MIN_TRACE_FREE_MB`; `:5969-5974`).

Any failure of steps 2, 3, 5, or 6 above `exit 2`s; step 4's failure
also `exit 2`s with a diagnostic naming which of the direct or
wine-capture-layer variant failed and why (missing `wine.real`,
missing `MetalCaptureEnabled`, etc.).
`DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER=1` is the one documented
escape hatch (R-HARN-PROBE-2.1
compatibility exception per parent R-HARN-2.3): it disables step 4 for
a deliberate late-failure diagnostic where the expected evidence is
`Capture layer is not inserted` after launch rather than a preflight
exit.

**Outer watchdog.** The whole supervised invocation —
`caffeinate -dimsu python3 scripts/run_apps/run_experiment.py run
app-d3d9-3dmark05 --output-suffix "$suffix" --timeout "$timeout"`,
optionally wrapped in
`bash scripts/tools/run_with_wine_metal_capture_layer.sh --wine-root ... --allow-3dmark05 --`
(`run_3dmark05_perf_probe.sh:5084-5102`) — is opened in a
subshell at `:6010` (the `(` line; `:6009` is the unrelated preceding
`start_3dmark05_frontmost_loop` call) and launched under
`python3 scripts/tools/run_with_timeout.py --timeout
"$watchdog_base_sec" --slack "$timeout_slack" --label
3dmark05-perf-wrapper -- env "${env_args[@]}" "${cmd[@]}"` at
`:6012-6017`, where
`watchdog_base_sec = resolved --timeout + effective_capture_delay_sec`
(`:3197-3203`, `resolved --timeout` default `420` with gputrace /
`120` without, `--timeout` itself rejected unless `> 0` at
`:3153-3157`) and
`timeout_slack` defaults to `45`
(`DXMT_3DMARK05_PROBE_TIMEOUT_SLACK`). `run_with_timeout.py` itself
requires a positive `--timeout` and non-negative `--slack`
(`positive_float`/`non_negative_float` argparse types), starts the
child in a new process session (`start_new_session=True`), and on
expiry of `--timeout + --slack` sends the whole process group
`SIGTERM`, waits up to `--grace` (default `5`) seconds, then
`SIGKILL`s the group (`run_with_timeout.py:27-42,70-83`). This is the
outer layer R-HARN-PROBE-3.2 requires: it recovers even if the inner
`run_experiment.py --timeout` supervision (a `runner`-domain
contract, `specs/experiments/harness/runner/requirements.md`
R-HARN-RUN-3.1/3.2) fails to fully terminate a detached Wine child.

---

## 4. Wine Capture-Layer Restoration

`run_with_wine_metal_capture_layer.sh` (invoked by
`--with-wine-capture-layer`) replaces `<wine-root>/bin/wine.real` and
`bin/wine-preloader` with `MetalCaptureEnabled` copies
(`wine.capture.real` / `wine.capture.real-preloader` by default),
after verifying both copies actually contain `MetalCaptureEnabled`
(`:121-125`) and refusing a 3DMark05 command line unless
`--allow-3dmark05` is also given (`:109-114`; this domain's core
script always passes `--allow-3dmark05` at `:5098` because a probe run
is the deliberate diagnostic use this guard exists to gate).

The file-replacement primitive
(`replace_with_file`, `:34-49`) copies into a `mktemp` file in the
**same directory** as the destination (`tmp=$(mktemp
"$dir/.${base}.replace.XXXXXX")`) and publishes it with `mv -f "$tmp"
"$dst"` — never an in-place `cp` onto the live file
(R-HARN-PROBE-4.2). Restoration is registered via `trap 'status=$?;
restore; exit "$status"' EXIT INT TERM` (`:150`) before the capture
copies are installed, so a signal during the wrapped command still
restores the originals; `restore()` is idempotent (a `restored` guard
at `:141-147`) and is called again unconditionally after a normal
return before the trap is cleared (`:159-162`). After restoration, the
script re-reads both restored files and the pre-replacement backup
copies and asserts byte-for-byte equality before printing `restored`
and exiting (`:164-178`) — this is the R-HARN-PROBE-4.1 validity
assertion; a `restore verification failed` exit means the trap fired
but did not actually recover the original bytes, and this domain
treats that as a hard failure (`fail`, `exit 2`), not a warning.

---

## 5. Geometry `.meta` Field Contract

This is the boundary parent spec.md calls out as having failed
silently (its §2 `dump-extract → offline-replay` section, R-HARN-4.3/
4.4, defect 2). This section restates the same rule from the `probe`
side, verified independently against
`traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/geometry/seq60-enc1-draw31825-slot140.meta`
(229-file directory, 2026-07-27) in addition to the parent's own
`.../vertexremap-dump-r1/analysis/geometry/` citation (156 files,
including the 8 non-zero-offset rows).

The `.meta` sidecar is a flat `key=value` text file. Fields relevant
to the slice rule, taken verbatim from the verified file above:

```
stream0_offset=0
stream0_stride=24
stream0_start_byte=0
stream0_byte_count=63120
```

**The slice rule, stated exactly:**

- `stream0_start_byte` is the **slice origin** — the byte offset,
  in the source D3D9 stream0 vertex buffer's own coordinate system, at
  which this draw's `.bin` payload begins. Payload byte 0 is fetch
  slot 0 of the draw.
- `stream0_offset` is the **D3D9 stream offset in the source buffer**
  — the stream binding's own byte offset into the app's original
  vertex buffer, expressed in that same source coordinate system. It
  is not, and must never be read as, an offset already relative to the
  sliced `.bin` payload.
- A consumer computes the in-payload offset for fetch slot `N` as
  `(stream0_offset - stream0_start_byte) + N * stream0_stride`,
  **never** `stream0_offset` directly. Using `stream0_offset` as a
  payload-relative base double-counts the slice origin.
- **Not a guaranteed invariant.** In the verified file above (and in
  every one of the 229 `.meta` files in this domain's own
  `vertexremap-enc1-r1` directory), `stream0_start_byte ==
  stream0_offset` because the current dump producer slices starting
  exactly at the stream binding's own offset. The parent spec's
  `vertexremap-dump-r1` citation independently confirms this equality
  holds even for its 8 non-zero-offset rows (`840`, `3384`, `92760`,
  `185760`, `186696`, `272760`, `280320`, `289728`). This is a
  **producer coincidence of the current slicing choice**, not a
  contract this or any consumer may assume without reading
  `stream0_start_byte` — a future producer that slices from a
  different origin (e.g. a fixed page boundary) would break the
  equality while leaving the `offset - start_byte` derivation above
  still correct. This is the exact distinction R-HARN-PROBE-5.3
  states as a requirement: subtract, do not assume equality.

The remaining `.meta` fields (`stream0_stride`, `stream0_byte_count`,
`index_*`, `wrote_*`, `vertex_decl_*`, `texture<N>_*`,
`attachment_*`, `vsconsts_byte_count`/`psconsts_byte_count`/
`ffpvs_byte_count`/`ffpps_byte_count`) describe draw shape, bound
resources, and optional cbuf-dump byte counts; they are not part of
the slice-origin ambiguity this section exists to close and are named
here only for completeness of what a `replay`-domain consumer will
find in the same file — the load-bearing contract is the four fields
above.

---

## 6. Environment Variables This Domain Sets

| Var | Purpose |
|---|---|
| `DXMT_EXPERIMENT_PROFILE` | `perf` (`run_3dmark05_perf_probe.sh:4198`) — selects the `runner`-domain launcher's perf-profile validation/logging/offload defaults. |
| `DXMT_EXPERIMENT_CAPTURE_DIR` | Forwarded only when `--capture-dir`/`--capture-frames`/`--capture-range` is used (`:5074`) — directory for `DXMT_CAPTURE_FRAMES`/`_RANGE` internal backbuffer dumps; read directly by the dxmt9 runtime (`src/d3d9/core.cpp:159`). |
| `DXMT_3DMARK05_PREFIX` | Resolved 3DMark05 Wine prefix (`:4200`). |
| `DXMT_3DMARK05_WINE_ROOT` | Resolved Wine root for the direct-launcher path (`:4201`). |
| `DXMT_3DMARK05_WINESERVER` | Resolved `wineserver` binary path (`:4202`). |
| `DXMT_3DMARK05_RESULT_FILE` | `--result-file` value, default `dxmt9_gt1.3dr` (`:4204`). |
| `DXMT_3DMARK05_LOG` | `$output_dir/3dmark05-direct.log` (`:4205`). |
| `DXMT_3DMARK05_DIRECT` | `1` (`:4199`) — **dual-owner deviation** (R-HARN-PROBE-6.3): also set by the `runner`-domain `scripts/run_apps/run_app-d3d9-3dmark05-verify_direct.sh:15`, on a mutually exclusive invocation path. Neither domain owns this variable alone. |
| `DXMT_DISABLE_AUTO_EXPAND_INDEXED` | `1` (`:4203`) — compiled-in default with **no wrapper flag**, unconditionally set in the same `env_args` initializer as the seven variables above and the `DXMT9_OFFLOAD_COMMIT_REPLAY` pair (R-HARN-PROBE-6.4's "compiled-in default the script itself pins for recipe determinism" class). Unlike `DXMT9_OFFLOAD_COMMIT_REPLAY=${DXMT9_OFFLOAD_COMMIT_REPLAY:-1}` two lines below it, this assignment has no `${...:-}` fallback, so a caller's pre-set environment value cannot override it. Per `agents/rules/environment_variables_encoder.rules.md`, the variable "disable[s] compatibility heuristic that auto-expands selected indexed draws" and its engine default is `0`; this domain pins it to `1` for every run it launches, so every 3DMark05 perf number this probe produces is measured with that heuristic off against an engine default of on, a fact no other document previously recorded. |
| `DXMT_CAPTURE_FRAMES` / `DXMT_CAPTURE_RANGE` | `--capture-frames`/`--capture-range` values, when given. |
| `DXMT_METAL_CAPTURE_FRAME` / `_PATH` / `_DESTINATION` | Gputrace capture target frame, `$capture_path`, and `--metal-capture-destination`, when gputrace capture is on. |
| every other flag-forwarded `DXMT9_*`/`DXMT_*` variable in §7 | Forwarded verbatim from the matching `--flag`; see §7 for the mapping. |

`DXMT_3DMARK05_WINE_ROOT` and `DXMT_3DMARK05_WINESERVER` are not
documented in any `agents/rules/environment_variables_*.rules.md`
file today (verified: `rg` over `agents/rules/` finds neither name).
They exist only in `scripts/tools/run_3dmark05_perf_probe.sh` and are
read with inline bash defaults
(`${DXMT_3DMARK05_WINE_ROOT:-...}`,
`${DXMT_3DMARK05_WINESERVER:-"$wine_root/bin/wineserver"}`) in
`experiments/launchers/app-d3d9-3dmark05.sh:182,184`.
`DXMT_3DMARK05_PREFIX`, `_RESULT_FILE`, and `_LOG` are documented in
`agents/rules/environment_variables_perf.rules.md`;
`DXMT_3DMARK05_DIRECT` is documented in
`agents/rules/environment_variables_wine.rules.md`;
`DXMT_DISABLE_AUTO_EXPAND_INDEXED` is documented in
`agents/rules/environment_variables_encoder.rules.md`.

`DXMT_EXPERIMENT_WINE_DLLOVERRIDES` and other `DXMT_EXPERIMENT_*`
variables not listed above remain owned by the `runner` domain per
that domain's own `spec.md` §4 — this domain reads, but does not set,
`run_experiment.py`'s standard CLI surface (`--output-suffix`,
`--timeout`) rather than assigning those values as environment
variables itself.

---

## 7. Mode Table

`run_3dmark05_perf_probe.sh --help` enumerates 350 `--`-prefixed
option lines as of 2026-07-27 (`grep -cE '^  --' <<< "$(bash
scripts/tools/run_3dmark05_perf_probe.sh --help)"`). Per
R-HARN-PROBE-7.1, every one of them alters this domain's output —
either the captured/dumped artifacts, the forwarded runtime
environment, a locally-invoked summarizer/comparator's own arguments
(§7.2b), or only the printed `finalize_cmd_after_xcode_export`/
`xctrace_system_trace_summary_cmd` suggestion text this domain emits
for a human to run afterward (§7.2). Two of the four sub-buckets below
are exact, grep-reproducible counts; the other two are not re-totaled
here because §7.1 groups several flags per table row (e.g. "18
sub-flags" in one row) and §7.3 covers a long tail of per-variable
diagnostic families documented elsewhere — reporting an unverified sum
across all four is exactly the kind of unreproduced arithmetic claim
this section was previously wrong about, so it is not repeated. §7.1
tables the run-shape/preflight flags individually; §7.2 is an exact
count of 104 finalizer-gate-passthrough flags (command and output
below); §7.2b names 13 flags verified individually to set no
environment variable at all; §7.3 states the mechanical naming rule
that accounts for the remaining diagnostic-probe flags without
re-deriving each row's semantics, which already live in
`agents/rules/environment_variables_encoder.rules.md`,
`_capture.rules.md`, `_renderer.rules.md`, `_present.rules.md`, and
`_perf.rules.md`.

### 7.1 Run-shape and preflight flags

| Flag | Effect on output |
|---|---|
| `--suffix` | Sets `<run-id>` (`app-d3d9-3dmark05-<suffix>`), i.e. both output roots in §2. |
| `--frame` | 1-based capture frame (default `60`); selects `frame<N>.gputrace`/`frame<N>-depth.bin` naming and the encoder-breakdown auto-scope. |
| `--timeout` | Inner `run_experiment.py --timeout`; also the base for the outer watchdog (§3). Rejected unless `> 0`. |
| `--no-gputrace` | Disables `DXMT_METAL_CAPTURE_FRAME`/`PATH`; changes the default `--timeout` (420 → 120) and free-space guard (2048 → 256 MiB). |
| `--metal-capture-destination` / `--xcode-developer-tools-capture` | Selects `gpuTraceDocument`/`gputrace`/`file` (direct `frame<N>.gputrace`) vs. `developerTools`/`xcode` (no direct file; Xcode-attach path). |
| `--with-wine-capture-layer` | Routes the launch through `run_with_wine_metal_capture_layer.sh` (§4); changes which Wine root files are live during the run. |
| `--require-xcode-attach-preflight` / `--xcode-attach-preflight-only` | Adds/isolates the Xcode attach preflight (§3 step 5). |
| `--wait-unlocked-sec` / `--wait-unlocked-interval-sec` | Polls macOS lock state before the session-lock preflight fails. |
| `--keep-frontmost[-interval-sec/-process]` | Periodically refocuses the 3DMark05 process during the run; changes scene-progress/FPS fidelity, not artifact shape. |
| `--capture-delay-sec` | Overrides the on-screen capture delay `run_experiment.py` uses; also feeds the outer watchdog's `watchdog_base_sec` (§3). |
| `--capture-frames` / `--capture-range` / `--capture-dir` | Sets `DXMT_CAPTURE_FRAMES`/`_RANGE`/`DXMT_EXPERIMENT_CAPTURE_DIR`; writes `analysis/captures/*.bmp`. |
| `--result-file` | `DXMT_3DMARK05_RESULT_FILE`. |
| `--min-free-mb` | Free-space preflight threshold (§3 steps 3/6); gputrace runs below the recommended minimum need `DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1`. |
| `--dry-run` | Preflight preview only; `exit 0` before any `mkdir`/launch (§3, R-HARN-PROBE-2.2). |
| `--encoder-breakdown-seq[-range]` / `--encoder-breakdown-all-frames` / `--no-encoder-breakdown` | `DXMT9_PERF_ENCODER_BREAKDOWN[_SEQ[_MIN/MAX]]`; scopes or disables per-encoder log detail. |
| `--pe-recorder-stats` / `--pe-recorder-chunk-log` / `--pe-draw-full-snapshot` / `--pe-chunk-max-records` / `--pe-chunk-max-bytes` | `DXMT9_PE_RECORDER_STATS`/`_CHUNK_LOG`/`_DRAW_FULL_SNAPSHOT`/`_CHUNK_MAX_RECORDS`/`_CHUNK_MAX_BYTES`. |
| `--dxmt-log-level` | `DXMT_LOG_LEVEL` for the wrapped run. |
| `--dump-shaders` | `DXMT_DUMP_SHADER_DIR`/`_BYTECODE_DIR`; writes `analysis/shaders/{msl,bytecode}/`. |
| `--dump-indexed-geometry[-cbufs/-max-draws/-vs/-ps/-texture0*]` | `DXMT9_DUMP_INDEXED_GEOMETRY_*`; writes `analysis/geometry/*.{bin,meta}` (§5). |
| `--dump-depth-attachment-{handle,seq,enc,path}` | `DXMT9_DUMP_DEPTH_ATTACHMENT_*`; writes `analysis/frame<N>-depth.bin` (+ `.bin.json`). |
| `--dump-color-attachment-*` (18 sub-flags) | `DXMT9_DUMP_COLOR_ATTACHMENT_*`; writes `analysis/frame<N>-color.bin` or `analysis/color-s<S>-e<E>-after-draw-d<N>.bin`. |
| `--dump-draw-texture*` (7 sub-flags) | `DXMT9_DUMP_DRAW_TEXTURE_*`; writes `analysis/textures/*`. |
| `--dump-framegraph-dag` / `--framegraph-dag-*` (6 sub-flags) | `DXMT9_RENDERER_DUMP_DAG*`; writes `analysis/dag/*.{json,dot,mmd}`. |
| `--frame-sampling` | `DXMT9_PERF_FRAME_SAMPLING=1`; per-Present `wall_ms`/fps in the log, consumed by `reduce`-domain summarizers, not by this domain. |
| `--present-boundary-deferred` / `--draw-chunk-command-limit` | `DXMT9_PRESENT_BOUNDARY_DEFERRED`/`DXMT9_DRAW_CHUNK_COMMAND_LIMIT`. |
| `--probe-draw-packet-actual-change` / `--probe-vs-const-setter-range` | `DXMT9_PERF_DRAW_PACKET_ACTUAL_CHANGE`/`_VS_CONST_SETTER_RANGE`. |
| `--render-pass-reentry-top` | `DXMT9_PERF_RENDER_PASS_REENTRY_TOP`. |
| `--measure-index-reuse` / `--measure-index-cache-opt-candidate` | `DXMT9_MEASURE_INDEX_REUSE`/`_CACHE_OPT_CANDIDATE`. |
| `--allow-3dmark05` (via `--with-wine-capture-layer` wrapper) | Bypasses the wrapper script's own 3DMark05-command refusal (§4); this domain's core script always passes it. |

### 7.2 Finalizer-passthrough flags (printed suggestion only)

The `--require-*`, `--max-*`, `--min-*` (excluding `--min-free-mb`,
tabled in §7.1), `--target-row-key`,
`--allow-partial-stable-frame-proof`, `--compare-baseline-output`, and
`--baseline-joined` flags do not change what this domain captures or
dumps. **Exact count, reproduced:**

```
$ bash scripts/tools/run_3dmark05_perf_probe.sh --help > /tmp/probe_help.txt
$ grep -cE "^  --require-" /tmp/probe_help.txt
84
$ grep -cE "^  --max-" /tmp/probe_help.txt
14
$ grep -E "^  --min-" /tmp/probe_help.txt | grep -vc "min-free-mb"
2
$ grep -cE "^  --target-row-key" /tmp/probe_help.txt
1
$ grep -cE "^  --allow-partial-stable-frame-proof" /tmp/probe_help.txt
1
$ grep -cE "^  --compare-baseline-output" /tmp/probe_help.txt
1
$ grep -cE "^  --baseline-joined" /tmp/probe_help.txt
1
```

`84 + 14 + 2 + 1 + 1 + 1 + 1 = 104`. (An earlier draft of this section
claimed "~280," which was wrong by roughly 2.7x — it was never
actually counted against this prefix set; the number above is.)

These 104 flags are accepted so this domain's core script can carry
them through into the `finalize_cmd_after_xcode_export` /
`xctrace_system_trace_summary_cmd` command lines it prints after the
run, for a human to paste into the `join`-domain
`finalize_3dmark05_perf_probe.sh` invocation once Xcode counters are
exported (per `agents/rules/metal_debugging.rules.md` §9's documented
recipe). Their gate semantics — what "top-N GPU decrease" or
"opaque-depth-index-cache-proof" means when actually evaluated — are
the `join`/`gate` domains' own mode-table responsibility, not this
domain's; this domain's contract for them is limited to "accepted and
echoed into the printed command unchanged," **except**
`--compare-baseline-output`, which additionally triggers this
domain's own local invocation of the `gate`-domain
`compare_3dmark05_perf_counters.py` against the given baseline
directory and this run's `output_dir`, writing
`$counter_comparison_path` (`run_3dmark05_perf_probe.sh:5104-5108`
builds `counter_compare_cmd`; `:6175-6180` executes it) —
so for that one flag, "accepted and echoed unchanged" is not the whole
contract; it also causes a real local side effect. It is also echoed
into `finalize_cmd` under a translated name, `--baseline-output`
(`:5305-5306`), which is `finalize_3dmark05_perf_probe.sh`'s own flag
spelling — this domain's own accepted flag is
`--compare-baseline-output`, not `--baseline-output`.

### 7.2b Flags that set no environment variable

The following flags do not set any environment variable in
`env_args` — they are consumed entirely by this domain's own script,
either shaping the printed `finalize_cmd`/`xctrace_summary_cmd`
suggestion text or feeding a locally-invoked summarizer's own CLI
arguments. This bucket was missing from an earlier draft of this
section, which incorrectly asserted (§7.3) that no exception to the
mechanical env-var rule existed; the 13 flags below are that
exception. The check that matters is narrower than "does this
variable's name appear anywhere in the script" — several of these
flags' shell variables *do* appear elsewhere, as the default-value
source for the wrapper's own `--flag` default:
`${DXMT_3DMARK05_SEMANTIC_IMAGE_POLICY:-}` and six siblings
(`:246-252`), `${DXMT_3DMARK05_VISIBILITY_SCOUT_SUMMARY_LIMIT:-12}`
(`:243`), `${DXMT_3DMARK05_TOP_N:-3}` (`:365`), and
`${DXMT_3DMARK05_HOT_GPU_SHARE:-95.0}` (`:366`). Each of those is a
read that seeds this wrapper's own flag default from the caller's
environment — a distinct mechanism from forwarding a value onward
into the launched Wine subprocess — so a plain name-search wrongly
counts it as an "appears in the script" hit. The actual question is
whether any `env_args+=(...)` line in the script forwards the
variable into the launched subprocess's environment:

```
$ grep -n "env_args" scripts/tools/run_3dmark05_perf_probe.sh | \
    grep -iE "semantic_image|top_n|hot_gpu_share|visibility_scout_draw_indices|visibility_scout_summary"
$ echo "exit=$?"
exit=1
```

No output, exit status 1 (grep found no matching line). As a sanity
check that this pipeline is actually exercising real content rather
than failing on a broken first-stage pattern, `grep -c "env_args"
scripts/tools/run_3dmark05_perf_probe.sh` returns `219` — the first
stage does match plenty of lines; none of them, filtered by the
second stage, ever forward one of these 13 flags' variables.

| Flag | Variable | Where it actually goes |
|---|---|---|
| `--semantic-image-policy` | `semantic_image_policy` | Printed into `finalize_cmd` as `--semantic-image-policy` (`:5313`). |
| `--semantic-image-before` | `semantic_image_before` | Printed into `finalize_cmd` (`:5314`). |
| `--semantic-image-after` | `semantic_image_after` | Printed into `finalize_cmd` (`:5315`). |
| `--semantic-image-output` | `semantic_image_output` | Printed into `finalize_cmd`, when set (`:5318-5319`). |
| `--semantic-image-summary-output` | `semantic_image_summary_output` | Printed into `finalize_cmd`, when set (`:5321-5322`). |
| `--semantic-image-diff-output` | `semantic_image_diff_output` | Printed into `finalize_cmd`, when set (`:5324-5325`). |
| `--semantic-image-min-active-pct` | `semantic_image_min_active_pct` | Printed into `finalize_cmd` (`:5316`). |
| `--top` | `top_n` | Printed into `xctrace_summary_cmd`/`finalize_cmd`'s `--top` arguments (`:5302,5685`); never executed by this domain itself (only `printf`'d, `:5824`). |
| `--hot-gpu-share` | `hot_gpu_share` | Printed into `finalize_cmd`'s `--hot-gpu-share` argument (`:5303`). |
| `--visibility-scout-draw-indices` | `visibility_scout_draw_indices` | Passed as `--draw-indices` to the locally-invoked `scripts/tools/summarize_visibility_scout.py` (`:6051-6052`). |
| `--visibility-scout-summary-output` | `visibility_scout_summary_output` | Passed as `--output` to the same locally-invoked summarizer (`:6043`). |
| `--visibility-scout-summary-csv-output` | `visibility_scout_summary_csv_output` | Passed as `--csv-output` to the same summarizer (`:6044`). |
| `--visibility-scout-summary-limit` | `visibility_scout_summary_limit` | Passed as `--limit`/`--no-sample-limit` to the same summarizer (`:6045-6046`). |

Note the base `--visibility-scout`, `--visibility-scout-row`,
`--visibility-scout-rows`, and `--visibility-scout-path` flags are
**not** in this bucket — they do set `DXMT9_VISIBILITY_SCOUT`,
`_ROW`, `_ROWS`, and `_PATH` respectively
(`run_3dmark05_perf_probe.sh:5051-5062`) and are covered by §7.3's
mechanical rule instead. Only the four `--visibility-scout-*` flags
whose whole purpose is to shape the locally-invoked summarizer's own
output are in this "no env var" bucket.

### 7.3 Mechanical flag-to-variable mapping for the remaining probe/diagnostic flags

Every flag not listed in §7.1, §7.2, or §7.2b sets exactly one
environment variable via a direct, spellable rule: `--some-flag-name`
sets `DXMT9_SOME_FLAG_NAME` (hyphens to underscores, uppercased),
except for the older `DXMT_DEBUG_*`/`DXMT_*` (no `9`) family, where
the flag spelling omits the `DEBUG`/`9` segment the variable carries
— for example `--force-cull-mode` sets `DXMT_DEBUG_FORCE_CULL_MODE`,
`--disable-cull` sets `DXMT_DISABLE_CULL`, `--force-texture-white`
sets `DXMT_FORCE_TEXTURE_WHITE`, and `--force-visible` sets
`DXMT_DEBUG_FORCE_VISIBLE` (verified at
`run_3dmark05_perf_probe.sh:2269-2287,2526,1824`). This covers the
`--probe-*`, `--split-large-indexed-draws*`, `--optimize-*`,
`--index-cache-candidate-*`, `--trim-*`, and row/class/texture-scoped
sub-flag families documented per-variable in
`agents/rules/environment_variables_encoder.rules.md`.

This rule's scope is now explicitly bounded: §7.2b is the complete set
of exceptions found by the check described there, run on 2026-07-27.
An earlier draft of this section asserted "none found" without having
run that check — that assertion was false, and the 13 flags in §7.2b
are what it missed. A future change to this domain's flag surface must
re-run an equivalent check (grep the candidate flag's assigned
variable name and its derived `DXMT9_*`/`DXMT_*` spelling for any
`env_args` occurrence) before repeating a "no exceptions" claim;
R-HARN-PROBE-7.1 still requires any newly discovered exception to be
named in §7.2b, not silently absorbed into this section's rule.

Diagnostic flags in this category print a `warning: --flag-name is
diagnostic only; ...` line describing the specific correctness risk
(e.g. `--disable-alpha-test`, `--force-texture-white`,
`--probe-reverse-indexed-triangles`) once the run proceeds past
preflight (`run_3dmark05_perf_probe.sh:5827-5911`); this is the
in-band signal required by parent R-HARN-6.1/6.2 that a diagnostic
mode is not being mistaken for the primary path.
