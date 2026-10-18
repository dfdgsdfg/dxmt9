---
domain: present-pacing
workload: STALKER H502/r5 original captured sources, default-path boundary audit
title: "One-image source-policy preflight: exact pixels, but parity is not a timing experiment"
type: evidence
status: correctness-preflight-only
updated: 2026-10-16
source: traces/stkcop-clear-crossover-h575/{contract.md,run.py,report.json,build.log,native.log}; traces/stkcop-clear-crossover-h576/{contract.md,policy.hpp,policy_spec.cpp,build.py,build.json,build.log,native.log,run.py,report.json,verify.py,verification.json,boundary-audit.md}; traces/stkcop-clear-crossover-h576/{off,on,even,odd}.log
related: present-pacing-current-clear-live.574.md; present-pacing-current-clear-self.573.md; present-pacing-default-owner.571.md
---

# Source-policy switching has a bounded original-input oracle

H574's fixed population remains closed: its live CPU, pacing and observer
equivalence screens failed. H575/H576 prepare a different experiment without
adding more live runs or treating noisy comparisons as savings.

## One-image serial control

H575 runs three fixed original-input controls: H572 canonical C OFF, candidate
D OFF and the identical D image ON. All pass original source/output pixel SHA,
seven mapped sources, production-query transcript and 670/670 wrapper lifetime
checks. D OFF has zero fusion witnesses; D ON has 65. This verifies the serial
successful-input oracle despite the candidate's always-present failure relays;
it does not establish equivalence of arbitrary failure paths.

The existing production-called Clear predicate spec also passes 16,384
admission rows, 311,296 refinement states and 2,490,368 final-store checks.
These are value-model checks, not a substitute for runtime GPU verification.

## Private immutable source policy

H576 changes exactly one runtime archive member relative to D:
`dxmt9_draw_encoder_chunk.mm.o`. A private immutable policy selects OFF, ON,
even sequence or odd sequence. The mode is read once per process. Each call's
policy is tied to the qualified queue/source/storage identity; repeated
fragments select the same policy. Seven stale identity fields are rejected.
The actual `pending_clear::classify` admission predicate is unchanged.

No flush, wait, source reordering, cache reset or command-buffer split is added.
The normal source and Wine staging are unchanged. Native selection tests pass
2,560 rows, including fragment-consistency and complementary assignments. The
private image builds without warnings. Policy/frontier/fusion witnesses are
enabled only for these correctness executions; no timing is admitted.

| Mode | Encoder calls | Enabled calls | Fusions | Original GPU/query/lifetime oracle |
|---|---:|---:|---:|---|
| OFF | 9 | 0 | 0 | Pass |
| ON | 9 | 9 | 65 | Pass |
| Even sequence | 9 | 4 | 65 | Pass |
| Odd sequence | 9 | 5 | 0 | Pass |

All modes use identical image/probe bytes. There are two initialization calls
and seven mapped source calls. The two mixed-mode fusion sets are disjoint and
their union is the all-ON set, with no duplicate fusion identity. Independent
verification checks seven H575/H576 runs, image/input hashes, 36 H576 source
calls, actual fusion identities, source maps and original pixel hashes.

## Important limits discovered

All 65 opportunities occur in sequence 8, captured source 5. Consequently this
input does **not** exercise two fusion-active sources under different policies.
All 36 source entries have no active encoder, pending Clear, prior CB work or
deferred finalization. It does not test an actual prefix/suffix continuation.

Parity therefore cannot allocate performance treatments: it is correlated with
source roles and puts every fusion opportunity on one side. Comparing average
CPU of even and odd sources would not estimate an optimization benefit. The
new selector is a correctness fixture, not a qualified live benchmark.

## Correct default-path scope

The code audit resolves an overly broad prerequisite suggested after H574.
`dxmt9_device.hpp::resolveCpuReadyTapeDirectReplayEnabled` returns false for
an unset value, and H574 does not set `DXMT9_CPU_READY_TAPE`. The default path
is not the opt-in multi-source Tape session coordinator. **Correction from
[H578](present-pacing-clear-default-route.578.md): DCE is also OFF by default.**
`framegraph_backend.cpp::resolveRendererFeatures` enables pass coalescing, not
DCE, when Progressive features are unset. The constructor therefore selects
`CommandQueue::runEncodeLoop`. DCE prefix/suffix and Tape multi-source carry
are optional-path concerns, not prerequisites for default-path measurement.
H578 verifies the selectors on a real Device using the unchanged H576 image;
this does not constitute a new live invocation census.

For the opt-in path, `CleanClosedEncoderNoPendingClear` still permits earlier
CB work, and `storageHasDeferredSubmissionPayload` includes deferred callbacks.
That frontier cannot be used as an independent timing epoch. Even in the default
path, cache history and GPU backlog can couple source CPU costs. Correct pixels
alone do not establish timing independence.

## Remaining full-goal gates

No new CPU removal amount, live FPS gain, global default-path owner or default
promotion is claimed. H573 remains the local removal result; H571/H574 remain
the observed-worker evidence.

H578 completes a two-source actual-default-queue switching fixture. The forced
DCE H577 fixture aborted; its preserved failure is outside this default-path
gate. A subsequent timing contract must use content-independent assignment,
record source workload and account for cross-source performance effects rather
than compare parity groups. The independent ownership gap remains executable
main-thread delegation, shared-event callbacks and closure lifetime outside
H566 bodies. In particular the event helper registers before its fast-return
check, so return-without-wait is not evidence of no later callback.
