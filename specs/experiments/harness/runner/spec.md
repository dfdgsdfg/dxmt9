---
type: "Spec"
title: "Harness Runner Spec — Catalogue Runs"
description: "Script inventory, artifact layout, owned environment variables, and mode table for the runner domain."
tags: [specs, experiments, harness, runner, spec]
---

# Harness Runner Spec — Catalogue Runs

Implements `specs/experiments/harness/runner/requirements.md`
(`R-HARN-RUN-*`). Instantiates the `runner` row of the domain map in
`specs/experiments/harness/spec.md` §1 and the `build-stage →
run-capture` and `run-capture → dump-extract` boundaries in that
spec's §2. Stage names, boundary names, and envelope fields are cited
from the parent spec rather than redefined here.

---

## 1. Script Inventory

| Script | Role |
|---|---|
| `scripts/run_apps/run_experiment.py` | The domain's core. Loads `experiments/CATALOGUE.toml`, resolves the Wine manifest entry and prefix, optionally runs `stage_dxmt9()` (`build-stage`), spawns the catalogue launcher subprocess and captures the on-screen frame (`run-capture`), and writes `result.json`. |
| `scripts/run_apps/run_app-d3d9-3dmark05-verify_direct.sh` | Direct (non-catalogue-supervised) wrapper around `experiments/launchers/app-d3d9-3dmark05.sh`, for standalone debugging outside `run_experiment.py`'s supervision. Applies its own `DXMT_3DMARK05_DIRECT_TIMEOUT` (default `120`) watchdog and kills the process group on timeout. |
| `experiments/launchers/*.sh` | One launcher per catalogue app (e.g. `app-d3d9-3dmark05.sh`, `conf-d3d9-fast-sanity.sh`, `perf-d3d9-present-loop.sh`), plus shared `common.sh`. These are the processes `run_experiment.py` spawns for `run-capture`; they start Wine and the target binary. |
| `scripts/run_suites/*.sh` | Batch drivers invoking `run_experiment.py` (or the catalogue launchers) over a fixed app list: `run_dx9_fast_sanity_suite.sh`, `run_dx9_regression_suite.sh`, `run_dx9_performance_suite.sh`, `run_dx9_builtin_oracle_suite.sh`, `run_dx9_oracle_compare_suite.sh`, `run_d3d9_conformance_render_modes.sh`, `run_boundary_audit_suite.sh`. |

`build-stage`'s actual dxmt9-into-Wine install step is delegated to
`scripts/install/install_heroic_wine.sh`, invoked by `stage_dxmt9()`.
That script lives outside `scripts/tools/`, `scripts/run_apps/`,
`scripts/check/`, and `scripts/run_suites/`, so it is outside the
86-script inventory `specs/experiments/harness/requirements.md`
R-HARN-1.1 scopes this spec family to; it is named here only because
`build-stage`'s contract depends on it running successfully.

---

## 2. Artifact Directory Layout

Layout root: `experiments/output/<app-runid>/`, where `<app-runid>`
is `<app.name>` (no suffix) or `<app.name>-<output-suffix>` when
`--output-suffix` is passed — `run_experiment.py`'s `output_name`.

This domain writes:

| File | Present when | Content |
|---|---|---|
| `result.json` | `run-capture` was reached (R-HARN-RUN-2.2) | Full run result: `status`, `returncode`, `timed_out`, `performance`, the counter payload, `image_metrics`, `failures`, `wine`, `wsi`. |
| `dxmt9.log` | same | The launcher subprocess's captured combined stdout+stderr. |
| `actual.png` | a screen or internal capture succeeded | On-screen (or internal-dump-derived) frame image. |
| `actual.bmp` | the `DXMT_EXPERIMENT_CAPTURE_PATH` internal dump fired | Raw internal backbuffer dump, source for `actual.png` when present. |
| `reference.png` | `app.reference_path` exists | Symlink to the catalogue reference image. |
| `diff.png` | both `actual.png` and a reference exist | Heat-diff image from `write_diff_image`. |
| `ssim.txt` | both exist and SSIM was computed | SSIM score, 6 decimal places. |

Verified against
`experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/`, which
contains `result.json`, `dxmt9.log`, and `actual.png` from this
domain, plus per-app extras this domain does **not** write:
`3dmark05-direct.log`, `3DMark05_dxmt9.log`,
`3dmark05-perf-summary.md`, `3dmark05-perf-encoders.csv`,
`3dmark05-perf-frames.csv`, `3dmark05-perf-encoder-streams.csv`,
`3dmark05-perf-indexed-probe-draws.csv`,
`3dmark05-perf-render-pass-reentry.csv`,
`3dmark05-perf-argbuf-payload-delta-sources.csv`,
`3dmark05-perf-vs-const-setter-ranges.csv`,
`3dmark05-index-cache-runtime-summary.md`,
`3dmark05-index-cache-runtime-summary.csv`, and
`3dmark05-trace-artifacts.json`.

Those extras are written by the `probe`-domain
`scripts/tools/run_3dmark05_perf_probe.sh`, which computes
`output_dir="$repo_root/experiments/output/$run_id"` with
`run_id="app-d3d9-3dmark05-${suffix}"` independently — matching this
domain's own `<app-runid>` naming convention rather than being told
the path in band — and separately invokes
`python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
--output-suffix "$suffix"` so both scripts' outputs land in the same
directory. `3DMark05_dxmt9.log` specifically is the dxmt9 runtime's
own default-named log file: the probe wrapper points `DXMT_LOG_PATH`
at the output directory (a `probe`-domain env choice; see
`agents/rules/environment_variables_logging.rules.md`), which is a
different file from this domain's own `dxmt9.log` (the captured
launcher-subprocess stdout+stderr).

This directory-naming convention is therefore itself part of this
domain's boundary contract: the `<app.name>[-<output-suffix>]` scheme
under `experiments/output/` is not carried in any artifact field —
a `probe`-domain script that wants to add files alongside this
domain's own artifacts must reconstruct the same naming convention.
Per parent spec.md's `build-stage → run-capture` boundary note, the
coordinate system here is "which directory", not a byte offset;
R-HARN-4.2 does not apply, but the convention itself is load-bearing
and undocumented anywhere except this file and the two scripts' source.

---

## 3. Failure Behavior: Build-Stage vs. Run-Capture

`run_experiment.py`'s `main()` catches only `FileNotFoundError` around
`run_experiment()`; every other exception — including
`subprocess.CalledProcessError` raised by `stage_dxmt9()` when the
underlying `install_heroic_wine.sh` exits non-zero, or a
`ManifestError` from Wine-id resolution — propagates unhandled (a
`ManifestError` is caught locally and turned into `sys.exit(2)`, but
still before any `result.json` write). In every one of these
`build-stage`-time failure paths, `experiments/output/<app-runid>/`
already exists (R-HARN-RUN-2.1: it is created before Wine-root
resolution or staging begins), but no `result.json` is written — the
directory alone carries no information distinguishing "build-stage
crashed" from "this run has not started yet." Per R-HARN-RUN-2.3, this
is at least not a *false* claim: the absence of `result.json` is how a
downstream reader tells this case apart from a completed run. A tool
that treats "the output directory exists" as evidence a run happened
would be wrong to do so; only `result.json`'s presence is that
evidence (R-HARN-RUN-2.2).

Failures that occur after the launcher subprocess is spawned —
non-zero exit, timeout, black-screen, missing-capture, SSIM below
threshold, or a counter-range violation — are all recorded as entries
in `result.json`'s `failures` array with `status: "fail"`, and
`run_experiment()` returns `1` (not raising), so `main()` returns that
code normally.

---

## 4. Environment Variables This Domain Sets

| Var | Purpose |
|---|---|
| `DXMT_EXPERIMENT_NAME` | The catalogue app name, for launcher/log self-identification. |
| `DXMT_EXPERIMENT_BINARY` | Resolved binary path (or Wine drive-letter path) to launch. |
| `DXMT_EXPERIMENT_PREFIX` | Wine prefix directory for this run. |
| `DXMT_EXPERIMENT_WINE_ROOT` | Resolved Wine root directory (manifest-resolved or legacy-detected). |
| `DXMT_EXPERIMENT_WINE_BIN` | Resolved `wine`/`wine64` executable path. |
| `DXMT_EXPERIMENT_PE_BUILD_DIR` | Staged 64-bit PE build dir containing `d3d9.dll`. |
| `DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR` | Staged 64-bit builtin runtime `winemetal_dxmt9.dll` build dir. |
| `DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR` | Staged 32-bit PE build dir containing `d3d9.dll`. |
| `DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR` | Staged 32-bit builtin runtime `winemetal_dxmt9.dll` build dir. |
| `DXMT_EXPERIMENT_UNIX_BUILD_DIR` | Unix build dir containing `winemetal_dxmt9.so`. |
| `DXMT_EXPERIMENT_OUTPUT_DIR` | This run's `experiments/output/<app-runid>/` path. |
| `DXMT_EXPERIMENT_LOG` | Path to this domain's own `dxmt9.log` (the captured launcher stdout+stderr file). |
| `DXMT_EXPERIMENT_CAPTURE_PATH` | Path the launcher should pass through for dxmt9's internal backbuffer dump (`actual.bmp`). |
| `DXMT_EXPERIMENT_SKIP_STAGE` | `"1"` when `--skip-stage`/catalogue `skip_stage` suppressed the `build-stage` install step. |
| `DXMT_EXPERIMENT_WINE_DLLOVERRIDES` | Forwarded verbatim from the catalogue's `wine_dll_overrides` key, when set. |
| `DXMT_EXPERIMENT_CX_BOTTLE` | Forwarded verbatim from the catalogue's `cx_bottle` key, when set. |
| `DXMT_CAPTURE_FRAME` | The catalogue's `capture_frame` key — the internal backbuffer capture frame number. |

`DXMT_EXPERIMENT_WINE_ID` is a variable this domain **reads**, not
sets: `resolve_wine_id()` consults `os.environ.get("DXMT_EXPERIMENT_WINE_ID")`
as one input (CLI `--wine-id` > this env var > catalogue `wine_id`)
when resolving which manifest entry to use. It is not written into the
launched subprocess's environment, so it falls outside the
single-setter rule of R-HARN-RUN-5.1, which governs only variables
this domain assigns *into* the run it launches.

`DXMT_EXPERIMENT_PROFILE` and `DXMT_EXPERIMENT_CAPTURE_DIR` also carry
the `DXMT_EXPERIMENT_*` prefix but are **not** set by this domain
(R-HARN-RUN-5.3): `scripts/tools/run_3dmark05_perf_probe.sh` (the
`probe` domain) assigns both directly — `DXMT_EXPERIMENT_PROFILE=perf`
at line 4198 and `DXMT_EXPERIMENT_CAPTURE_DIR=$capture_dir` at line
5074 — before invoking `run_experiment.py`. Because
`run_experiment.py`'s subprocess environment starts from
`os.environ.copy()` and its `env.update()` dict (§4 above) never
touches either key, both values pass through this domain unchanged
into the launched subprocess. `experiments/launchers/common.sh` (a
script this domain owns) then reads `DXMT_EXPERIMENT_PROFILE` to
select `DXMT_VALIDATE`/`DXMT_LOG_LEVEL`/`DXMT9_OFFLOAD_COMMIT_REPLAY`
defaults, and the dxmt9 runtime itself reads
`DXMT_EXPERIMENT_CAPTURE_DIR` directly (`src/d3d9/core.cpp`) — neither
is ever assigned by a `runner`-domain script. Ownership of both
variables belongs to the `probe` domain; this domain's contract is
limited to reading (`DXMT_EXPERIMENT_PROFILE`, for its own launcher
defaults) or purely forwarding (`DXMT_EXPERIMENT_CAPTURE_DIR`) them.

`experiments/launchers/app-d3d9-3dmark05.sh` additionally reads a
`DXMT_3DMARK05_*` variable family that selects its standalone
direct-invocation mode, independent of `run_experiment.py`'s own
supervision. Unlike the table in §4, this family does **not** have a
single owning domain today, and the deviation is precise enough to
name per variable rather than paper over:

- `DXMT_3DMARK05_PREFIX`, `_WINE_ROOT`, `_WINESERVER`,
  `_RESULT_FILE`, and `_LOG` are set only by the `probe`-domain
  `scripts/tools/run_3dmark05_perf_probe.sh` (lines 4200-4205);
  `scripts/run_apps/run_app-d3d9-3dmark05-verify_direct.sh` (this
  domain's own direct wrapper) does not set any of these five.
  Ownership belongs to `probe`, not to this domain.
- `DXMT_3DMARK05_DIRECT` is set by **both** domains, on different
  invocation paths: this domain's own
  `run_app-d3d9-3dmark05-verify_direct.sh:15` (`export
  DXMT_3DMARK05_DIRECT=1`) and the `probe`-domain
  `run_3dmark05_perf_probe.sh:4199` (`"DXMT_3DMARK05_DIRECT=1"` in its
  `env_args`). The two setters are mutually exclusive within any one
  run — a probe-driven run never goes through the verify-direct
  wrapper and vice versa — but the variable itself does not have one
  fixed owning domain the way `specs/experiments/harness/spec.md` §4
  Rule 1 expects. This is a genuine deviation from that rule, not a
  documentation gap: both call sites are named here so a future
  `specs/experiments/harness/gap.md` entry can cite them without
  re-deriving the finding.
- `DXMT_3DMARK05_DIRECT_TIMEOUT` is set only by this domain's
  `run_app-d3d9-3dmark05-verify_direct.sh:16`; no `probe`-domain
  script sets it. Ownership is genuinely this domain's.
- `DXMT_3DMARK05_KILL_SERVER_ON_EXIT`, `_ALLOW_UNSUPERVISED`, and
  `_REQUIRE_UNLOCKED` are read by `app-d3d9-3dmark05.sh` with inline
  bash defaults (`${VAR:-default}`) but are not assigned by any
  harness script in either domain; there is no ownership conflict
  because there is no in-repo setter to conflict.

Catalogue pointer, corrected per variable: `DXMT_3DMARK05_DIRECT` and
`DXMT_3DMARK05_LOG` are documented in
`agents/rules/environment_variables_wine.rules.md`;
`DXMT_3DMARK05_DIRECT_TIMEOUT`, `_PREFIX`, `_RESULT_FILE`,
`_KILL_SERVER_ON_EXIT`, `_ALLOW_UNSUPERVISED`, `_REQUIRE_UNLOCKED`,
and `_LAUNCHER_TIMEOUT` are in
`agents/rules/environment_variables_perf.rules.md`.
`DXMT_3DMARK05_WINE_ROOT` and `DXMT_3DMARK05_WINESERVER` are not
documented in any `agents/rules/environment_variables_*.rules.md`
file today — they exist only in
`scripts/tools/run_3dmark05_perf_probe.sh` and
`experiments/launchers/app-d3d9-3dmark05.sh` source.

---

## 5. Mode Table

### 5.1 `run_experiment.py run` CLI flags that alter output

| Flag | Effect on output |
|---|---|
| `--wine-root` | Overrides the resolved Wine root; changes which Wine build stages/runs the app. |
| `--wine-bin` | Overrides the resolved `wine`/`wine64` binary directly. |
| `--wine-id` | Selects a `experiments/wine/manifest.toml` entry, overriding catalogue `wine_id` and `DXMT_EXPERIMENT_WINE_ID`. |
| `--wine-manifest` | Overrides the manifest file path used for `--wine-id`/catalogue `wine_id` resolution. |
| `--rebuild-prefix` | Deletes and re-bootstraps `experiments/prefixs/<name>/` before the run. |
| `--allow-non-vanilla` | Suppresses the non-vanilla-Wine warning line this domain would otherwise write to `dxmt9.log`. |
| `--prefix` | Overrides the Wine prefix directory used. |
| `--binary` | Overrides the binary path launched, bypassing the catalogue-blocker check for ignored/generated binaries. |
| `--timeout` | Overrides `run_timeout_sec`; changes whether/when the run is treated as timed out. Rejected when `<= 0` and the app sets `require_positive_timeout` (R-HARN-RUN-3.1). |
| `--pe-build-dir` / `--runtime-pe-build-dir` / `--wow64-pe-build-dir` / `--wow64-runtime-pe-build-dir` / `--unix-build-dir` | Override which staged build directories `stage_dxmt9()` installs from. |
| `--skip-stage` | Skips the `build-stage` install step entirely. |
| `--stage-mingw-runtime` | With `--skip-stage`, still copies `libc++`/`libunwind` into the prefix. |
| `--mingw-bin-dir` / `--wow64-mingw-bin-dir` | Override the source directories for `--stage-mingw-runtime`. |
| `--accept-reference` | Creates the catalogue reference image from this run's `actual.png` if none exists yet, and sets `result["reference_created"]`. |
| `--keep-temp-prefix` | Keeps an auto-created temp prefix after the run instead of deleting it. |
| `--output-suffix` | Changes `<app-runid>`, i.e. the output directory name itself. |
| `--capture-delay-sec` | Overrides how long the domain waits before capturing the on-screen frame, changing which animation frame `actual.png` shows. |
| `--build` | Runs the catalogue `build_script` before launching, changing the binary under test. |

### 5.2 Catalogue per-app keys that alter output

| Key | Effect on output |
|---|---|
| `render_mode` | Forwarded as `DXMT9_RENDER_MODE` for the launched process; selects the traditional vs. framegraph backend. |
| `capture_delay_sec` | Default wait before on-screen capture (see `--capture-delay-sec`). |
| `capture_frame` | Forwarded as `DXMT_CAPTURE_FRAME`; selects the internal backbuffer capture frame. |
| `run_timeout_sec` | Default `--timeout` value. |
| `allow_timeout` | When true, a `timed_out` run is not also recorded as a `process_exit` failure. |
| `require_positive_timeout` | Rejects a non-positive effective timeout at both catalogue-load and CLI-invocation time (R-HARN-RUN-3.1/3.2). |
| `reference_optional` | When true, a missing catalogue reference image is not recorded as a `missing_reference` failure. |
| `skip_stage` | Catalogue-level default for `--skip-stage`. |
| `wine_dll_overrides` | Forwarded as `DXMT_EXPERIMENT_WINE_DLLOVERRIDES`. |
| `cx_bottle` | Forwarded as `DXMT_EXPERIMENT_CX_BOTTLE`. |
| `wine_id` / `wine_alternatives` | Select the manifest entry; a resolved non-vanilla variant not listed in `wine_alternatives` adds a warning line to `dxmt9.log` unless `--allow-non-vanilla` is also given. |
| `install_drive_letter` | Changes how a drive-letter `binary` path is translated to a local filesystem path. |
| `expected_counters` | Per-counter `{min, max}` bounds; a violation adds a `counter_out_of_range` failure entry (the `expected_counters` L3 gate). |
| `build_script` | The script `--build` runs before launching. |
