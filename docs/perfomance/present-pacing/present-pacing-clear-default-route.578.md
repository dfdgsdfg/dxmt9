---
domain: present-pacing
workload: STALKER candidate image, synthetic two-source default-queue fixture
title: "Default queue route correction and two-source policy oracle"
type: evidence
status: correctness-preflight-only
updated: 2026-10-16
source: traces/stkcop-clear-default-route-h578/{contract.md,fixture.cpp,run.py,report.json,build.log,off.log,on.log,even.log,odd.log}; traces/stkcop-clear-default-fragments-h577/r2/{fixture.cpp,report.json,off.log}; traces/stkcop-clear-crossover-h576/build.json
related: present-pacing-clear-crossover-preflight.576.md; present-pacing-current-clear-live.574.md; present-pacing-current-clear-self.573.md
---

# Ordinary queue, not DCE, is the default

H576's initial default-DCE description was wrong. Both CPU-ready Tape and DCE
are opt-in. H574 set neither selector. The relevant selectors are
`dxmt9_device.hpp::resolveCpuReadyTapeDirectReplayEnabled` and
`framegraph_backend.cpp::resolveRendererFeatures`. With both disabled, the
`CommandQueue` constructor selects `runEncodeLoop`.

H578 checks the actual `CreateDXMT9Device` queue against the unchanged H576
runtime libraries. The friend test seam reads the actual selectors and publishes
sources through the production lifecycle; it does not replace the backend,
choose an encoder loop, change stop flags or process completion manually.
Public `submitFlush` waits for completion, and both source sequences reach the
completion waterline before readback. All four executions report Tape=0/DCE=0.

## Bounded oracle

Two disjoint attachment pairs are allocated and seeded before publication.
Each source contains a depth Clear followed by a Draw. The four processes use
the same image and fixture bytes, with Metal API validation enabled.

| Policy | Fusion sequences | Completed waterline | Color/depth/stencil readback |
|---|---|---:|---|
| OFF | None | 2 | Pass, both surfaces |
| ON | 1, 2 | 2 | Pass, both surfaces |
| Even | 2 | 2 | Pass, both surfaces |
| Odd | 1 | 2 | Pass, both surfaces |

All eight source entries are non-deferred with no pending Clear on entry.
The complementary policies now exercise two fusion-active sources, unlike
the original-input H576 control, where every fusion belonged to one source.
The build emits no diagnostics; loaded dylib/unixlib paths and hashes match
the frozen image. No product runtime source or Wine staging is changed.

`DXMT_SKIP_ALL_DRAWS=1` isolates attachment actions: the renderer still opens
the render pass, but shader draw execution is skipped. Exact color, depth and
stencil checks therefore cover Clear/load/store behavior, not arbitrary shader
output, query transcripts, lifetime-fault matrices or the full game. H575/H576
remain the original-input GPU/query/lifetime evidence. This is not a timing
experiment, randomization design or default-promotion result.

## Preserved optional-path failure

H577/r2 forced DCE with an injected scheduling hint. Its first OFF execution
aborted after prefix and suffix with:

```text
unique_lock::lock: already locked: Resource deadlock avoided
```

`CommandQueue::runDceChunkLookaheadEncodeLoop` reacquires the lock in its branch
and subsequently calls `lock.lock()` again. The observed exception matches
that source pattern; no stack-based attribution was collected. The population
remains incomplete and preserved. This optional-path candidate is neither a
default performance explanation nor a prerequisite for the current goal.

## Consequence for the investigation

Return to the ordinary queue's CPU ownership and content-independent treatment
design. Do not extend H574's failed population or add optional coordinator
proofs as gates. H573's 2.291 ms paired median source-5 saving remains local;
H574 did not establish repeatable live savings. Remaining global-owner gaps
are main-thread delegation, shared-event callbacks, and captured closure
construction/destruction outside the observed worker bodies.
