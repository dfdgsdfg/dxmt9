---
domain: present-pacing
workload: STALKER H502/r5 original input, actual ordinary batch preparation
title: "Exact-prefix state comparison preserves actual-call-site rendering effects"
type: evidence
status: actual-call-site-effects-qualified-live-transfer-separate
updated: 2026-10-16
source: traces/stkcop-pair-production-h607/{contract.md,candidate.hpp,gate.hpp,prepare.py,derivation.json,predicate_native.cpp,build.py,build.json,preflight.py,preflight.json,verify.py,verifier-correction.md,verification.json,production-call-site.asm}; traces/stkcop-pair-production-h607/original-gate{0,1}-perf{0,1}.log; traces/stkcop-pair-production-h607/queue-gate{0,1}-perf0.log
related: present-pacing-pair-shortcut.606.md; present-pacing-generation-cpu.602.md; present-pacing-pair-live.608.md
---

# Candidate now drives actual submission, not only scratch output

H607 connects H606's pure exact-prefix render-state comparison to the real
`src/dxmt9/dxmt9_command_queue.cpp`, `CommandQueue::submitDrawRunBatch`
preparation loop. Only this predicate changes; layout equality, preparation
clocks, queue order, capacity, resource marking and ownership remain unchanged.
The private image derives from H602; only its command-queue archive member is
replaced. Shared inline headers and canonical source/build artifacts are not
modified. H605/H606 deferred capture and scratch measurement are absent.

Private `H607_GATE` selects the candidate once per batch; `H607_WITNESS` logs
reach only in effect controls. These are not product runtime knobs. Native
ARM64 and x86_64 rerun H606's 1,468,900 render-state and 50,000 full-predicate
checks. Pure equality refinement changes no scheduling/lifecycle transition;
the existing bounded/property tests apply without a new interleaving model.

## Fixed effect controls

Four original-capture controls cover gate OFF/ON with product counters OFF/ON.
Each ON control executes 2,259 candidate pairs in actual submission and retains:

- Exact original source and output SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
- Seven source boundaries, query issues/polls/results/readiness, 670 balanced
  wrapper lifetimes and normal-state restoration.
- Functional enabled payload-clock diagnostics and no perf report when OFF.

Two ordinary API-validation controls preserve route/completion/readbacks with
draws skipped. They execute no candidate pair and are **not** candidate pixel
coverage. The four original controls supply actual rendered candidate coverage.
No failed controls or replacement runs occur.

Independent verification checks source reversal back to the exact H602 file,
candidate equality to H606, native results, image/effect hashes and disassembly
calls to both predicates inside the actual production-shaped batch function.
An initial symbol selector also matched four `.cold.N` outlines; excluding
those fixes verifier entry selection, without changing any runtime or evidence.

No CPU/FPS saving follows from these correctness controls. H608 measures live
transfer separately. No product promotion or commit is made.
