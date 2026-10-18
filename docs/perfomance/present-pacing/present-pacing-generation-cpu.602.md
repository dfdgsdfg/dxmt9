---
domain: present-pacing
workload: STALKER CPU observer lifecycle qualification and H502/r5 original capture
title: "Generation-qualified Replay CPU identity accepts retired address reuse"
type: evidence
status: observer-generation-qualified-live-attribution-pending
updated: 2026-10-16
source: traces/stkcop-generation-cpu-h602/{contract.md,registry.inc,observer.hpp,trace.hpp,prepare.py,derivation.json,native.cpp,build.py,build.json,sparse_parse.py,parser_test.py,identity-tests.json,parse.py,parser-tests.json,preflight.py,preflight.json,verify.py,verification.json}; traces/stkcop-generation-cpu-h602/{original-observer0,original-observer1,queue-observer0,queue-observer1}.log
related: present-pacing-queue-cpu-owner.601.md; present-pacing-replay-cpu-owner.596.md
---

# Identity ambiguity is closed in the new private observer, not in old results

H602 implements generation-qualified observer identity and passes native,
parser and original-effect controls. H601 O1 remains rejected and is not
reinterpreted. This step supplies no live timing result or CPU removal claim.

## Mechanism

The old sparse observer identified Replay by queue address only. H602 schema
version2 carries the immutable `queryQueueIdentity_` domain for both Replay
and Encode endpoints. A private bounded registry records Bind and Retire:

- `CommandQueue::bindSelfLifecycle` registers queue/controller/domain.
  Repeating the identical live binding is idempotent.
- The end of the `CommandQueue` destructor body records retirement after
  `stopThreads`. This is a diagnostic lifecycle marker, not proof that every
  C++ member destructor has finished. Storage cannot be reused until the
  containing object's destruction and deallocation complete.
- `ReplayOffloadWorker::run` resolves its immutable tuple once at startup.
  Polls carry that captured domain, not a lookup of whichever object currently
  occupies the address.
- Phase scopes read the thread-local tuple and select one queue/domain/thread.
  A second generation cannot silently enter the same aggregate.

The registry mutex is used for construction/retirement and worker attachment,
not per draw or per CPU sample. The registry holds at most128 generations and
fails diagnostic qualification on exhaustion. Simultaneous owner/controller
overlap, retired-domain reuse, stale retirement and unknown attachment also
set faults without changing product command effects.

The sparse parser accepts address reuse only after explicit retirement. Every
sample must name a live tuple; both epochs must retain one domain and worker
identity, and Replay/Encode must select the same domain. Missing retirement,
stale samples, cross-generation workers and overlapping bindings reject the
trace. Runtime TLS is not a resource-lifetime capability: stale samples are
rejected by lifecycle validation rather than a hot-path registry lock.

Four private objects are rebuilt together, avoiding mixed inline observer
definitions: frontend core, replay offload provider, command queue and queue
lifecycle controller. No product header, object layout, PE ABI, queue policy,
shader, ordering, resource stamp or lifetime behavior changes. This is
diagnostic code under ignored `traces/`, not a production promotion.

## Verification

| Evidence | Result |
|---|---|
| Native ARM64 / Rosetta | Six controls pass: valid address reuse, registry faults/capacity, disabled pass-through |
| Sparse lifecycle parser | One address-reuse positive and13 malformed/stale/overlap negatives pass |
| Phase parser | One positive and13 malformed identity/count/bounds/padding negatives pass |
| Original capture observer OFF/ON | Exact pixels, seven source mappings, query transcript,670 wrapper lifetimes and normal restoration pass |
| Ordinary queue fixture OFF/ON | Two sources, completion and API readbacks pass; draws skipped, not a pixel oracle |
| Independent byte verifier | Rechecks generation1 retirement → generation2 reuse, selected domains, phase counts and four effect controls without parser imports |

Original source/output SHA-256 remains
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
The relinked original-capture executable and dynamic provider use the same
changed objects. Archive member checks allow only the four intended objects;
unix install-name audits pass. Shared Wine staging was not changed.

Metal toolchain checks pass on Apple M1, Xcode27.0 and Metal compiler32023.921.
The metal-gpu-debug workflow supplies toolchain and existing readback-oracle
checks here; no new GPU timing trace or shader optimization is introduced.
Formal product-scheduling refinement is not applicable because the change
only records diagnostic identity and does not alter production transitions.

## Next attribution gate

Keep H601's accepted single-run queue CPU range6.888–8.483ms/frame as limited
evidence, not a replicated or removable budget. Do not spend another live
population solely recovering the same coarse number. The next image should
combine this identity repair with disjoint queue-body attribution: binding
capture, segment/capacity planning, reservation, resource marking, actual SoA
append, commit and residual teardown. Retain coarse caller-self CPU endpoints;
avoid per-draw CPU syscalls and double-counted nested wall scopes.

After equivalent effect controls, freeze a new bounded live population and
identify the largest constituent before implementing a removal candidate.
The objective remains default-live largest single-thread CPU ownership and
correctness-qualified removable CPU, not merely a passing observer.
