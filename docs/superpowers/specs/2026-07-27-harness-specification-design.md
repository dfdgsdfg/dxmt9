# Harness Specification — Design

Date: 2026-07-27
Status: approved design, not yet written

## Context

dxmt9 has 86 harness scripts under `scripts/tools`, `scripts/run_apps`,
`scripts/check`, and `scripts/run_suites`. They produce every piece of
performance and correctness evidence the project relies on. **No document in
`specs/` describes any of them.** A grep for `mini_replay` across `specs/`
returns nothing.

That gap has a measured cost. A 2026-07-25/27 vertex-remap locality experiment
was blocked by five independent defects in one harness,
`scripts/tools/run_3dmark05_mini_replay.py`:

| # | Defect | Nature |
|---|---|---|
| 1 | `transform_msl` could not find the `buffer(30)` argbuf parameter | Engine drift: `DXMT9_ARGBUF_DIRECT_CBUF` was promoted default-on (`9eb02437`), so dumped MSL now binds constants directly at `buffer(0)`/`buffer(3)`. The harness still expected the old argbuf signature. |
| 2 | Sliced stream payloads double-counted `stream0_offset` | Boundary ambiguity: the dump writes a slice starting at `stream0_offset`, so payload byte 0 is fetch slot 0. The replay treated the same value as an offset *within* the payload. Latent — every previously used row happened to have `stream0_offset == 0`. |
| 3 | R32F render targets silently fell back to `RGBA8Unorm` | Silent degradation: `color_pixel_format()` handles `core::Format` 1-4 and returns `RGBA8Unorm` for everything else without failing. |
| 4 | Every lane rendered a fully black image and exited 0 | No output validation: the replay reported `draws=229 repeat=1` and success while writing an image with one distinct pixel value. |
| 5 | `--force-fragment-color` failed to compile | Diagnostic path uncovered: returns `float4` from a function whose return type is `FfpFsOut`. |

Defect 2 is the instructive one. Neither side was wrong on its own terms — the
dump was internally consistent, the replay was internally consistent, and no
sentence anywhere connected them. Code review cannot catch that class of
defect; only a written boundary contract can.

Defects 1, 3, 4, and 5 are all inside the `replay` family. Defect 2 sits on the
`probe → replay` boundary.

## Goal

Make harness corrosion detectable by writing down what each harness family owes
its neighbours: which stages it participates in, what it consumes, what it
produces, and which environment variables it owns.

## Non-goals

- No enforcement mechanism in this project. Requirements are phrased as
  machine-checkable predicates so a checker can be added later without
  rewriting them, but no checker is built now. Enforcement is recorded in
  `gap.md`.
- No harness code changes. This project writes specification documents only.
- No new harness. Specifying the fleet does not add to it.
- No migration of existing artifacts. The legacy envelope's removal is planned,
  not performed.

## Location and layout

`specs/experiments/harness/`, a sibling of the existing
`specs/experiments/runtime/` (which covers Wine roots, prefixes, and external
app installs). Harnesses support experiment runs, so they belong to the
`experiments` domain.

`specs/verification/` was considered and rejected: it is entirely about TLA+
formal verification (TLC runs, module structure, refinement mappings), and
placing experiment tooling there would invite readers to conflate `R-VERIF-*`
concurrency proofs with harness contracts.

```
specs/experiments/harness/
  requirements.md      R-HARN-* contracts binding every domain
  spec.md              8-stage boundary map, artifact envelope, env ownership
  plan.md              rollout order and legacy migration
  runner/    {requirements,spec}.md
  probe/     {requirements,spec}.md
  replay/    {requirements,spec}.md
  reduce/    {requirements,spec}.md
  join/      {requirements,spec}.md
  gate/      {requirements,spec}.md
  audit/     {requirements,spec}.md
```

Seventeen files. The two-file subdirectory shape matches the repository's
dominant convention — six of the seven existing spec subdirectories carry
exactly `requirements.md` + `spec.md`; only `specs/backend/draw-uniforms` adds
a `plan.md`, and it has an active rollout.

## Domain axis

Domains follow **harness families**, not pipeline stages. A stage-based split
would scatter one script's contract across several directories:
`run_3dmark05_perf_probe.sh` alone spans run, capture, dump, and reduce, and
there would be no single answer to "who owns this script's contract". The
family axis gives each script exactly one owning domain.

The stage view is preserved as the boundary map in the parent `spec.md`, and
each domain spec declares which stages it participates in.

| Domain | Owns | Stages |
|---|---|---|
| `runner` | `run_experiment.py`, catalogue launchers, `scripts/run_apps/`, `scripts/run_suites/` | build/stage → run |
| `probe` | `run_3dmark05_perf_probe.sh`, `run_with_wine_metal_capture_layer.sh`, `run_3dmark05_system_trace_sidecar.sh` | run/capture → dump extract |
| `replay` | `build_3dmark05_mini_replay_manifest.py`, `plan_3dmark05_mini_replay.py`, `run_3dmark05_mini_replay.py` | dump → manifest → offline replay |
| `reduce` | `summarize_*` that read dxmt9's own logs, such as `summarize_3dmark05_perf.py` | log → CSV |
| `join` | `finalize_3dmark05_perf_probe.sh`, and the `summarize_*` that read external-tool exports — `summarize_xcode_encoder_counters.py`, `summarize_xctrace_metal_intervals.py` | external-tool join |
| `gate` | `compare_*`, `analyze_xcode_replay_variance.py` | comparison and proof gates |
| `audit` | `scripts/check/*` | record verification |

## The eight stages

```
build/stage → run/capture → dump extract → log reduce
            → offline replay → external join → compare/gate → record
```

Seven boundaries. Each gets an explicit "what crosses here" entry in the parent
`spec.md`, naming the artifact, its schema, and its interpretation rules.

## Cross-cutting contracts

Five `R-HARN-*` requirement groups, each derived from an observed defect and
each stated as a predicate something could later check.

**No silent degradation.** A harness must produce a valid artifact or exit
non-zero. It must not substitute a fallback for unsupported input. Where a
fallback is genuinely wanted it must be an explicit opt-in flag, and the
resolved choice must be recorded in the artifact envelope.
*Predicate:* every input-classification branch that selects a fallback is
reachable only behind a named opt-in, or exits non-zero.
*Derived from defect 3.*

**Output validity self-assertion.** A harness that produces a measurement
artifact must assert the artifact is non-degenerate before reporting success,
and must record the assertion result. A harness that cannot assert validity
must fail rather than report success.
*Predicate:* the envelope carries a `validity` field; the producing stage exits
non-zero if it cannot populate it.
*Derived from defect 4.*

**Boundary semantics are declared, not inferred.** An artifact crossing a
domain boundary must carry, in band, the information needed to interpret its
bytes. Offsets, strides, slice origins, and index bases are interpretation
rules, not incidental metadata. Consumers compute from declared fields rather
than assuming a convention.
*Predicate:* for every byte-payload artifact, the envelope names the coordinate
system of every offset it carries.
*Derived from defect 2.*

**Engine-shape dependencies are pinned and detectable.** A harness that
pattern-matches engine output — MSL signatures, log line formats, CSV column
names — must declare the shape it expects in its domain spec, and its failure
message must name that expectation.
*Predicate:* every pattern match against engine output has a corresponding
declared expectation in the owning domain spec.
*Derived from defect 1.*

**Diagnostic paths carry the primary contract.** Diagnostic and secondary modes
are bound by the same contracts and the same coverage requirement as the
primary path.
*Predicate:* every flag that alters harness output appears in the domain spec's
mode table.
*Derived from defect 5.*

## Artifact envelope

Every harness artifact directory carries one manifest:

| Field | Content |
|---|---|
| `schema` | Schema name and version |
| `producer` | Script path and git revision |
| `stage` / `domain` | Which stage and domain produced it |
| `inputs` | Paths and digests of consumed upstream artifacts |
| `env_snapshot` | Resolved values of the contract-relevant environment variables actually in effect |
| `validity` | Result of the validity assertion |

`inputs` digests have a second use. During a 2026-07-27 disk cleanup, 34 of the
56 log paths cited as `source:` evidence in `docs/perfomance/` were found
already missing — citations that had been dangling silently. Digested inputs
make that state machine-detectable.

**Legacy.** The existing ad-hoc envelope fields are legacy and may be removed.
One caveat governs the migration: `result.json` is not purely an envelope. Its
counter payload is consumed by `run_experiment.py`'s `expected_counters` L3
gate, by `compare_3dmark05_perf_counters.py`, and by documentation citations.
The spec therefore separates the envelope from the counter payload, defines the
envelope afresh, and records legacy field removal as a migration step in
`plan.md` rather than presenting it as already done.

## Environment variable ownership

A variable is **contract-relevant** when its value changes either what a
measurement means or how a downstream artifact must be interpreted. A knob that
only affects logging verbosity is not; `DXMT9_ARGBUF_DIRECT_CBUF`, which
changes the emitted MSL signature that `replay` pattern-matches, is.

- Each contract-relevant variable has exactly one owning domain that may set
  it. Downstream domains may read but not set.
- Any variable a harness forwards must appear in `env_snapshot`.
- A contract-relevant variable must not be silently defaulted; the resolved
  value is recorded whether it came from the caller, the profile, or the
  engine default.

This does not duplicate `agents/rules/environment_variables_*.rules.md`. Those
files are the catalogue — which knobs exist and what they do — and they state
their own boundary: "These files are **descriptive**, not a behavioral spec —
for that, see `specs/`." The harness spec supplies the contract: who owns each
knob, who forwards it, and what must never be silently defaulted.

## Testing

No executable tests in this project — it produces specification documents.

Two existing checks constrain the work and must keep passing:
`scripts/check/audit_perf_docs_sources.py` (registered as a Meson test) and the
`scripts` Meson suite. Neither reads `specs/`, so the risk is low, but the
`audit` domain spec must describe `audit_perf_docs_sources.py` accurately
enough that a future reader can tell what it does and does not check — it
verifies that a leaf carries a `source:` field and does not cite the retired
`specs/perfomance.plan.md`; it does **not** verify that cited paths exist.

## Risks

**Documents corrode too.** This was raised during design and the docs-only
scope was chosen deliberately. The mitigation is to phrase every requirement as
a predicate, so adding a checker later is implementation work rather than a
rewrite. `gap.md` records enforcement as the open item.

**Seventeen files is a large surface to keep current.** The domain specs are
deliberately narrow — each covers one family's scripts, stages, modes, and env
ownership — so a change to one harness touches one file.

**The replay domain is specified against a harness that does not currently
work.** Four of the five defects remain unfixed, and the cause of the black
output is still unknown. The `replay` spec must describe the intended contract
and record the known-broken state in `gap.md` rather than describing current
behaviour as correct.
