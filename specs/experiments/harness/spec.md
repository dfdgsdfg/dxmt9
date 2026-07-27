---
type: "Spec"
title: "Harness Spec — Stages, Artifacts, Environment Ownership"
description: "Harness stage boundary map, artifact envelope, env ownership, and legacy envelope migration."
tags: [specs, experiments, harness, spec]
---

# Harness Spec — Stages, Artifacts, Environment Ownership

Implements the requirements in
`specs/experiments/harness/requirements.md`. Defines the eight
pipeline stages, the boundaries between them, the artifact envelope
every cross-boundary artifact carries, and the rule that decides
which domain owns a contract-relevant environment variable. Seven
domain documents under `specs/experiments/harness/<domain>/`
instantiate what this file defines; they cite stage names, boundary
names, and envelope fields from here rather than redefining them.

Evidence for §2's boundary claim and §3's second `inputs` use is
`traces/app-d3d9-3dmark05-vertexremap-dump-r1/analysis/geometry/*.meta`
(156 files, captured 2026-07-25/27 during the vertex-remap
experiment referenced in `requirements.md`).

---

## 0. The Eight Stages

Every harness script belongs to one or more of exactly eight pipeline
stages, named here and used verbatim in every domain document:

`build-stage`, `run-capture`, `dump-extract`, `log-reduce`,
`offline-replay`, `external-join`, `compare-gate`, `record`.

---

## 1. Domain Map

R-HARN-1.1 groups the 86 scripts in scope into seven domains. This
table states, per domain, the scripts it owns and the stages it
participates in:

| Domain | Owns | Stages |
|---|---|---|
| `runner` | `scripts/run_apps/run_experiment.py`, the catalogue launchers under `experiments/launchers/`, `scripts/run_apps/*.sh`, `scripts/run_suites/*` | `build-stage`, `run-capture` |
| `probe` | `scripts/tools/run_3dmark05_perf_probe.sh`, `scripts/tools/run_with_wine_metal_capture_layer.sh`, `scripts/tools/run_3dmark05_system_trace_sidecar.sh` | `run-capture`, `dump-extract` |
| `replay` | `scripts/tools/build_3dmark05_mini_replay_manifest.py`, `scripts/tools/plan_3dmark05_mini_replay.py`, `scripts/tools/run_3dmark05_mini_replay.py` | `offline-replay` |
| `reduce` | the `scripts/tools/summarize_*` that read dxmt9's own logs, including `summarize_3dmark05_perf.py`, `summarize_index_cache_runtime.py`, `summarize_framegraph_dag.py` | `log-reduce` |
| `join` | `scripts/tools/finalize_3dmark05_perf_probe.sh`, `scripts/tools/summarize_xcode_encoder_counters.py`, `scripts/tools/summarize_xctrace_metal_intervals.py`, `scripts/tools/summarize_xctrace_cpu_threads.py` | `external-join` |
| `gate` | `scripts/tools/compare_*`, `scripts/tools/analyze_xcode_replay_variance.py` | `compare-gate` |
| `audit` | `scripts/check/*` | `record` |

**`reduce` versus `join`.** A summariser belongs to `reduce` when its
input is a dxmt9-produced log — a `[dxmt9-perf*]` stderr line, a
`result.json`, a `DXMT9_RENDERER_DUMP_DAG` JSON file. A summariser
belongs to `join` when its input is an external tool's export — an
Xcode "Export Encoder Counters" CSV, an `xctrace` XML/TSV export.
The distinction is the origin of the input artifact, not the shape of
the output: both domains emit CSV/Markdown summaries, and both may
read a dxmt9 log as a *secondary* input (`join` reads a dxmt9 log
alongside the external export to attribute rows), but only `join`'s
*primary* input crosses the `external-join` boundary from a tool
dxmt9 does not control.

**Why the domain axis is harness families, not stages.** A single
script commonly spans several stages —
`run_3dmark05_perf_probe.sh` spans `run-capture` (launches Wine and
the app), `dump-extract` (writes `.gputrace` / geometry dumps), and
`log-reduce` (invokes `summarize_3dmark05_perf.py`-family output
inline). A stage-based split would cut this one script across three
owning documents and leave no single owner accountable for its
contract as a whole. The domain axis groups scripts by the family
that owns their contract; §2 below is what states, stage by stage,
what crosses each boundary regardless of which domain's script
produced or consumed it.

---

## 2. Stage Boundary Map

Each boundary below is named by the pair of stages it joins. Per
R-HARN-4.1, every artifact crossing a boundary carries in-band the
information its consumer needs; per R-HARN-4.2, every offset, stride,
slice origin, and index base names its coordinate system. A domain
document may add boundary detail specific to its own artifacts, but
must not contradict the rule stated here for the boundary it touches.

The eight stages (§0) do not form a single linear chain, which is why
eight boundaries are named below rather than the seven a strict
build-stage-to-record chain would need. The graph branches twice:
`dump-extract` fans out to two consumers
(`offline-replay` and `log-reduce`, because geometry-payload dumps
and dxmt9 log/`result.json` output are independent artifacts written
at the same stage), and `compare-gate` fans in from two producers
(`log-reduce` and `external-join`, because a gate may compare a
dxmt9-only summary or an Xcode-joined summary, or both). Every one of
the eight stages named in §0 is the source or destination of at least
one boundary below, including the terminal `compare-gate → record`
boundary — `record` is not left unreached.

### `build-stage → run-capture`

What crosses: staged build directories (`build-x86_64-builtin`,
`build-win32-x64-builtin`, `build-win32-x86-builtin`) and the Wine
manifest entry resolved per `specs/experiments/runtime/spec.md` §6.
The consumer (a `runner`-domain launcher) must not assume a build
directory is fresh; per `agents/rules/test_wild.rules.md`'s
diagnostic checklist item 3, a stale staged directory has previously
been mismatched against `build/` for a 32-bit binary. The coordinate
system here is "which staged directory", not a byte offset — R-HARN-4.2
does not apply to this boundary's fields.

### `run-capture → dump-extract`

What crosses: the running process's log stream (stderr,
`DXMT_LOG_PATH`), the `.gputrace` bundle when
`DXMT_METAL_CAPTURE_FRAME`/`PATH` fired, and any `DXMT9_DUMP_*`
sidecar files (geometry payloads, depth attachment dumps). The
consumer must not read GPU-side content out of a capture layer that
was never inserted (`agents/rules/metal_debugging.rules.md` §1); a
`probe`-domain script's file-capture preflight is the in-band signal
that the layer is present, not an assumption from the command line
used to launch it.

### `dump-extract → offline-replay`

This is the boundary that failed silently (R-HARN-4.3, R-HARN-4.4,
defect 2) and is stated precisely here because every other boundary
in this document builds on the same discipline.

**What crosses:** per-draw geometry payload files under
`analysis/geometry/*.bin` plus a sidecar `.meta` file per payload,
written by the `probe`-domain dump path
(`DXMT9_DUMP_INDEXED_GEOMETRY_DIR` and related knobs, documented in
`agents/rules/environment_variables_capture.rules.md`). The `.meta`
file is a flat `key=value` text format; confirmed fields relevant to
this boundary, taken verbatim from
`traces/app-d3d9-3dmark05-vertexremap-dump-r1/analysis/geometry/seq60-enc0-draw32159-slot117.meta`:

```
stream0_offset=3384
stream0_stride=24
stream0_start_byte=3384
stream0_byte_count=137184
```

**The slice rule.** The `.bin` payload is a **slice** of the source
D3D9 stream0 vertex buffer, not the whole buffer. The slice is
written starting at `stream0_start_byte` — i.e. the producer reads
`stream0_byte_count` bytes from the source buffer beginning at byte
offset `stream0_start_byte`, and writes those bytes starting at
payload byte 0. Consequently:

- Payload byte 0 corresponds to source-buffer byte
  `stream0_start_byte`, which is fetch slot 0 of the draw.
- `stream0_offset` is a field of the *source* coordinate system — the
  D3D9 stream binding's byte offset into the app's original vertex
  buffer — not an offset already relative to the sliced payload.
- A consumer computing "where in this payload does fetch slot N's
  data begin" must compute the in-payload offset as
  `stream0_offset - stream0_start_byte` (plus `N * stream0_stride`),
  **never** `stream0_offset` directly. Using `stream0_offset` as a
  payload-relative offset double-counts the slice origin and produces
  a negative or out-of-range in-payload offset whenever
  `stream0_offset != 0`.
- Verified against the real dump: in every one of the 156 `.meta`
  files under `.../vertexremap-dump-r1/analysis/geometry/`,
  `stream0_start_byte == stream0_offset`, including all 8 rows with a
  non-zero offset (checked values: `840`, `3384`, `92760`, `185760`,
  `186696`, `272760`, `280320`, `289728` — each row's `start_byte`
  equals its `offset` exactly). This equality is a property of how
  the dump producer chose to slice (it slices from the stream
  binding's own offset, so `stream0_start_byte` and `stream0_offset`
  happen to coincide for this producer), **not** a guarantee a
  consumer may assume without reading `stream0_start_byte` — a future
  producer that slices from a different origin (e.g. a fixed 4KiB
  page boundary) would break that equality while leaving the
  `offset - start_byte` derivation correct. R-HARN-4.3 requires the
  consumer to derive from the declared field, not from an observed
  coincidence.

**Why neither side was wrong (R-HARN-4.4).** The dump producer's
slice arithmetic — write `stream0_byte_count` bytes starting at
`stream0_start_byte` — is internally correct: it is a valid, minimal
slice of the accessed byte range. `run_3dmark05_mini_replay.py`'s
consumer arithmetic — index into the payload using `stream0_offset`
as a payload-relative base — is also internally correct *for a
payload that started at source-buffer byte 0*. Each side reviewed in
isolation passes review. The defect existed only in the relationship
between the two: no artifact field told the consumer which coordinate
system `stream0_offset` was expressed in, so the consumer chose the
wrong one, and the choice happened to be unobservable for months
because every previously exercised row had `stream0_offset == 0`,
the one value where "payload-relative" and "source-relative minus
slice origin" are numerically identical. The fix per R-HARN-4.2/4.3
is not "pick the right convention" but "the artifact must declare the
convention" — which `stream0_start_byte` already does; the missing
piece was the consumer's derivation rule, now stated above.

### `dump-extract → log-reduce`

What crosses: dxmt9's own stderr/log-file lines (`[dxmt9-perf]`,
`[dxmt9-perf-encoder]`, `[dxmt9-perf-indexed-probe-draw]`, PE
recorder stats lines) and `result.json`. The consumer parses a
documented line prefix and field grammar; per R-HARN-5.1/5.2, a
`reduce`-domain script's own `spec.md` states the exact line shapes
it depends on, and a parse failure names which line/field did not
match rather than silently skipping the line.

### `offline-replay → external-join`

What crosses: a `.gputrace` bundle produced by
`run_3dmark05_mini_replay.py`'s `DXMT9_MINI_REPLAY_CAPTURE_PATH`, or
a color-output image at `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH`, for a
downstream `join`-domain tool (Xcode, `xctrace`) to open or diff. The
consumer must not treat the presence of the file alone as proof of
content (R-HARN-3.1); the producer's `validity` field (§3) is what a
downstream harness reads before trusting the bundle.

### `log-reduce → compare-gate`

What crosses: `reduce`-domain summary CSV/Markdown output — for
example `summarize_3dmark05_perf.py`'s per-frame breakdown — consumed
by `gate`-domain comparison scripts such as
`compare_3dmark05_perf_counters.py`. Column names and units are the
in-band contract; a `gate` script must fail on an unrecognized or
missing column rather than silently treating it as zero
(R-HARN-2.1/2.2).

### `external-join → compare-gate`

What crosses: the joined Xcode/`xctrace`-plus-dxmt CSV/Markdown output
of `join`-domain scripts (`finalize_3dmark05_perf_probe.sh`'s joined
summary, `summarize_xcode_encoder_counters.py`'s output) consumed by
`gate`-domain proof gates (`--require-*` flags,
`compare_xcode_dxmt_bottlenecks.py`). Per R-HARN-3.4, a `gate` script
comparing two joined artifacts for agreement must consult each
artifact's `validity` field before treating matching values as a
pass — a degenerate joined row (zero Xcode counter coverage, zero
dxmt join coverage) must not silently satisfy a gate.

### `compare-gate → record`

This is the terminal boundary. What crosses is not a new measurement
artifact but an **evidence citation**: the `gate`-domain script's
pass/fail verdict, plus the specific upstream artifact paths and
content digests (§3's `inputs` field, carried forward from whichever
artifacts the gate consumed) that a `docs/perfomance/` leaf
document's `source:` field will go on to name. The `audit`-domain
consumer — `scripts/check/audit_perf_docs_sources.py` — is what makes
this boundary checkable: it reads a `source:` citation and confirms
the cited path still resolves to an artifact whose digest matches
what was recorded at citation time, rather than only confirming the
path exists. This is the mechanism behind §3's "second use of
`inputs` digests": a citation with no recorded digest cannot
distinguish "never produced" from "produced, then the file moved or
was cleaned up" from "a different file now happens to sit at that
path", and the 34-of-56 missing-path finding cited in §3 is exactly
the failure mode this boundary exists to make detectable. Unlike the
other seven boundaries, the `record` stage does not hand its output
to a further pipeline stage; §1 assigns it to the `audit` domain
because auditing a citation is where the chain ends.

---

## 3. Artifact Envelope

Every artifact that crosses a stage boundary (§2) carries an envelope
with the following fields, in addition to its measurement payload:

| Field | Content |
|---|---|
| `schema` | Schema name and version string |
| `producer` | Script path and git revision |
| `stage` | One of the eight stage names (§0) |
| `domain` | One of the seven domain names (§1) |
| `inputs` | Consumed upstream artifacts: path and digest per entry |
| `env_snapshot` | Resolved values of contract-relevant environment variables actually in effect |
| `validity` | Result of the `R-HARN-3.*` validity assertion |

`validity` is the field R-HARN-3.2 requires: it holds the result of
the non-degeneracy assertion R-HARN-3.1 mandates before a producer
reports success, and R-HARN-3.4's compare-gate consultation reads
this field rather than re-deriving validity from the payload.

**The envelope is provenance, separate from measurement payloads.**
It answers "where did this artifact come from and can it be trusted",
not "what did dxmt9 measure". The payload — an FPS number, a GPU-time
counter, a rendered image, a geometry slice — is untouched by this
spec; only the provenance wrapper around it is standardized.

**Concrete consequence for `result.json`.** `result.json`'s counter
payload is consumed today by three independent readers:
`run_experiment.py`'s `expected_counters` L3 gate,
`compare_3dmark05_perf_counters.py`, and `source:` citations in
`docs/perfomance/`. This spec does not touch that payload or those
readers. Adopting the envelope means adding the seven fields above
alongside the existing counter payload — `result.json` gains
`schema`/`producer`/`stage`/`domain`/`inputs`/`env_snapshot`/`validity`
as a wrapper, or a sibling sidecar file referencing it, but its
counter fields and the three readers above are unchanged.

**Second use of `inputs` digests: detecting missing evidence.** On
2026-07-27, an audit of `docs/perfomance/` `source:` citations found
34 of 56 cited log/output paths already missing from disk. A
citation that names a path but carries no digest cannot distinguish
"the file was never produced" from "the file existed and was later
cleaned up" from "a different file now sits at that path". Recording
a digest per `inputs` entry at the time an artifact is produced makes
that distinction machine-detectable after the fact: a later audit can
tell whether a cited path is missing-and-was-once-present (digest on
record, file absent) versus missing-and-never-existed (no digest on
record for that path at all), instead of relying on a human noticing
a stale citation.

---

## 4. Environment Ownership

**Contract-relevant, defined.** An environment variable is
contract-relevant when its value changes what a measurement means or
how a downstream artifact must be interpreted — as opposed to merely
changing how verbosely the harness describes what it is already
doing. A logging-verbosity knob such as `DXMT_LOG_LEVEL` is not
contract-relevant: turning it up or down does not change what a
counter, image, or geometry payload means, only how much the harness
narrates producing it. `DXMT9_ARGBUF_DIRECT_CBUF` is contract-relevant:
it changes the emitted MSL cbuf-binding signature (`buffer(30)`
argument-buffer parameter present or absent) that the `replay` domain
pattern-matches against, which is exactly how defect 1 (R-HARN-5.3)
arose — the variable's promotion to default-on changed what a
downstream artifact (translated MSL source) looked like, and the
harness that depended on the old shape had no declared dependency to
check against.

**Rule 1 — single owning domain.** Exactly one domain (§1) may *set*
each contract-relevant variable for a given run. Every other domain
in that run's pipeline may read the resolved value but must not set
it. This prevents two stages from silently disagreeing about which
value is in effect — the failure mode R-HARN-4.4 generalizes beyond
byte offsets to any contract-relevant knob.

**Rule 2 — forwarded variables appear in `env_snapshot`.** Any
contract-relevant variable that a domain forwards into a subprocess
it launches, or that it reads to decide its own behavior, is recorded
in that artifact's `env_snapshot` (§3) with its resolved value —
not its unresolved/unset state, and not merely its name. A downstream
consumer reads `env_snapshot` to know which contract shape the
artifact was produced under, mirroring how
`agents/rules/environment_variables_*.rules.md` already documents
each variable's default and resolution but does not, today, record
what was *actually resolved* for one specific artifact.

**Rule 3 — no silent default.** A contract-relevant variable's
resolved value is never silently defaulted; it is recorded in
`env_snapshot` whether it came from the caller's explicit env, from
`DXMT_EXPERIMENT_PROFILE`'s profile substitution (see
`agents/rules/environment_variables_wine.rules.md`), or from the
engine's own compiled-in default (as `DXMT9_OFFLOAD_COMMIT_REPLAY`
and `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE` are today, per
`agents/rules/environment_variables_bridge.rules.md`). Per R-HARN-2.4,
recording only that "a value was in effect" without recording *which*
source resolved it would itself be a silent degradation of the
provenance the envelope exists to provide.

---

## 5. Legacy Envelope Migration

Today's provenance is spread across two ad-hoc, per-app-family
shapes with no shared schema:

- **`result.json`**, written by the `runner` domain
  (`scripts/run_apps/run_experiment.py`), mixes the counter payload
  three independent readers depend on — `run_experiment.py`'s own
  `expected_counters` L3 gate, `compare_3dmark05_perf_counters.py`,
  and `docs/perfomance/` `source:` citations — with provenance
  fields such as `wine_root` that were added over time without a
  declared schema (`agents/rules/test_wild.rules.md`'s diagnostic
  checklist already depends on `result.json:wine_root` informally).
- **`3dmark05-trace-artifacts.json`**, written by the `probe` domain
  (`scripts/tools/run_3dmark05_perf_probe.sh` and
  `scripts/tools/finalize_3dmark05_perf_probe.sh`) as a manifest of
  capture and analysis artifact paths for one probe run, in a shape
  unrelated to `result.json`'s provenance fields.

The migration to the artifact envelope (§3) is ordered in three
steps:

1. **Add.** The envelope's seven fields (`schema`, `producer`,
   `stage`, `domain`, `inputs`, `env_snapshot`, `validity`; §3) are
   added alongside the existing ad-hoc fields in both files — either
   as a wrapper around the existing shape or as a sibling sidecar
   file that references it.
2. **Move.** Consumers that today read the ad-hoc provenance fields
   are moved to read the corresponding envelope fields instead.
3. **Remove.** Once no consumer reads an ad-hoc provenance field,
   that field is removed from `result.json` and
   `3dmark05-trace-artifacts.json`.

**The counter payload is not provenance and is not migrated.**
`result.json`'s counter fields — the values `expected_counters`,
`compare_3dmark05_perf_counters.py`, and `docs/perfomance/`
`source:` citations actually read as measurements, as opposed to the
`wine_root`-style provenance fields above — are untouched at every
step of this migration. Step 3 removes only the ad-hoc provenance
fields the envelope replaces; it does not touch, rename, or
restructure the counter payload those three readers consume.

**No migration step has been performed.** As of this writing,
neither `result.json` nor `3dmark05-trace-artifacts.json` carries
any envelope field. This section states the intended migration path;
it does not claim any part of it is done. A reader who finds an
`env_snapshot` or `validity` field in a live `result.json` should
treat that as evidence the migration has since started elsewhere,
not as something this document produced.
