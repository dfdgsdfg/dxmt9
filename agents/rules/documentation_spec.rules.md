---
description: dxmt9 spec authoring rules for requirements, design, DOD ownership, Wine conformance, verification, benchmarks, and gap tracking
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
| `specs/core/` | D3D9 COM/API behavior, state, resources, queries, WSI-facing core |
| `specs/backend/` | Metal translation, command queue, encoder lifecycle, resources |
| `specs/deploy/` | Wine runtime and app-local packaging |
| `specs/tests/` | Test corpus, Wine conformance, deterministic test design |
| `specs/verification/` | TLA+ and implementation evidence |
| `specs/benchmarks/` | Performance workloads, baselines, regression policy |
| `specs/d3d7/`, `specs/d3d8/` | Compatibility shim layers |
| `specs/gap.md` | Current implementation/evidence gap tracker |

Every topic should have `requirements.md` and `design.md` unless it is an
intentional single-purpose support doc such as `specs/gap.md` or
`specs/tests/dod.md`.

`specs/**/plan.md` is local-only and ignored. Do not commit implementation plans.
Promote durable ordering constraints into `design.md`, and missing work into
`specs/gap.md`.

## Requirement IDs

Requirements are written as numbered contracts with stable IDs:

| Area | Prefix |
|------|--------|
| Core D3D9 layer | `R-CORE-*` |
| Metal backend | `R-BACK-*` |
| Tests and conformance | `R-TEST-*` |
| Deployment | `R-DEPLOY-*` |
| Verification / TLA+ | `R-VERIF-*` |
| Benchmarks | `R-BENCH-*` |
| D3D7 / D3D8 shims | `R-D3D7-*`, `R-D3D8-*` |

Rules:

- One requirement should state one observable contract.
- Keep IDs stable. If a requirement is removed, update references in design,
  tests, benchmarks, verification docs, and `specs/gap.md`.
- Use normative language: "must", "must not", "may", "should".
- Avoid vague criteria such as "works correctly", "optimize", or "support later".
- State the compatibility scope: Windows D3D9 behavior, Wine runtime behavior,
  DXMT-compatible architecture, or data-oriented transform design.

## Design Docs

`design.md` explains the chosen architecture for the requirements. It should make
ownership and ordering obvious.

Required design content for non-trivial topics:

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

When specs use Wine-derived behavior:

- Cite the Wine source file and test function when practical.
- Record whether the target is Windows D3D9 API parity, Wine loader/runtime
  compatibility, or deployment behavior.
- Keep Wine conformance manifests separate from native unit tests.
- Do not copy Wine implementation structure into the architecture requirements.
- Preserve license/provenance notes for imported tests, expected values, or
  corpus material.

## Tests and Verification

Tests must map back to requirement IDs or explicit gap rows.

| Evidence | Use for |
|----------|---------|
| Native Meson unit tests | Stateless transforms, packet schemas, descriptors, HRESULT helpers |
| `shader_runner_dxmt9` | GPU-visible shader, texture, geometry, and render-state readback |
| Wine PE conformance executables | Public D3D9 ABI, COM, HRESULT, loader, and window behavior |
| TLA+ / assertions | Queue, resource lifetime, encoder lifecycle, query sequence safety |
| Benchmarks | Bridge-op budget, command batching, throughput, frame timing |
| `specs/gap.md` | Missing, partial, or newly required evidence |

Rules:

- Prefer deterministic unit or fake-backend evidence before runtime-only evidence.
- Queue and bridge behavior should be observable without sleeps or GPU timing.
- Runtime shader probes complement, but do not replace, source/IR and descriptor
  assertions.
- If a requirement cannot be verified yet, add or update a `specs/gap.md` row.

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

Update `specs/gap.md` whenever a spec adds a requirement that is not fully
implemented or not fully evidenced.

Gap rows should include:

- status: implemented, partial, or not started,
- concise current evidence,
- missing evidence or implementation work,
- requirement IDs that own the gap.

Do not hide TODOs in `requirements.md` or `design.md`. Long-lived missing work
belongs in `specs/gap.md`; session-local sequencing belongs in ignored `plan.md`
or a scratchpad.

## Research Notes

Use `researches/{target}.researches.md` only for substantial findings that inform
requirements or design, such as:

- comparison of DXMT, DXVK, D9VK, Wine, or D3D9On12 behavior,
- Wine test review summaries,
- license/provenance analysis,
- platform or Metal behavior investigation.

Each research note should state subject, why it exists, findings, and implications.
Once a finding becomes a hard rule, move the rule into `requirements.md` or
`design.md` and link the research note.

## Workflow

1. Update `requirements.md` when behavior or compatibility scope changes.
2. Update `design.md` when ownership, data layout, ABI, ordering, or failure
   behavior changes.
3. Update tests/verification/benchmarks specs when the acceptance evidence changes.
4. Update `specs/gap.md` for anything partial or missing.
5. Run a focused markdown check such as `git diff --check`.

## Anti-Patterns

- Committing `plan.md`.
- Adding a requirement without an ID.
- Adding a design choice without an owner.
- Treating Wine source structure as mandatory architecture.
- Treating runtime readback as proof of packet-transform correctness.
- Leaving references to helper rules, skills, scripts, or manifests that do not
  exist in the repository.
