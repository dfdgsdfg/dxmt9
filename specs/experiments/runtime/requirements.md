---
type: "Spec Requirements"
title: "Experiments Runtime Requirements — Wine, Prefix, External Apps"
description: "Experiments / Runtime requirements and compatibility contracts."
tags: [specs, experiments, runtime, requirements]
---

# Experiments Runtime Requirements — Wine, Prefix, External Apps

This spec governs how dxmt9 manages the **non-source artifacts** that wild
experiments need to run: the Wine runtime binary, the per-experiment Wine
prefix, and the externally-installed app binaries (e.g. SFIV, 3DMark05,
3DMark06).

It is a sibling to `specs/experiments/requirements.md` (which covers what an
experiment is and how its pass criteria are evaluated). This document covers
**where the supporting bits live and how the harness picks them up**.

Scope keywords used below: **wine root**, **prefix**, **install**,
**manifest**.

---

## 1. Scope

**R-RT-1.1** This spec applies to *wild* experiments only — runs that load a
real D3D9 binary into Wine and exercise dxmt9's PE-side `d3d9.dll` /
`winemetal_dxmt9.dll`. Native macOS experiments (those covered by
`specs/experiments/spec.md` §2 — `DYLD_INSERT_LIBRARIES`) are out of scope
and continue to use no Wine, no prefix, no external install.

**R-RT-1.2** The system manages three categories of non-source artifact:
1. **Wine roots** — the `wine` / `wine64` binary plus its `lib/wine/...`
   support directory. Sourced from Heroic, Homebrew, Apple GPTK, CrossOver,
   Sikarugir, or a manually placed Heroic-style bundle.
2. **Prefixes** — `WINEPREFIX` directories with their own `drive_c`,
   `system.reg`, etc. One prefix per wild experiment.
3. **External app installs** — the actual game / benchmark binaries. Stored
   *outside* the prefix's `drive_c/` and surfaced into the prefix via a
   `dosdevices/` junction.

**R-RT-1.3** Out of scope: Wine binary auto-download, prefix snapshot/restore,
parallel runs of the same experiment, and any sharing of a single prefix
across multiple experiments.

---

## 2. Directory Layout

**R-RT-2.1** All runtime artifacts live under `experiments/`. No project-root
top-level directories (`/prefixs/`, `/apps_3rd/`, `/wine/`) are introduced.
Reason: keeps the experiment namespace cohesive and matches existing
`experiments/apps/` (fixtures), `experiments/output/` (results), etc.

**R-RT-2.2** The required subdirectories are:

| Path | Committed? | Purpose |
|---|---|---|
| `experiments/apps/` | yes | Existing — small fixture EXEs (D9VK, BasicHLSL). Unchanged. |
| `experiments/apps_3rd/` | no (gitignored) | New — externally-installed app binaries (SFIV, 3DMark05, 3DMark06, etc.). One subdirectory per app. |
| `experiments/prefixs/` | no (gitignored) | New — per-experiment Wine prefixes. One subdirectory per app. |
| `experiments/wine/` | mixed | New — manifest committed; downloaded Wine bundles gitignored. |
| `experiments/wine/manifest.toml` | yes | New — Wine root catalogue (see §3). |
| `experiments/wine/README.md` | yes | New — explains the manual-download workflow. |
| `experiments/CATALOGUE.toml` | yes | Existing — extended with runtime fields (see §5). |

**R-RT-2.3** `experiments/apps_3rd/<name>/` and `experiments/prefixs/<name>/`
must use the same `<name>` as the matching `[[app]].name` in
`CATALOGUE.toml`. Naming consistency is mechanical: harness derives both
paths from `name`.

**R-RT-2.4** `.gitignore` must add patterns that ignore `apps_3rd/`,
`prefixs/`, and `experiments/wine/*` while keeping `manifest.toml`,
`README.md`, and `.gitkeep` committed.

**R-RT-2.5** The harness must avoid `/tmp` for any artifact larger than a
few KB or longer-lived than a single CLI invocation. Per-experiment temp
artifacts (capture buffers, intermediate logs, screencaps) go under
`experiments/output/<app>/tmp/` and are cleaned up on the next run of that
experiment.

---

## 3. Wine Manifest

**R-RT-3.1** A single committed file `experiments/wine/manifest.toml`
enumerates every Wine root the harness may use. The schema is defined by
`specs/experiments/assets/wine-manifest.schema.toml`.

**R-RT-3.2** Each entry has, at minimum:
- `id` (required, unique) — short stable string referenced from CATALOGUE.
- `source` (required) — one of `heroic`, `brew`, `gptk`, `crossover`,
  `sikarugir`, `manual`. Drives diagnostics; not behavioral.
- `variant` (required) — one of `vanilla`, `dxmt`, `vk`, `kegworks`,
  `patched`. Drives the rule that "default = `vanilla`".
- `path` (required) — absolute path to the Wine root directory. May contain
  `$HOME` or `$REPO_ROOT`, expanded at load time.

Optional: `version`, `notes`.

**R-RT-3.3** The harness must validate at load time that `<path>/bin/wine`
(or `<path>/bin/wine64`) exists and is executable. On validation failure the
manifest entry is dropped with a warning; an experiment that explicitly
references it errors out.

**R-RT-3.4** No two entries may share an `id`. Duplicates are a hard error.

**R-RT-3.5** The manifest is hand-edited. The harness does not download,
upgrade, or modify entries. A helper script (`scripts/wine/discover.py`,
optional) may *suggest* entries by scanning known install locations; output
goes to stdout for the user to paste in.

---

## 4. Prefix Lifecycle

**R-RT-4.1** A prefix at `experiments/prefixs/<name>/` is created by the
harness's bootstrap step using the wine root resolved from
`[[app]].wine_id`. Bootstrap is `wineboot --init` followed by junction
setup (R-RT-4.2).

**R-RT-4.2** The app install at `experiments/apps_3rd/<name>/` is mounted
into the prefix as a Windows drive letter via a `dosdevices/` symlink:

```
experiments/prefixs/<name>/dosdevices/<letter>:  →  ../../apps_3rd/<name>/
```

Default `<letter>` is `d`. The app is then reachable from within Wine as
`D:\<binary>.exe`. The drive letter is configurable per-app via
`[[app]].install_drive_letter` (default `"d"`).

**R-RT-4.3** The prefix is **single-use per experiment**. The harness must
not run multiple experiments concurrently against the same prefix. (Future
parallel runs would need separate prefix dirs.)

**R-RT-4.4** The harness must support a `--rebuild-prefix` flag that
deletes and re-bootstraps `experiments/prefixs/<name>/`. The matching
`apps_3rd/<name>/` is **not** touched — the user installs the game once,
re-bootstraps the prefix as often as needed.

**R-RT-4.5** A prefix bootstrap that emits any `try_map_free_area` mmap
error during `wineboot` is treated as a soft failure: the harness records
it in `result.json` (`prefix_bootstrap.mmap_errors`) and proceeds. A run
that has bootstrap mmap errors is flagged as `degraded` in the run log
even if the experiment otherwise passes.

---

## 5. CATALOGUE Extensions

**R-RT-5.1** Each `[[app]]` in `experiments/CATALOGUE.toml` that is a wild
experiment (`requires_wine = true`, `source_kind` ≠ `"project-authored"`)
gains the following fields:

| Field | Required | Meaning |
|---|---|---|
| `wine_id` | yes (when `requires_wine = true`) | Default Wine manifest ID for this app. |
| `wine_alternatives` | no | Array of additional Wine IDs explicitly accepted for manual A/B runs. |
| `install_drive_letter` | no (default `"d"`) | Drive letter the prefix mounts `apps_3rd/<name>/` under. |
| `binary` | yes (existing) | Reinterpreted: when the app is wild, this is a Windows-style path beginning with the drive letter (e.g. `"D:/StreetFighterIV_Benchmark.exe"`). For non-wild apps the existing relative-to-repo semantics apply. |

**R-RT-5.2** The `wine_id` referenced from CATALOGUE must exist in the
manifest. Harness validates this at startup; missing IDs are a hard error
with a precise diagnostic (`CATALOGUE.toml [[app]].name=foo: wine_id=bar
not in experiments/wine/manifest.toml`).

**R-RT-5.3** `wine_alternatives` entries are *not* used automatically. They
exist only so a maintainer can write `--wine-id <alternative>` knowing the
experiment was designed to also work under that root. An ID outside both
the default and alternatives is still acceptable on the CLI — it just
counts as an unverified configuration.

**R-RT-5.4** Existing fixture `[[app]]` entries (project-authored sample EXEs
in `experiments/apps/`) are unchanged. Their `binary` remains a relative
path to the repo, no `wine_id` is required (they may opt in if they need
prefix isolation).

---

## 6. Harness Contract

**R-RT-6.1** The harness resolves a wine root in this order, highest priority
first:
1. CLI flag: `--wine-id <id>`
2. Environment variable: `DXMT_EXPERIMENT_WINE_ID=<id>`
3. CATALOGUE entry: `[[app]].wine_id`
4. Hard error if none of the above resolve to a valid manifest entry.

**R-RT-6.2** The harness must record the resolved wine root in
`experiments/output/<name>/result.json` under `wine` as
`{ id, source, variant, path }`. Diagnostic cross-reference for postmortem.

**R-RT-6.3** When `--wine-id` resolves to a non-`vanilla` variant, the
harness must emit a one-line warning at run start unless the chosen ID
appears in `[[app]].wine_alternatives` *or* the user passed
`--allow-non-vanilla`. This codifies the existing
`agents/rules/test_wild.rules.md` rule programmatically.

**R-RT-6.4** Existing wrapper scripts (`scripts/run_apps/*.sh`,
`scripts/run_suites/*.sh`, `scripts/tools/*`) must derive their default
wine root from the manifest (resolve `wine_id` → path) rather than
hardcoding `Wine-11.x` paths. Hardcoded paths are removed.

**R-RT-6.5** All transient artifacts (intermediate captures, scratch logs)
go under `experiments/output/<name>/tmp/`. The harness creates this dir on
each run and is allowed to wipe it on next run of the same experiment.
`/tmp` is reserved for genuinely-ephemeral OS-level handoffs.

---

## 7. Migration

**R-RT-7.1** Existing prefixes at `~/Games/_Prefixes/<name>/` are not
migrated automatically. Each affected experiment is converted by:
1. Re-installing the game into `experiments/apps_3rd/<name>/`.
2. Running `scripts/run_python.sh scripts/run_apps/run_experiment.py run <name> --rebuild-prefix`.
3. Verifying the run.

**R-RT-7.2** SFIV is the first experiment converted (this spec's driving
case) and serves as the migration template.

**R-RT-7.3** *(Retired 2026-07-29. ID retained, not reused.)* This requirement
staged the migration of the one commercial catalogue entry that carried a
documented Wine-DXMT exception, and fixed its `wine_id` to the patched
manifest entry. That app was removed from `experiments/CATALOGUE.toml`; no
catalogue entry now requires a non-vanilla Wine root, and
`agents/rules/test_wild.rules.md` records no documented exceptions. The
general mechanism the requirement relied on is unaffected and still normative:
`wine_alternatives` (R-RT-5.3) plus the run-start warning rather than a hard
refusal (R-RT-6.3). See `specs/experiments/log.md`.

**R-RT-7.4** No deletion of old `~/Games/_Prefixes/<name>/` is mandatory.
A maintainer may keep the old prefix for comparison until the new lane is
stable.

---

## 8. Compatibility & Hygiene

**R-RT-8.1** The runtime layout must not affect non-wild test surfaces:
unit tests, native tests, fixture experiments, the build itself.

**R-RT-8.2** `.gitignore` additions are restricted to the new directories.
No existing pattern is loosened.

**R-RT-8.3** This spec governs only paths and contracts. Specific Wine
behavior — which variant works for which game — remains documented in
`agents/rules/test_wild.rules.md`.

**R-RT-8.4** The wine manifest format is versioned via
`manifest_version = 1`. A future schema change increments the version; the
harness refuses to load an unsupported version with a clear migration
hint.

---

## 9. Non-Goals

- **No** Wine binary auto-download, no automatic upgrade of installed Wine
  builds. The user owns those installs (via Heroic, brew, etc.) or places
  them under `experiments/wine/` manually.
- **No** automatic discovery of installed apps. The user installs the game
  into `experiments/apps_3rd/<name>/` and registers it in CATALOGUE.
- **No** sharing of prefixes between experiments. Each experiment owns its
  prefix dir.
- **No** support for parallel runs of the same experiment. Prefix is a
  single-writer resource.
