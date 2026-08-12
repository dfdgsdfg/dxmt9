---
description: dxmt9 spec authoring rules for requirements, spec docs, DOD ownership, Wine conformance, verification, benchmarks, and gap tracking
paths:
  - "specs/**/*.md"
globs: "specs/**/*.md"
alwaysApply: false
---

# dxmt9 Spec Authoring

Specs describe durable behavior, architecture, compatibility contracts, and
verification evidence for dxmt9. They are not work logs, issue trackers, or
implementation checklists.

## Spec Layout

Root `specs/` is multi-topic. Each major concern lives in a topic folder:

| Topic | Role |
|------|------|
| `specs/archicture/` | Project-wide architecture, DOD, provenance, and DXMT merge readiness |
| `specs/d3d9/` | D3D9 COM/API behavior, state, resources, queries, WSI-facing frontend |
| `specs/d3d9/{caps,formats,queries,wsi}/` | D3D9 subtopic contracts and design tables |
| `specs/backend/` | Shared Metal translation, command queue, encoder lifecycle, resources |
| `specs/backend/surface-ops/` | Backend surface copy, fill, scale, and readback contracts |
| `specs/deploy/` | Wine runtime and app-local packaging |
| `specs/tests/` | Test corpus, Wine conformance, deterministic test design |
| `specs/verification/` | TLA+ and implementation evidence |
| `specs/benchmarks/` | Performance workloads, baselines, regression policy |
| `specs/experiments/` | Wild integration experiments and fuzzy pass criteria |
| `specs/d3d7/`, `specs/d3d8/` | Compatibility shim layers |
| `specs/<topic>/gap.md` | Topic-owned implementation/evidence gap tracker |
| `specs/<topic>/log.md` | Topic-owned maintenance history and retired detail |
| `specs/gap.md` | Root gap index and cross-topic rollup |

Every topic should have `requirements.md` and `spec.md` unless it is an
intentional single-purpose support doc such as a gap inventory.

Durable `specs/**/*.md` documents should carry lightweight YAML frontmatter with
`type`, `title`, `description`, and `tags` keys. Keep it descriptive, not a
state tracker. Exclude `specs/README.md`, local-only `*.plan.md`, and
`specs/**/plan.md` files.

`specs/**/plan.md` and `*.plan.md` files are local-only and gitignored. Do not
commit implementation plans. Promote durable ordering constraints into
`spec.md`, missing work into the owning `specs/<topic>/gap.md`. Plan
structure rules live in [Implementation Plans](#implementation-plans) below.

## Requirement IDs

Requirements are written as numbered contracts with stable IDs:

| Area | Prefix |
|------|--------|
| Whole-project architecture | `R-ARCH-*` |
| D3D9 frontend layer | `R-CORE-*` |
| D3D9 caps / formats | `R-CAPS-*`, `R-FORMAT-*` |
| Metal backend | `R-BACK-*` |
| Tests and conformance | `R-TEST-*` |
| Deployment | `R-DEPLOY-*` |
| Verification / TLA+ | `R-VERIF-*` |
| Benchmarks | `R-BENCH-*` |
| Experiments | `R-WILD-*` |
| D3D7 / D3D8 shims | `R-D3D7-*`, `R-D3D8-*` |

Rules:

- One requirement should state one observable contract.
- Keep IDs stable. If a requirement is removed, update references in spec docs,
  tests, benchmarks, verification docs, and the owning gap document.
- Use normative language: "must", "must not", "may", "should".
- Avoid vague criteria such as "works correctly", "optimize", or "support later".
- State the compatibility scope: Windows D3D9 behavior, Wine runtime behavior,
  DXMT-compatible architecture, or data-oriented transform design.

## Spec Docs

`spec.md` explains the chosen architecture for the requirements. It should make
ownership and ordering obvious.

Required `spec.md` content for non-trivial topics:

- **Ownership:** which module owns validation, state, resource lifetime,
  scheduling, and failure reporting.
- **Data boundaries:** PE vs unix, COM vs backend handles, POD wire records vs
  queue-local objects.
- **Runtime ordering:** sequence, state, or flow diagrams for command submission,
  reset/lost-device handling, WSI, query resolution, or provider loading.
- **Failure behavior:** HRESULT propagation, loader failure, unsupported runtime,
  malformed packet, stale handle, or validation error policy.
- **Verification mapping:** which tests, TLA+ models, benchmarks, or manifests
  prove the important requirements.

Mermaid diagrams are encouraged when they show ordering or ownership that prose
would hide. Do not add diagrams that only restate a directory tree.

## DXMT / DOD Rules

Specs that touch the hot path must preserve the DXMT-shaped architecture:

| Owner | Must own |
|------|----------|
| PE D3D9 layer | COM validation, Windows-visible state, getters, state blocks |
| `CommandRecorder` / `CommandChunk` | POD packet construction and retained handle derivation |
| `winemetal` bridge | ABI marshalling only |
| unix importer | packet validation, canonicalization, handle lookup, retention |
| `CommandQueue` | ordered replay, encode/finish threads, seq IDs, frame tokens |
| Presenter | drawable acquisition and present encoding |

Data-oriented requirements:

- Hot-path `Set*`, `Draw*`, and ordinary `Clear` traffic records into chunks.
  It must not regress to one PE/unix call per D3D9 operation.
- Wire records must be POD, versioned, bounds-checked, and free of COM pointers,
  ObjC pointers, unix object pointers, lambdas, and process-local containers.
- State, draw, shader, descriptor, barrier, and retention decisions should be
  expressed as stateless value transforms where possible.
- Runtime probes prove GPU-visible behavior. They do not replace stateless
  transform tests or deterministic queue/bridge observer evidence.
- Any intentional divergence from upstream DXMT should be documented as an ABI,
  ownership, or Wine-boundary reason, not as an unexplained fork.

## Wine Conformance

Wine D3D9 is a behavioral oracle, not an implementation structure requirement.

When specs use Wine-oracle behavior:

- Cite the Wine source file and test function when practical.
- Record whether the target is Windows D3D9 API parity, Wine loader/runtime
  compatibility, or deployment behavior.
- Keep Wine conformance manifests separate from native unit tests.
- Do not copy Wine implementation structure into the architecture requirements.
- Preserve license/provenance notes for external fixtures, expected oracle
  values, or corpus material. Imported third-party files must stay outside the
  MIT-owned project-code scope unless their license is explicitly compatible and
  notices are preserved.

## Tests and Verification

Tests must map back to requirement IDs or explicit gap rows.

| Evidence | Use for |
|----------|---------|
| Native Meson unit tests | Stateless transforms, packet schemas, descriptors, HRESULT helpers |
| `shader_runner_dxmt9` | GPU-visible shader, texture, geometry, and render-state readback |
| Wine PE conformance executables | Public D3D9 ABI, COM, HRESULT, loader, and window behavior |
| TLA+ / assertions | Queue, resource lifetime, encoder lifecycle, query sequence safety |
| Benchmarks | Bridge-op budget, command batching, throughput, frame timing |
| `specs/<topic>/gap.md` | Missing, partial, or newly required evidence |

Rules:

- Prefer deterministic unit or fake-backend evidence before runtime-only evidence.
- Queue and bridge behavior should be observable without sleeps or GPU timing.
- Runtime shader probes complement, but do not replace, source/IR and descriptor
  assertions.
- A spec for a stateful rendering performance lane must classify the change
  against `specs/verification/requirements.md` R-VERIF-1.5–1.8 and record the
  applicable semantic, bounded-refinement, model/code, GPU-oracle, wild, and
  performance layers. Put missing layers in the owning `gap.md` rather than
  treating a later wild run as a substitute.
- If a requirement cannot be verified yet, add or update the owning
  `specs/<topic>/gap.md` row.

## Benchmarks

Benchmark specs must state what regression means and which counters prove the
architecture did not drift.

For DXMT/DOD hot-path work, include:

- logical D3D9 operation count,
- bridge operation count by class,
- chunk commit count,
- compatibility fallback count,
- frame timing or throughput where relevant.

A rendering-correct path can still fail DXMT merge readiness if it regresses to
per-state or per-draw bridge calls.

## Gap Tracking

Update the owning `specs/<topic>/gap.md` whenever a spec adds a requirement that
is not fully implemented or not fully evidenced. Keep `specs/gap.md` as a root
index and cross-topic rollup; do not put detailed status rows there unless no
topic owner exists yet.

Detailed inventory docs may live next to the owning topic gap when a matrix is
too large for the topic overview. Current durable inventories are
`specs/d3d9/gap_d3d9.md` for D3D9 API coverage and
`specs/tests/gap_d3d9_wine_test.md` for Wine D3D9 conformance coverage.

Keep root `specs/gap.md` concise: it should provide only the project-level
overview, domain ownership links, and inventory pointers. Move older detail,
structural migrations, and non-current notes to the owning `specs/<topic>/log.md`
instead of expanding the root overview.

Gap rows should include:

- status: implemented, partial, or not started,
- concise current evidence,
- missing evidence or implementation work,
- requirement IDs that own the gap.

Do not hide TODOs in `requirements.md` or `spec.md`. Long-lived missing work
belongs in the owning topic gap; session-local sequencing belongs in ignored
`plan.md` or a scratchpad.

## Implementation Plans

Plans stage execution for spec-driven work. They are local-only (gitignored) so
they can be regenerated, restructured, or thrown away without polluting the
durable spec history. Their job is to maximize parallel agent dispatch and make
file/state conflicts visible upfront.

### Naming and Location

| Pattern | Use when |
|---------|----------|
| `specs/<topic>/plan.md` | Plan belongs to a single topic with `requirements.md` + `spec.md` (e.g., `specs/backend/draw-uniforms/plan.md`, `specs/verification/plan.md`). |
| `specs/<scope>.plan.md` | Plan is a cross-cutting refactor or staging that does not fit a single topic (e.g., `specs/round2-module-splits.plan.md`). |

Both patterns are matched by `.gitignore`. Do not invent a third naming.

### Required Sections

A plan must contain enough for a fresh agent (no session context) to dispatch
parallel work safely. Every plan needs the following, in this order:

1. **Goal + Architecture** — one paragraph stating what is built and the
   high-level approach. Link to the spec's `requirements.md` / `spec.md` if
   one exists; the plan does not restate them.
2. **Task DAG** — a Mermaid `flowchart` (or `graph TD`) that shows every task,
   its dependencies, and the parallel batches/waves. Use class colours per
   batch so a reader can see the parallel groups at a glance.
3. **File / resource matrix** — either a Mermaid graph showing which files each
   task touches, or a table marking `W` (write) and `r` (read) per file. The
   matrix must prove that no two tasks in the same parallel batch share a `W`.
4. **Per-task scaffolds** — for each task: files affected (create / modify /
   delete / keep frozen), bucket or routing heuristic where relevant,
   step-by-step actions with exact commands or code, verification commands
   (build, focused test target, TLA if applicable), and a commit message.
5. **Verification gates** — per-batch or final acceptance criteria stating
   which builds must pass, which tests must run, and which performance or
   conformance numbers must hold.
6. **Anti-goals / Out of scope** — explicit list of things the plan must not
   do. Prevents scope creep when an agent is dispatched without conversational
   context.

Optional sections that improve large or risky plans:

- **Open issues** — coordination decisions that must be resolved before
  dispatch (e.g., counter-symbol header location, ordering of merges).
- **Sync points** — table of `(trigger event → doc/spec edit)` pairs so
  `gap.md`, `spec.md`, and cross-references stay in step as tracks land.
- **Parallelization protocol** — exact dispatch / merge / cherry-pick sequence
  when worktree-based parallel execution is the recommended path.

### Plan vs. Spec vs. Gap

| Doc | Holds | Lifetime |
|-----|-------|----------|
| `plan.md` | Execution staging, task DAG, file conflict matrix, batch gates | Single round of work; discarded after merge |
| `spec.md` | Durable ownership, ordering, ABI, failure behavior | Permanent |
| `requirements.md` | Numbered contracts with stable IDs | Permanent |
| `gap.md` | Implementation/evidence shortfalls | Active until closed |

If a plan discovers a durable rule (e.g., "PE side never resolves Metal buffer
slot indices"), promote that line into `spec.md` before the plan is
discarded. If it discovers missing evidence, add a `gap.md` row. The plan
itself stays local.

### Mermaid Conventions for Plans

- Use `flowchart TD` (top-down) for task DAGs; `graph LR` only when a strict
  pipeline reads better left-to-right.
- Apply `classDef` per batch or wave with distinct fill colours so parallel
  groups are visually obvious. Example pattern from
  `specs/backend/draw-uniforms/plan.md`:
  ```
  classDef batch1 fill:#fff0d6,stroke:#b26b00,color:#2b1900
  class A1,A2,A3 batch1
  ```
- Node labels should name the file(s) the task writes, not just the task
  number, so the file-conflict claim is checkable from the diagram alone.
- For merge / sync flows, `sequenceDiagram` is appropriate.

### Plan Authoring Workflow

1. Read the topic's `requirements.md` and `spec.md` first; the plan exists
   only to stage their implementation.
2. Decompose into tasks small enough that one agent can finish each in a
   single dispatch (≤ a few files, ≤ a few hours of agent time).
3. Build the file/resource matrix before drawing the DAG — the matrix is what
   tells you which tasks can actually run in parallel.
4. For each task, write the verification command verbatim; agents should not
   have to invent test target names.
5. Run focused builds / tests on the plan's own scaffolding (`git diff
   --check`, link-only Mermaid render check) before dispatching agents.

The `superpowers:writing-plans` skill is the long-form authoring guide; this
section is the dxmt9-specific override for naming, location, and required
sections.

## Research Notes

Use `researches/{target}.researches.md` only for substantial findings that inform
requirements or design, such as:

- comparison of DXMT, DXVK, D9VK, Wine, or D3D9On12 behavior,
- Wine test review summaries,
- license/provenance analysis,
- platform or Metal behavior investigation.

Each research note should state subject, why it exists, findings, and implications.
Once a finding becomes a hard rule, move the rule into `requirements.md` or
`spec.md` and link the research note.

## Workflow

1. Update `requirements.md` when behavior or compatibility scope changes.
2. Update `spec.md` when ownership, data layout, ABI, ordering, or failure
   behavior changes.
3. Update tests/verification/benchmarks specs when the acceptance evidence changes.
4. Update the owning `specs/<topic>/gap.md` for anything partial or missing.
5. Run a focused markdown check such as `git diff --check`.

## Anti-Patterns

- Committing `plan.md` or `*.plan.md`.
- Plans without a Mermaid task DAG (parallelism becomes invisible).
- Plans without a file-conflict matrix (parallel batches become unsafe).
- Plans that restate `requirements.md` or `spec.md` instead of linking.
- Adding a requirement without an ID.
- Adding a design choice without an owner.
- Treating Wine source structure as mandatory architecture.
- Treating runtime readback as proof of packet-transform correctness.
- Leaving references to helper rules, skills, scripts, or manifests that do not
  exist in the repository.
