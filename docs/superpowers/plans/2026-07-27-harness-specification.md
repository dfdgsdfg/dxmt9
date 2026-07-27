# Harness Specification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write 17 specification documents under `specs/experiments/harness/` that define what each dxmt9 harness family owes its neighbours, so harness corrosion becomes detectable.

**Architecture:** A parent `requirements.md` states five cross-cutting `R-HARN-*` contract groups derived one-to-one from observed defects. A parent `spec.md` carries the eight-stage boundary map, the artifact envelope, and environment-variable ownership. Seven domain subdirectories — one per harness family, not per pipeline stage — each carry `requirements.md` + `spec.md` describing that family's scripts, stages, modes, artifacts, and owned env vars.

**Tech Stack:** Markdown only. No code, no tests, no harness changes.

## Global Constraints

- **Documents only.** Do not modify any file under `scripts/`, `src/`, or `tests/`. Do not add or change any environment variable. Do not change any harness behaviour.
- **Frontmatter is mandatory and follows the repository convention exactly.** `requirements.md` uses `type: "Spec Requirements"`; `spec.md` uses `type: "Spec"`; `plan.md` uses `type: "Spec Plan"`. Every file carries `title`, `description`, and a `tags` array.
- **Requirement IDs use the `R-HARN-` prefix**, which is unused in the repository today. Numbering is `R-HARN-<section>.<item>`, matching the style of `R-RT-1.1` and `R-VERIF-2.1`.
- **Every requirement must be phrased as a predicate** — something a future checker could evaluate. "Harnesses should be robust" is a plan failure; "every input-classification branch that selects a fallback is reachable only behind a named opt-in, or exits non-zero" is correct.
- **Describe intent, not current behaviour, where they differ** — and say which is which. Four of the five `replay` defects are unfixed and the black-output cause is unknown. The `replay` spec states the intended contract and records the broken state as an open item. It must never describe current behaviour as correct.
- **Do not invent scripts, environment variables, or artifact fields.** Every script named must exist on disk. Every env var named must appear either in `agents/rules/environment_variables_*.rules.md` or in the harness source. Verify before writing.
- **Do not modify `specs/index.md`.** Its `Topics` section lists top-level domains only; sibling subdirectories such as `specs/experiments/runtime/` are not listed there, and consistency beats discoverability here.
- Line width in `specs/` wraps at roughly 76 characters. Match the surrounding files.

---

## File Structure

| File | Responsibility |
|---|---|
| `specs/experiments/harness/requirements.md` | The five `R-HARN-*` contract groups. Binds every domain. |
| `specs/experiments/harness/spec.md` | Eight-stage boundary map, artifact envelope schema, env-ownership rules. |
| `specs/experiments/harness/plan.md` | Rollout order only, and **untracked** — see the correction section above. The legacy envelope migration belongs in `spec.md` and the open items in `specs/experiments/gap.md`. |
| `specs/experiments/harness/runner/{requirements,spec}.md` | Catalogue runner and launchers. |
| `specs/experiments/harness/probe/{requirements,spec}.md` | Capture orchestration and preflights. |
| `specs/experiments/harness/replay/{requirements,spec}.md` | Dump → manifest → standalone Metal replay. |
| `specs/experiments/harness/reduce/{requirements,spec}.md` | dxmt9 logs → CSV. |
| `specs/experiments/harness/join/{requirements,spec}.md` | External-tool exports joined to dxmt9 attribution. |
| `specs/experiments/harness/gate/{requirements,spec}.md` | Comparison and proof gates. |
| `specs/experiments/harness/audit/{requirements,spec}.md` | Repository-level audits. |

Each domain file pair is self-contained: a reader asking "what does the replay harness owe me?" reads two short files, not seventeen.

## One design-document correction — CORRECTED AGAIN, read this

The design doc (`docs/superpowers/specs/2026-07-27-harness-specification-design.md`) twice says open items go in `gap.md`, but its layout block lists only `requirements.md`, `spec.md`, and `plan.md` at the parent.

My first resolution of that inconsistency was **wrong**, and Task 3's implementer caught it. I reasoned from the observation that spec subdirectories carry no `gap.md` and routed open items into `specs/experiments/harness/plan.md`. I had not read the rule that governs the question. `.gitignore:37` carries `specs/**/plan.md`, and `agents/rules/documentation_spec.rules.md:44-47` states: "`specs/**/plan.md` and `*.plan.md` files are local-only and gitignored. Do not commit implementation plans. Promote durable ordering constraints into `spec.md`, missing work into the owning `specs/<topic>/gap.md`." Sibling evidence is unanimous — `specs/verification/plan.md`, `specs/experiments/runtime/plan.md`, and `specs/backend/draw-uniforms/plan.md` are all untracked.

The design document's original instinct was right. The correct routing is:

| Content | Destination |
|---|---|
| Durable ordering constraints, including the legacy envelope migration | `specs/experiments/harness/spec.md` |
| Open items / missing work | `specs/experiments/gap.md`, the owning topic's gap document |
| Rollout order (session-local sequencing) | `specs/experiments/harness/plan.md`, left **untracked** |

**The committed deliverable is therefore 16 files, not 17** — two at the parent (`requirements.md`, `spec.md`) plus fourteen across the seven domains. `specs/experiments/harness/plan.md` may exist on disk but must not be tracked.

---

### Task 1: Parent requirements — the five contract groups

**Files:**
- Create: `specs/experiments/harness/requirements.md`

**Interfaces:**
- Consumes: nothing.
- Produces: requirement IDs `R-HARN-1.1` through `R-HARN-6.x` that every later task cites. Section numbering is fixed here: §1 Scope, §2 No Silent Degradation, §3 Output Validity, §4 Boundary Semantics, §5 Engine-Shape Dependencies, §6 Diagnostic Paths.

**Background the writer needs.** Five defects in `scripts/tools/run_3dmark05_mini_replay.py` blocked a vertex-remap experiment on 2026-07-25/27. Each contract group below exists because of one of them. Cite the defect in the requirement's rationale so a future reader knows the contract is not speculative.

1. `transform_msl` raised `SystemExit("mini replay cbuf rewrite could not find buffer(30) argbuf parameter")` because `DXMT9_ARGBUF_DIRECT_CBUF` was promoted default-on in commit `9eb02437`; dumped MSL now binds constants at `buffer(0)`/`buffer(3)` with no argbuf parameter.
2. Sliced stream payloads double-counted `stream0_offset`. The geometry dump writes a slice starting at `stream0_start_byte`, and `stream0_start_byte == stream0_offset`, so payload byte 0 is fetch slot 0. The replay treated the same number as an offset within the payload, producing negative slot capacities. Latent for months because every previously used row had `stream0_offset == 0`.
3. `color_pixel_format()` handles `core::Format` 1-4 and returns `MTLPixelFormatRGBA8Unorm` for everything else. Row `60/0` renders to R32F (`core::Format` 16), so the replay rendered into a wrong-format attachment without any diagnostic.
4. All four replay lanes reported `mini replay draws=229 repeat=1` and exited 0 while writing a 1024x768 PPM containing exactly one distinct pixel value.
5. `--force-fragment-color` fails to compile: it returns `float4` from a function whose declared return type is `FfpFsOut`.

- [ ] **Step 1: Write the file**

Use this frontmatter exactly:

```markdown
---
type: "Spec Requirements"
title: "Harness Requirements — Evidence Production Contracts"
description: "Harness requirements and cross-domain contracts."
tags: [specs, experiments, harness, requirements]
---
```

Then write these sections. Each requirement is a predicate.

**§1 Scope.** State that this spec governs the scripts under `scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, and `scripts/run_suites/` — 86 files at time of writing — grouped into seven domains. State that it governs contracts between harnesses, not the correctness of what they measure. State that `agents/rules/environment_variables_*.rules.md` remains the catalogue of knobs and this spec is the contract over them, quoting those files' own line: "These files are **descriptive**, not a behavioral spec — for that, see `specs/`."

**§2 No silent degradation.** Requirements covering: a harness produces a valid artifact or exits non-zero; an unsupported input classification must not silently select a fallback; a fallback that is genuinely wanted is reachable only behind a named opt-in flag; the resolved choice is recorded in the artifact envelope. Rationale cites defect 3.

**§3 Output validity self-assertion.** Requirements covering: a harness producing a measurement artifact asserts the artifact is non-degenerate before reporting success; the assertion result is recorded in the envelope's `validity` field; a harness that cannot assert validity exits non-zero rather than reporting success. Rationale cites defect 4, and notes that a degenerate artifact is worse than a missing one because it silently passes downstream gates — the four identical black images produced four identical SHA-256 digests, which a naive equality gate would have read as a pass.

**§4 Boundary semantics are declared, not inferred.** Requirements covering: an artifact crossing a domain boundary carries in band the information needed to interpret its bytes; offsets, strides, slice origins, and index bases are interpretation rules and must name their coordinate system; consumers compute from declared fields rather than assuming a convention. Rationale cites defect 2 and makes the point that neither side was individually wrong — both were internally consistent, and no sentence connected them, which is a class of defect code review cannot catch.

**§5 Engine-shape dependencies are pinned and detectable.** Requirements covering: a harness that pattern-matches engine output — MSL signatures, log line formats, CSV column names — declares the expected shape in its domain spec; its failure message names that expectation; a shape change that the harness cannot handle is a failure, not a fallback. Rationale cites defect 1.

**§6 Diagnostic paths carry the primary contract.** Requirements covering: diagnostic and secondary modes are bound by the same contracts as the primary path; every flag that alters harness output appears in the owning domain spec's mode table. Rationale cites defect 5.

- [ ] **Step 2: Verify frontmatter and ID conventions**

```sh
head -6 specs/experiments/harness/requirements.md
grep -c '\*\*R-HARN-' specs/experiments/harness/requirements.md
grep -oE '\*\*R-HARN-[0-9]+\.[0-9]+' specs/experiments/harness/requirements.md | sort | uniq -d
```

Expected: frontmatter matches the block above; at least 12 requirements; the third command prints nothing (no duplicate IDs).

- [ ] **Step 3: Verify every cited fact is real**

```sh
git log --oneline -1 9eb02437
grep -n "could not find buffer(30) argbuf parameter" scripts/tools/run_3dmark05_mini_replay.py
grep -n "def color_pixel_format" -A 8 scripts/tools/run_3dmark05_mini_replay.py
grep -rn "descriptive" agents/rules/environment_variables.rules.md
```

Expected: the commit resolves; the error string exists; `color_pixel_format` handles only formats 1-4 then falls through; the rules file contains the "descriptive" sentence quoted in §1. If any check fails, correct the document rather than the source.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/requirements.md
git commit -m "specs(harness): state cross-domain harness contracts

Five R-HARN contract groups, each derived from one of the defects that
blocked the 2026-07-25 vertex-remap experiment. Requirements are phrased
as predicates so an enforcement checker can be added later without
rewriting them."
```

---

### Task 2: Parent spec — boundary map, envelope, env ownership

**Files:**
- Create: `specs/experiments/harness/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs from Task 1.
- Produces: the eight stage names and seven boundary names that every domain spec cites; the artifact envelope field list; the env-ownership rule that each domain spec instantiates.

**Stage names — use these exact strings in every document:**
`build-stage`, `run-capture`, `dump-extract`, `log-reduce`, `offline-replay`, `external-join`, `compare-gate`, `record`.

**Boundaries** are named by the pair they join, for example `run-capture → dump-extract`.

- [ ] **Step 1: Write the file**

Frontmatter:

```markdown
---
type: "Spec"
title: "Harness Spec — Stages, Artifacts, Environment Ownership"
description: "Harness stage boundary map, artifact envelope, and env ownership."
tags: [specs, experiments, harness, spec]
---
```

**§1 Domain map.** A table mapping each of the seven domains to its owning scripts and the stages it participates in. Use exactly this assignment:

| Domain | Owns | Stages |
|---|---|---|
| `runner` | `scripts/run_apps/run_experiment.py`, the catalogue launchers under `experiments/launchers/`, `scripts/run_apps/*.sh`, `scripts/run_suites/*` | `build-stage`, `run-capture` |
| `probe` | `scripts/tools/run_3dmark05_perf_probe.sh`, `scripts/tools/run_with_wine_metal_capture_layer.sh`, `scripts/tools/run_3dmark05_system_trace_sidecar.sh` | `run-capture`, `dump-extract` |
| `replay` | `scripts/tools/build_3dmark05_mini_replay_manifest.py`, `scripts/tools/plan_3dmark05_mini_replay.py`, `scripts/tools/run_3dmark05_mini_replay.py` | `offline-replay` |
| `reduce` | the `scripts/tools/summarize_*` that read dxmt9's own logs, including `summarize_3dmark05_perf.py`, `summarize_index_cache_runtime.py`, `summarize_framegraph_dag.py` | `log-reduce` |
| `join` | `scripts/tools/finalize_3dmark05_perf_probe.sh`, `scripts/tools/summarize_xcode_encoder_counters.py`, `scripts/tools/summarize_xctrace_metal_intervals.py`, `scripts/tools/summarize_xctrace_cpu_threads.py` | `external-join` |
| `gate` | `scripts/tools/compare_*`, `scripts/tools/analyze_xcode_replay_variance.py` | `compare-gate` |
| `audit` | `scripts/check/*` | `record` |

State the rule that decides `reduce` versus `join`: a summariser belongs to `reduce` when its input is a dxmt9-produced log, and to `join` when its input is an external tool's export. State that the domain axis is harness families rather than stages because a single script spans several stages — `run_3dmark05_perf_probe.sh` spans `run-capture`, `dump-extract`, and `log-reduce` — and a stage-based split would leave no single owner for its contract.

**§2 Stage boundary map.** For each of the seven boundaries, one subsection naming what crosses, in what form, and the interpretation rules that travel with it. The `dump-extract → offline-replay` boundary is the one that failed and must be the most precise: geometry payloads are **slices**, written from `stream0_start_byte`, so payload byte 0 is fetch slot 0, and a consumer computes the in-payload offset as `stream0_offset - stream0_start_byte` rather than using `stream0_offset` directly. Cite `R-HARN-4.*`.

**§3 Artifact envelope.** The field table:

| Field | Content |
|---|---|
| `schema` | Schema name and version string |
| `producer` | Script path and git revision |
| `stage` | One of the eight stage names |
| `domain` | One of the seven domain names |
| `inputs` | Consumed upstream artifacts: path and digest per entry |
| `env_snapshot` | Resolved values of contract-relevant environment variables actually in effect |
| `validity` | Result of the `R-HARN-3.*` validity assertion |

State that the envelope is provenance and is separate from measurement payloads. Name the concrete consequence for `result.json`: its counter payload is consumed by `run_experiment.py`'s `expected_counters` L3 gate, by `compare_3dmark05_perf_counters.py`, and by `source:` citations in `docs/perfomance/`, so the envelope replaces the provenance fields only and the counter payload stays. State the second use of `inputs` digests: on 2026-07-27, 34 of the 56 log paths cited as `source:` evidence in `docs/perfomance/` were found already missing, and digested inputs make that state machine-detectable.

**§4 Environment ownership.** Define contract-relevant: a variable whose value changes what a measurement means or how a downstream artifact must be interpreted. Give the discriminating example — a logging-verbosity knob is not contract-relevant; `DXMT9_ARGBUF_DIRECT_CBUF` is, because it changes the emitted MSL signature that `replay` pattern-matches, which is exactly how defect 1 arose. Then the three rules: exactly one owning domain may set each contract-relevant variable and downstream domains may read but not set; any forwarded variable appears in `env_snapshot`; a contract-relevant variable is never silently defaulted and its resolved value is recorded whether it came from the caller, the `DXMT_EXPERIMENT_PROFILE` profile, or the engine default.

- [ ] **Step 2: Verify every named script exists**

```sh
for f in scripts/run_apps/run_experiment.py \
         scripts/tools/run_3dmark05_perf_probe.sh \
         scripts/tools/run_with_wine_metal_capture_layer.sh \
         scripts/tools/run_3dmark05_system_trace_sidecar.sh \
         scripts/tools/build_3dmark05_mini_replay_manifest.py \
         scripts/tools/plan_3dmark05_mini_replay.py \
         scripts/tools/run_3dmark05_mini_replay.py \
         scripts/tools/summarize_3dmark05_perf.py \
         scripts/tools/summarize_index_cache_runtime.py \
         scripts/tools/summarize_framegraph_dag.py \
         scripts/tools/finalize_3dmark05_perf_probe.sh \
         scripts/tools/summarize_xcode_encoder_counters.py \
         scripts/tools/summarize_xctrace_metal_intervals.py \
         scripts/tools/summarize_xctrace_cpu_threads.py \
         scripts/tools/analyze_xcode_replay_variance.py; do
  test -f "$f" || echo "MISSING: $f"
done
```

Expected: no output.

- [ ] **Step 3: Verify the boundary claim against the real dump**

```sh
grep -E "^stream0_(offset|start_byte|stride)=|^base_vertex=|^index_type=" \
  traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/geometry/*.meta | head -20
```

Expected: `stream0_start_byte` equals `stream0_offset` on every row that has a non-zero offset, confirming the slice rule §2 states. If those trace files have been cleaned up, state the rule with its derivation instead and note the evidence path in the document's source line.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/spec.md
git commit -m "specs(harness): map stages, artifacts, and env ownership

Eight stages and seven boundaries, with the dump-extract to offline-replay
boundary stated precisely because that is the one that silently failed.
Defines the artifact envelope as provenance separate from measurement
payloads so result.json's counter gates survive the migration."
```

---

### Task 3: Parent plan — rollout, legacy migration, open items

**Files:**
- Create: `specs/experiments/harness/plan.md`
- Modify: `specs/experiments/gap.md` — add one pointer line

**Interfaces:**
- Consumes: stage and domain names from Task 2.
- Produces: the Open Items section that later readers check for known-broken state.

- [ ] **Step 1: Write the plan file**

Frontmatter uses `type: "Spec Plan"`, title `"Harness Plan — Rollout and Legacy Migration"`.

Cover four things.

**Rollout order.** Domains are specified in dependency order: `runner` and `probe` first because everything downstream consumes their artifacts; then `replay`, `reduce`, `join`; then `gate` and `audit`.

**Legacy envelope migration.** Today's provenance is spread across `result.json` and `3dmark05-trace-artifacts.json` in ad-hoc shapes. The migration adds the envelope alongside, moves consumers over, then removes the legacy provenance fields. The counter payload in `result.json` is not part of the envelope and is not removed. State plainly that no migration step has been performed.

**Open items.** At minimum:
- Enforcement is unbuilt. Requirements are predicates but nothing evaluates them. This is deliberate — the docs-only scope was chosen on 2026-07-27 — and it means these documents can corrode exactly as the harnesses did.
- The `replay` harness does not currently work. Defects 1, 3, 4, and 5 are unfixed; defect 2 was fixed in commit `12348666`. The cause of the black replay output is unknown: constants, scissor, cull, depth input, and draw issue were all eliminated, and `--force-fragment-color`, the tool that would bisect geometry from fragment, is itself broken.
- `scripts/tools/summarize_3dmark05_cleanup_candidates.py` miscounts brace-expanded citations such as `...-r{1,2,3}-...`, classifying 84 referenced runs (4.5 GB) as unreferenced.
- 34 of the 56 log paths cited as `source:` in `docs/perfomance/` are already missing; the citations are dangling.

**Non-goals.** No harness code changes, no new harness, no enforcement checker, no artifact migration.

- [ ] **Step 2: Add the pointer to the experiments gap document**

Append one line to `specs/experiments/gap.md` pointing at `harness/plan.md` for harness open items. Match the file's existing line style — read it first.

- [ ] **Step 3: Verify the referenced commits resolve**

```sh
git log --oneline -1 12348666
grep -n "brace" docs/superpowers/plans/2026-07-25-vertex-remap.md | head -3
```

Expected: `12348666` resolves to the mini-replay repair commit. If it does not, find the correct SHA with `git log --oneline --grep="sliced stream offset"` and use that.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/plan.md specs/experiments/gap.md
git commit -m "specs(harness): record rollout, legacy migration, open items

Open items name the unbuilt enforcement, the four unfixed replay defects,
and two known-bad evidence conditions, so a reader is not misled into
treating the current harness state as the specified one."
```

---

### Task 4: Runner domain

**Files:**
- Create: `specs/experiments/harness/runner/requirements.md`
- Create: `specs/experiments/harness/runner/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs, stage names, envelope fields.
- Produces: `R-HARN-RUN-*` requirement IDs.

**Facts to verify before writing.** Read `scripts/run_apps/run_experiment.py` for: the `stage_dxmt9` build-and-install step, the `--timeout` handling and `allow_timeout` / `require_positive_timeout` catalogue keys, the `expected_counters` L3 gate, and the `result.json` writer. Read `experiments/CATALOGUE.toml` for the per-app keys. The `DXMT_EXPERIMENT_*` variables are catalogued in `agents/rules/environment_variables_wine.rules.md`.

- [ ] **Step 1: Write `requirements.md`**

Frontmatter `type: "Spec Requirements"`, title `"Harness Runner Requirements — Catalogue Runs"`, tags `[specs, experiments, harness, runner, requirements]`.

Requirements must cover: which stages the domain participates in; that a run always produces an output directory even on failure or timeout; that a positive timeout is required for apps whose catalogue entry sets `require_positive_timeout`; that the counter payload and the envelope are separate artifacts; that `DXMT_EXPERIMENT_*` variables are owned by this domain and no downstream domain sets them. Cite the parent `R-HARN-*` group each requirement instantiates.

- [ ] **Step 2: Write `spec.md`**

Frontmatter `type: "Spec"`, title `"Harness Runner Spec — Catalogue Runs"`.

Cover: the script inventory; the artifact directory layout under `experiments/output/<app-runid>/` naming the files actually produced — `result.json`, `dxmt9.log`, `actual.png`, and the per-app extras; the env vars this domain owns with a one-line purpose each; the mode table listing every flag that alters output.

- [ ] **Step 3: Verify claims against source**

```sh
grep -n "def stage_dxmt9\|expected_counters\|require_positive_timeout\|allow_timeout" scripts/run_apps/run_experiment.py | head
ls experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/
```

Expected: each grep term is found; the directory listing matches the artifact layout the spec describes. Correct the document to match reality, never the reverse.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/runner/
git commit -m "specs(harness): specify the runner domain"
```

---

### Task 5: Probe domain

**Files:**
- Create: `specs/experiments/harness/probe/requirements.md`
- Create: `specs/experiments/harness/probe/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs, stage names, envelope fields; runner artifacts.
- Produces: `R-HARN-PROBE-*` requirement IDs; the geometry-dump interpretation rules that `replay` consumes.

**Facts to verify before writing.** Run `bash scripts/tools/run_3dmark05_perf_probe.sh --help` and read the preflight and watchdog logic. Known behaviour worth specifying, all observed on 2026-07-27: the watchdog wraps the whole runner at `timeout + capture delay + slack`; a `--dry-run` prints resolved paths, env, free space, and `file_capture_layer_preflight` status; file `.gputrace` capture fails preflight unless `--with-wine-capture-layer` is passed; that wrapper replaces `wine.real` and `wine-preloader` with `MetalCaptureEnabled` copies and restores them, and `agents/rules/metal_debugging.rules.md` records that it must use same-directory temp files plus `mv`, never in-place `cp`, because `cp` reproduced `SIGKILL (Code Signature Invalid)`.

- [ ] **Step 1: Write `requirements.md`**

Title `"Harness Probe Requirements — Capture Orchestration"`.

Requirements must cover: preflights fail before launching Wine rather than after — free space, session lock, capture layer, and Xcode attach; a positive timeout and an outer watchdog are mandatory because 3DMark05 can hang after emitting its useful data; a mutated external runtime is always restored, including on signal; the geometry dump declares its coordinate system per `R-HARN-4.*`; env vars forwarded to the runtime appear in `env_snapshot`.

- [ ] **Step 2: Write `spec.md`**

Title `"Harness Probe Spec — Capture Orchestration"`.

Cover: the script inventory; the `traces/<run-id>/` artifact layout naming `frame<N>.gputrace`, `analysis/geometry/`, `analysis/shaders/msl/`, `analysis/frame<N>-depth.bin`, and `3dmark05-trace-artifacts.json`; the geometry `.meta` field contract with the slice rule stated explicitly; the env vars owned and forwarded; the mode table.

The `.meta` contract is the load-bearing part. State that `stream0_start_byte` is the slice origin, `stream0_offset` is the D3D9 stream offset in the source buffer, these are equal when the dump slices at the offset, and a consumer computes the in-payload offset by subtraction.

- [ ] **Step 3: Verify against a real dump**

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh --help | grep -cE "^  --"
head -30 traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/geometry/*.meta | head -30
```

Expected: the flag count is non-zero and the mode table covers the output-altering ones; the `.meta` fields match those the spec names. If the trace directory has been cleaned, cite the design document's recorded field list instead and say so in the source line.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/probe/
git commit -m "specs(harness): specify the probe domain

States the geometry .meta slice rule explicitly: start_byte is the slice
origin, stream0_offset is the source-buffer offset, and the in-payload
offset is their difference. Leaving that unwritten is what let the replay
harness double-count it."
```

---

### Task 6: Replay domain

**Files:**
- Create: `specs/experiments/harness/replay/requirements.md`
- Create: `specs/experiments/harness/replay/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs, stage names, the probe geometry `.meta` contract.
- Produces: `R-HARN-REPLAY-*` requirement IDs.

**This domain is currently broken. Write the intended contract, and record actual state as open items.** Four of five defects are unfixed. Do not describe current behaviour as correct anywhere.

**Facts to verify before writing.** Read `scripts/tools/run_3dmark05_mini_replay.py` for: `transform_msl` and its argbuf-versus-direct-cbuf branches; `color_pixel_format` and `depth_pixel_format`; `resolve_stream_payload_offset`; the `--primitive-order`, `--vertex-order`, `--draw-order`, `--trim-vsout-to-fs-reads`, `--force-fragment-color`, `--force-fragment-primitive-id`, `--depth-clear`, `--depth-input`, `--texture-input-dir`, `--capture-path`, and `--color-output` flags. Read `scripts/tools/build_3dmark05_mini_replay_manifest.py` for the manifest schema string and the `--shader-summary` input. Note that the replay binary hardcodes `MTLIndexTypeUInt16`.

- [ ] **Step 1: Write `requirements.md`**

Title `"Harness Replay Requirements — Offline Metal Replay"`.

Requirements must cover, each citing its parent group: the replay declares which engine MSL shape it accepts and fails naming that expectation when it does not match (`R-HARN-5.*`); an unsupported attachment format is a failure, not a fallback (`R-HARN-2.*`); the replay asserts its output image is non-degenerate before reporting success (`R-HARN-3.*`); in-payload offsets are computed from declared fields, never assumed (`R-HARN-4.*`); every output-altering flag including diagnostics is covered by the mode table and kept compiling (`R-HARN-6.*`); index width support is declared, since the emitter hardcodes uint16.

- [ ] **Step 2: Write `spec.md`**

Title `"Harness Replay Spec — Offline Metal Replay"`.

Cover: the script inventory and the manifest schema string `dxmt9.3dmark05.mini_replay_manifest.v1`; the input contract from `probe`; the mode table listing every flag above with what it alters; the env vars `DXMT9_MINI_REPLAY_CAPTURE_PATH`, `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH`, `DXMT9_MINI_REPLAY_REPEAT`, and `DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB`; the declared engine-shape expectations, naming both the legacy `constant ArgbufLayout& abuf [[buffer(30)]]` signature and the current direct-cbuf `constant PsConsts& psConsts [[buffer(0)]]` form.

Add a **Known deviations** section listing the four unfixed defects with their symptoms, and stating that defect 2 was fixed in `12348666`. State that the black-output cause is unresolved and that the `--force-fragment-color` bisection tool is itself broken, so the usual next diagnostic step is unavailable.

- [ ] **Step 3: Verify claims against source**

```sh
grep -n "mini_replay_manifest.v1" scripts/tools/build_3dmark05_mini_replay_manifest.py
grep -n "def transform_msl\|def color_pixel_format\|def resolve_stream_payload_offset" scripts/tools/run_3dmark05_mini_replay.py
python3 scripts/tools/run_3dmark05_mini_replay.py --help | grep -oE '^\s+--[a-z-]+' | sort -u
grep -n "MTLIndexTypeUInt16" scripts/tools/run_3dmark05_mini_replay.py
```

Expected: the schema string matches; all three functions exist; every flag the mode table lists appears in `--help` and vice versa; the uint16 hardcode is present.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/replay/
git commit -m "specs(harness): specify the replay domain

Documents the intended contract and records the four unfixed defects as
known deviations. The domain is specified against a harness that does not
currently render, so current behaviour is nowhere described as correct."
```

---

### Task 7: Reduce domain

**Files:**
- Create: `specs/experiments/harness/reduce/requirements.md`
- Create: `specs/experiments/harness/reduce/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs, stage names; runner and probe log artifacts.
- Produces: `R-HARN-REDUCE-*` requirement IDs; the CSV shapes that `join` and `gate` consume.

**Facts to verify before writing.** Read `scripts/tools/summarize_3dmark05_perf.py` for the CSV files it emits. The observed outputs are `3dmark05-perf-summary.md`, `3dmark05-perf-encoders.csv`, `3dmark05-perf-encoder-streams.csv`, `3dmark05-perf-indexed-probe-draws.csv`, `3dmark05-perf-frames.csv`, `3dmark05-perf-render-pass-reentry.csv`, `3dmark05-perf-argbuf-payload-delta-sources.csv`, and `3dmark05-perf-vs-const-setter-ranges.csv`.

- [ ] **Step 1: Write `requirements.md`**

Title `"Harness Reduce Requirements — Log Reduction"`.

Requirements must cover: the reducer's input is a dxmt9-produced log and its log-line expectations are declared per `R-HARN-5.*`; a log line shape it cannot parse is reported, not skipped silently; emitted CSV column names are part of the contract because `join` and `gate` consume them by name; an empty reduction is reported as such rather than emitted as a valid-looking empty CSV, per `R-HARN-3.*`.

- [ ] **Step 2: Write `spec.md`**

Title `"Harness Reduce Spec — Log Reduction"`.

Cover: the script inventory with one line each; the emitted artifact list above; the log-line prefixes consumed, such as `[dxmt9-perf-encoder]` and `[dxmt9-perf-indexed-probe-draw]`; the env vars that gate the underlying log emission, which are owned by `runner` or `probe` and only read here.

- [ ] **Step 3: Verify the emitted artifact list**

```sh
grep -n "3dmark05-perf-.*\.csv\|3dmark05-perf-summary.md" scripts/tools/summarize_3dmark05_perf.py | head -12
ls experiments/output/app-d3d9-3dmark05-vertexremap-enc1-r1/*.csv
```

Expected: the two lists agree with what the spec names.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/reduce/
git commit -m "specs(harness): specify the reduce domain"
```

---

### Task 8: Join domain

**Files:**
- Create: `specs/experiments/harness/join/requirements.md`
- Create: `specs/experiments/harness/join/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs, stage names; reduce CSV shapes.
- Produces: `R-HARN-JOIN-*` requirement IDs; the joined CSV shape `gate` consumes.

**Facts to verify before writing.** Read `scripts/tools/finalize_3dmark05_perf_probe.sh` and `scripts/tools/summarize_xcode_encoder_counters.py`. This domain has a property no other domain has: **its input arrives through a manual GUI step.** Xcode encoder counters have no CLI export, so a human performs Open, Show Performance, Counters, wait for draw-counter profiling, Export. `agents/rules/metal_debugging.rules.md` §2b documents the sequence and warns that Xcode's save panel can retain a previous run's `analysis` folder, so the landing path must be verified rather than trusted.

- [ ] **Step 1: Write `requirements.md`**

Title `"Harness Join Requirements — External Tool Joins"`.

Requirements must cover: the human-in-the-loop input step is part of the contract and its expected file path and column shape are declared; a missing or wrong-run counter export is detected rather than joined into misleading rows, citing the existing `--require-xcode-counter-coverage` and `--require-dxmt-join-coverage` gates; join coverage is reported as a number, not asserted; the external tool's column names are a declared engine-shape dependency per `R-HARN-5.*`.

- [ ] **Step 2: Write `spec.md`**

Title `"Harness Join Spec — External Tool Joins"`.

Cover: the script inventory; the manual export procedure by reference to `agents/rules/metal_debugging.rules.md` §2b rather than duplicating it; the expected input paths such as `traces/<run>/analysis/frame<N>-counters-xcode.csv`; the produced artifacts including `frame<N>-xcode-dxmt-joined-summary.csv` and the bottleneck report; the coverage-gate flags.

- [ ] **Step 3: Verify the flags and paths**

```sh
grep -n "require-xcode-counter-coverage\|require-dxmt-join-coverage\|joined-summary" scripts/tools/finalize_3dmark05_perf_probe.sh | head
ls traces/*/analysis/frame*-counters-xcode.csv 2>/dev/null | head -3
```

Expected: the gate flags exist; at least one historical counter export exists to confirm the path shape, or the spec records that none remain on disk.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/join/
git commit -m "specs(harness): specify the join domain

Records the manual Xcode counter export as part of the contract, since it
is the one input in the pipeline that no script can produce."
```

---

### Task 9: Gate domain

**Files:**
- Create: `specs/experiments/harness/gate/requirements.md`
- Create: `specs/experiments/harness/gate/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs, stage names; reduce and join CSV shapes.
- Produces: `R-HARN-GATE-*` requirement IDs.

**Facts to verify before writing.** Read `scripts/tools/compare_xcode_dxmt_bottlenecks.py` for its `--require-*` proof gates and `scripts/tools/analyze_xcode_replay_variance.py` for the coefficient-of-variation gate and its `--max-cv-pct` default. `agents/rules/metal_debugging.rules.md` records that Xcode replay variance is typically under 5% and that sub-10% deltas need the variance analysis before they can be believed.

- [ ] **Step 1: Write `requirements.md`**

Title `"Harness Gate Requirements — Comparison and Proof"`.

Requirements must cover: a gate states which mechanism it proves, not merely that numbers moved; a gate over degenerate inputs must fail rather than pass, since identical empty artifacts compare equal — cite the observed case where four identical black images produced four identical digests that a naive equality gate would have read as a pass; a delta inside the measured noise floor is reported as inconclusive rather than as a result; gates that require a baseline fail when the baseline is absent rather than running standalone.

- [ ] **Step 2: Write `spec.md`**

Title `"Harness Gate Spec — Comparison and Proof"`.

Cover: the script inventory; the proof-gate flag families with what each proves; the variance procedure and its 5% default; the rule that run-level gates require `--baseline-output` and Xcode comparison gates require `--baseline-joined`.

- [ ] **Step 3: Verify the gate flags**

```sh
python3 scripts/tools/compare_xcode_dxmt_bottlenecks.py --help | grep -oE '\--require-[a-z-]+' | sort -u
python3 scripts/tools/analyze_xcode_replay_variance.py --help | grep -n "max-cv-pct" 
```

Expected: the flag list matches the spec's table; the CV flag exists with its default.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/gate/
git commit -m "specs(harness): specify the gate domain

Requires that a gate over degenerate inputs fails rather than passes,
which is the failure mode that let four identical empty images produce a
clean-looking equality result."
```

---

### Task 10: Audit domain

**Files:**
- Create: `specs/experiments/harness/audit/requirements.md`
- Create: `specs/experiments/harness/audit/spec.md`

**Interfaces:**
- Consumes: `R-HARN-*` IDs, stage names.
- Produces: `R-HARN-AUDIT-*` requirement IDs.

**Facts to verify before writing.** Read `scripts/check/audit_perf_docs_sources.py`. Its `audit_paths` checks two things only: that a leaf carries a frontmatter `source:` field, and that the field does not cite the retired `specs/perfomance.plan.md`. **It does not verify that cited paths exist on disk**, and by default it audits only git-new files. Describe this accurately — the gap between what the audit is assumed to check and what it checks is itself the thing worth writing down. Check `tests/meson.build` for which audits are registered as Meson tests.

- [ ] **Step 1: Write `requirements.md`**

Title `"Harness Audit Requirements — Record Verification"`.

Requirements must cover: an audit declares precisely what it does and does not check; an audit registered as a Meson test is part of CI and its scope is stated; citation audits verify referent existence, which is the gap `audit_perf_docs_sources.py` currently has and which left 34 of 56 cited log paths dangling undetected.

- [ ] **Step 2: Write `spec.md`**

Title `"Harness Audit Spec — Record Verification"`.

Cover: the script inventory from `scripts/check/`; which are registered as Meson tests and under which names; for `audit_perf_docs_sources.py` specifically, an explicit checks-versus-does-not-check table.

- [ ] **Step 3: Verify the audit semantics and registration**

```sh
grep -n "def audit_paths" -A 22 scripts/check/audit_perf_docs_sources.py
grep -n "scripts/check" tests/meson.build
```

Expected: the function checks only the two conditions described; the Meson registrations match the spec's list.

- [ ] **Step 4: Commit**

```bash
git add specs/experiments/harness/audit/
git commit -m "specs(harness): specify the audit domain

States what audit_perf_docs_sources.py does not check — referent
existence — since that gap let 34 of 56 cited evidence paths go dangling
without any signal."
```

---

### Task 11: Cross-document consistency pass

**Files:**
- Modify: any of the 17 files that fail a check below.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: nothing new.

- [ ] **Step 1: Check every cited requirement ID resolves**

```sh
cd specs/experiments/harness
grep -rhoE 'R-HARN-[0-9]+\.[0-9]+' . | sort -u > /tmp/cited.txt
grep -ohE '\*\*R-HARN-[0-9]+\.[0-9]+' requirements.md | tr -d '*' | sort -u > /tmp/defined.txt
comm -23 /tmp/cited.txt /tmp/defined.txt
```

Expected: no output. Any line printed is a cited-but-undefined requirement — fix the citation or add the requirement.

- [ ] **Step 2: Check frontmatter on all 17 files**

```sh
cd specs/experiments/harness
for f in $(find . -name '*.md'); do
  head -1 "$f" | grep -q '^---$' || echo "NO FRONTMATTER: $f"
  grep -q '^tags: \[specs, experiments, harness' "$f" || echo "BAD TAGS: $f"
done
```

Expected: no output.

- [ ] **Step 3: Check stage names are used consistently**

```sh
cd specs/experiments/harness
grep -rhoE '\b(build-stage|run-capture|dump-extract|log-reduce|offline-replay|external-join|compare-gate|record)\b' . | sort | uniq -c
```

Expected: all eight names appear. A name appearing once may be a typo of another — inspect any singleton.

- [ ] **Step 4: Check the tracked file count**

```sh
git ls-files specs/experiments/harness | wc -l
git ls-files specs/experiments/harness | grep -c 'plan\.md'
```

Expected: `16`, then `0`. Sixteen tracked documents — two at the parent plus fourteen across the seven domains — and no tracked `plan.md`, which `.gitignore` and `agents/rules/documentation_spec.rules.md` both forbid. An untracked `specs/experiments/harness/plan.md` on disk is fine and expected.

- [ ] **Step 5: Confirm the existing checks still pass**

This project adds markdown under `specs/` only, and neither check reads
`specs/`, so this is a guard against an accidental stray edit rather than an
expected failure.

```sh
meson test -C build --suite scripts --print-errorlogs
git status --short
```

Expected: the scripts suite passes; `git status` shows changes only under
`specs/experiments/harness/`, `specs/experiments/gap.md`, and
`docs/superpowers/`. Anything under `scripts/`, `src/`, or `tests/` violates
the Global Constraints — revert it.

- [ ] **Step 6: Commit any fixes**

```bash
git add specs/experiments/harness/
git commit -m "specs(harness): fix cross-document consistency"
```

If Steps 1-5 all passed with nothing to fix, skip this commit and say so.

---

## Self-Review Notes

Spec coverage checked against `docs/superpowers/specs/2026-07-27-harness-specification-design.md`: location (Task 1-3 paths), layout (Task 11 Step 4 count), domain axis (Task 2 §1), eight stages (Task 2 §2), five contracts (Task 1), artifact envelope (Task 2 §3), env ownership (Task 2 §4), legacy migration (Task 3), the three risks (Task 3 Open Items). The design's `gap.md` reference is resolved to `plan.md` in the "One design-document correction" section above.
