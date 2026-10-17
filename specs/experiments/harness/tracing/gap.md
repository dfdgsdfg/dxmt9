---
type: "Spec Gap"
title: "Detailed Performance Tracing Gaps"
description: "Existing observation primitives and missing integrated trace qualification."
tags: [specs, experiments, harness, tracing, gap]
---

# Detailed Performance Tracing Gaps

Owns implementation/evidence status for [requirements](requirements.md) and
[spec](spec.md). This inventory distinguishes existing components from the
integrated contract; source presence is not a fresh runtime qualification.

The [48-requirement evidence matrix](evidence.md) is the authoritative
code/test mapping. Implementation and platform qualification remain distinct.
Native SDK import, deterministic fixtures, and device smoke do not by themselves
qualify an unavailable external collector or establish negligible overhead.

| Contract | Implementation | Qualification and remaining limits |
|---|---|---|
| R-HARN-TRACE-1.1–1.3 | Catalogue profiles, unique attempts, frozen requests, build/source provenance, separate collector outcomes and atomic manifest sealing. | Python positive/negative fixtures pass; final catalogue Wine evidence is recorded in the matrix. |
| R-HARN-TRACE-2.1–2.3 | Process/thread/session incarnations, distinct raw/queue/CB identities and explicit cold-side relation resolution. | Native and Wine x64 GPU captures qualify typed submit-to-CB flows. Broad many-source/shared-CB fan-out remains unqualified; completion membership does not establish exclusive physical GPU contribution. |
| R-HARN-TRACE-3.1–3.4 | Declared clocks, bounded calibration validity, source metrics, unknown populations and legacy adapters. | Native and Wine x64 GPU calibration passes. Wine x86/x64 PE QPC mapping remains uncovered/high-error and fails strict complete attribution; unknown populations remain unknown. |
| R-HARN-TRACE-4.1–4.3 | Cached gates, bounded POD buffers, generation-qualified admission and generated bounded PE transport. | Disabled ARM64 assembly, matched native emitter controls, and x86/x64 PE builds pass. Wine x86/x64 each emit 8 PE records with zero loss, but strict clock qualification remains pending because calibration is missing/uncovered and uncertainty exceeds 1000 ns. |
| R-HARN-TRACE-5.1–5.3 | Immutable raw/derived artifact graph, native protobuf, pinned Trace Processor and fail-closed normalization. | Native and Wine x64 CPU/GPU captures pass pinned TP. Wine x86/x64 PE traces are retained but rejected by complete-attribution checks; zero loss alone is insufficient. |
| R-HARN-TRACE-6.1–6.4 | REA/module/address/ABI/installation validation and owned portable probe lifecycle. | No live target patching adapter is admitted. Static validity never claims activation, hits or state preservation. |
| R-HARN-TRACE-7.3 | Separate optional emulation profile with explicit capability rejection. | QEMU is not installed or qualified as a macOS/Wine performance equivalent. |
| R-HARN-TRACE-8.1–8.3 | Deterministic counterexamples, CPU/GPU lifecycle models, matched OFF/ON controls and frozen budgets. | No speedup or negligible-overhead claim. See exact run scope and limitations in the evidence matrix. |
| R-HARN-TRACE-9.1–9.3 | Private pinned opt-in unix SDK, SDK-free observer facade and deferred original-time native serialization. | Native SDK-off/on and x86_64 unix builds exercised; PE builds reject SDK linkage; public SDK symbols audited. |
| R-HARN-TRACE-9.4, 9.7 | Session generation/admission/drain/seal, atomic receipts, frozen limits and bounded failure cleanup. | Production lifecycle fixtures plus safety/progress and deliberate TLA counterexamples. |
| R-HARN-TRACE-9.5–9.6 | Import/replay/publication/encode/wait/query/submit/completion sites, opt-in detail and batched guest observations. | Final Wine x64 GPU captures resolve engine categories and one detail event; query applicability remains unobserved. Wine x86/x64 each have 8 retained PE events, but uncovered/high-error QPC clocks fail complete attribution. |
| R-HARN-TRACE-9.8–9.9 | Native tracks/relations, loss checks and build/runtime test matrix. | SDK-off tests pass 17/17; SDK-on tests pass 20/20; x86/x64 PE and unix builds pass. Wine x64 CPU, CB+detail and encoder-stage captures pass exact consumer qualification; x86/x64 PE captures remain clock-unqualified. |
| R-HARN-TRACE-10.1–10.2 | Built-in Metal CB timestamps and optional render-stage counter samples with explicit capability/level outcomes. | Wine x64 CB+detail and encoder-stage traces qualify with requested levels attached; unsupported levels still fail explicitly. Completion callbacks remain retirement hints, not GPU timestamps. |
| R-HARN-TRACE-10.3–10.4 | Bounded generation-qualified slots, nonblocking admission, asynchronous polling, quarantine and safe retirement. | GPU model/code tests and intended mutant failures pass; native/Wine GPU captures retire with zero loss. Dedicated live timeout injection remains pending; no unbounded wait or production lifetime authority. |
| R-HARN-TRACE-10.5–10.7 | Source-specific API/tick metadata, actual device clock anchors and GPU track/CB bindings. | Apple M1 matched controls and Wine x64 stage/CB traces pass; GPU source metadata and calibration reach pinned TP. The x86 PE capture does not qualify its QPC mapping. |
| R-HARN-TRACE-11.1–11.3 | Platform-qualified manifests, loaded-site/ABI validation and thread/installation coverage accounting. | Live Linux perf/ptrace and macOS/Wine breakpoint adapters are unavailable and unqualified. |
| R-HARN-TRACE-11.4–11.6 | Portable wrap-safe perf parsing, loss/throttle records, sample/register semantics and attribution exclusions. | Fixtures do not establish live Linux scheduling or macOS target coverage. |
| R-HARN-TRACE-11.7–11.8 | Owned bounded attach/detach state model and fail-closed capability admission. | No live target mutation occurs without an admitted adapter. No perftrace code or mandatory dependency is included. |

## Existing Component Entry Points

- `src/dxmt9/dxmt9_cpu_supply_trace.{hpp,cpp}` and
  `scripts/tools/analyze_cpu_supply.py` (`load`, `pair`).
- `src/dxmt9/dxmt9_wait_trace.{hpp,cpp}` and
  `scripts/tools/summarize_wait_trace.py` (`load`, `summarize`).
- `scripts/tools/join_replay_wait_trace.py` (`join`).
- `src/d3d9/d3d9_pe_thread_sampler.hpp`,
  `scripts/tools/symbolicate_xctrace_pe.py`, and
  `scripts/tools/summarize_guest_pc_window.py` (`summarize`).
- `src/dxmt9/dxmt9_signposts.{hpp,cpp}` and
  `scripts/tools/summarize_xctrace_metal_intervals.py`.
- `tests/scripts/test_cpu_supply_trace.py`,
  `tests/scripts/test_wait_trace.py`, and
  `tests/scripts/test_summarize_xctrace_metal_intervals.py`.
