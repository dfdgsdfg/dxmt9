---
domain: present-pacing
workload: production ingress routing; bounded lifetime checks
title: "H394: Capture-only ordinary ingress candidate"
type: implementation-evidence
status: default-off-promotion-open
updated: 2026-09-27
source: src/d3d9/device_c_chunk_replay.cpp; tests/native/backend/cpu_ready_production_routing_spec.cpp; tests/native/backend/producer_mark_reclaim_spec.cpp; build-x86_64-builtin/meson-logs/ingress-h394.txt
related: present-pacing-ingress-lifetime-audit.393.md
---

# H394: Defer provisional ingress marks, not ownership protection

After documentation commit `0fe59616`, the default-off
`DXMT9_DEFER_INGRESS_RESOURCE_MARKS=1` candidate selects capture-only ingress
independently of CPU-ready execution. No ABI, retention representation, queue
adoption, mutation fence, GPU watermark or production default is changed.
Existing direct publication or compatibility replay owns marking after handoff.

## Evidence

- Native arm64 baseline/candidate production routing: 2/2 pass.
- x86_64 builtin build passes; `file` confirms the provider and routing test
  are x86_64. Final focused suite: 9/9 pass (routing OFF/ON, mark/reclaim,
  producer interleaving, rename, offload queue, managed mutation OFF/ON,
  install-name audit). The candidate environment is set for the suite;
  baseline routing explicitly overrides it to zero.
- Actual ordinary commit with CPU-ready planning disabled captures before any
  mark on the app thread, then establishes a nonzero use sequence in Replay.
  The compatibility case checks that the bulk mark runs on the worker thread.
  The provider oracle still compares direct/compatibility command semantics.
- The bounded handoff checker uses production mark/reclaim predicates and
  exhausts action orders for one resource and two sequences. Correct protocol
  has no violation; early pin release, stale ingress stamp and missing mutation
  fence each produce violations. This is not the old producer reservation
  abstraction: ingress reserves no queue sequence.
- Existing `bash scripts/check/verify_tla.sh` completes successfully:
  65 safety, 5 progress and 157 expected-counterexample configurations.
  This is regression evidence, not a new all-family ingress refinement proof.

## Limits and next gate

The bounded checker assumes atomic final publication/stamping and excludes
reset/poison, backing rename, C++ memory ordering and GPU pixels. Existing native
coverage of those neighboring paths does not by itself prove their composition
with every deferred-ingress command family. Track closure in
[producer concurrency gaps](../../../specs/backend/producer-concurrency/gap.md),
R-BACK-43.9.

No wild run, GPU exact comparison or performance claim is made. The experiment
removes the combined ingress mark-ticket acquisition but compatibility may
perform that work later; do not interpret removal from the producer as removal
of all CPU work. Promotion requires final-use/failure coverage, identical-input
GPU comparison, then interference-free STALKER correctness and matched timing.
