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

| Contract | Status | Current evidence | Missing implementation or evidence |
|---|---|---|---|
| R-HARN-TRACE-1.1–1.3: run/profile lifecycle | partial | Existing catalogue runner, staged-build provenance, perf-probe and system-trace sidecar. | Shared tracing profile/manifest, per-collector required/optional outcomes, retry/seal and unified claim gates. |
| R-HARN-TRACE-2.1–2.3: identity/relations | partial | CPU supply replay-ledger bindings and `join_replay_wait_trace.py`; Metal `seq/encoder` joins. | Shared incarnation schema, PE/native mapping, many-to-many submission relations and cross-source adversarial fixtures. |
| R-HARN-TRACE-3.1–3.4: clocks/metrics | partial | Existing CPU/wall distinction and guest-PC restrictions. | Reusable calibrated clock adapter, uncertainty propagation, GPU mapping and source-specific coverage qualification. |
| R-HARN-TRACE-4.1–4.3: observer boundary | partial | Existing bounded CPU supply/wait collectors and project observer rules. | Integrated collector disabled/enabled cost evidence, bounded lifecycle model/code binding and diagnostic transport qualification if added. |
| R-HARN-TRACE-5.1: bundle/envelope | not started | Existing raw artifacts and parent envelope design are inputs. | Versioned trace bundle, source-digest graph, normalized schema and compatibility fixtures. |
| R-HARN-TRACE-5.2: Perfetto | not started | Engine integration is selected; no Perfetto exporter/SDK integration found in the inspected runtime/tools. | Engine-produced native protobuf and pinned consumer round trip; legacy JSON conversion is a compatibility adapter. |
| R-HARN-TRACE-5.3: integrated validity | partial | `analyze_cpu_supply.py` and `summarize_wait_trace.py` reject incomplete native inputs; existing external joins enforce coverage. | Unified loss/closure/clock/identity qualification and nonzero required-claim failures. |
| R-HARN-TRACE-6.1: REA evidence | partial | [REA rule](../../../../agents/rules/rea.rules.md) defines static evidence provenance. | Machine-readable static annotation/probe manifest linked to runtime run IDs and source records. |
| R-HARN-TRACE-6.2–6.4: target probes | partial | PE sampler provides guest-context observations; target-specific probe experiments do not qualify a general injector. | Reusable target-probe backend and manifest validator; supported ABI/lifecycle matrix, actual-hit joins and matched controls. |
| R-HARN-TRACE-7.3: QEMU | not started | Separate structural lane specified; no runtime-performance equivalence asserted. | Only if needed: pinned emulator/mode, fixtures and artifact adapter. Not an initial Perfetto dependency. |
| R-HARN-TRACE-8.1–8.3: acceptance | partial | Existing workload rules separate diagnostics from baseline timing. | Integrated deterministic counterexamples, observer budgets/controls, pinned-viewer checks and bounded qualified-runtime run. |
| R-HARN-TRACE-9.1–9.3: SDK/build boundary | not started | Private unix ownership and bounded POD capture are specified. | Pin SDK release/digest/license; Meson opt-in, private SDK module, SDK-free facade, native serialization and disabled-path verification. |
| R-HARN-TRACE-9.4, 9.7: session/control | not started | Required state machine and engine receipt are specified. | Generation/admission/drain model and code binding, bounded failure cleanup, capture configuration and runner receipt handling. |
| R-HARN-TRACE-9.5–9.6: engine/PE events | partial | Existing CPU supply/wait hooks provide candidate event sites. | Native track/flow adapters, event coverage and no-duplication checks; qualified optional PE transport and clock/thread mapping. |
| R-HARN-TRACE-9.8–9.9: integration acceptance | not started | Native-output and build/runtime acceptance matrix are specified. | Trace Processor fixtures, SDK loss checks, session race counterexamples, SDK coexistence and native/Wine x86/x64 observer controls. |

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
