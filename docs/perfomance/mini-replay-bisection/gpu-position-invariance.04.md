---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Explicit vertex-position invariance repairs the retained GT2 pixel cohort"
type: experiment
status: accepted-verdict
updated: 2026-09-14
source: experiments/output/gpu-tape-qualification-20260910/compile-profile-matrix-r585.json; experiments/output/gpu-tape-qualification-20260910/public-policy-r593/result.json; experiments/output/gpu-tape-qualification-20260910/public-policy-reauthentication-r598.json; experiments/output/gpu-tape-qualification-20260910/profile-reauthentication-r597.json; build/meson-logs/gpu-policy-final-r594.txt; build-x86_64-builtin/meson-logs/gpu-policy-final-x86-r595.txt; build/meson-logs/gpu-policy-final-tla-r596.txt; build/meson-logs/gpu-policy-regression-r601.txt; experiments/output/gpu-tape-qualification-20260910/public-policy-r586; experiments/output/gpu-tape-qualification-20260910/.public-policy-r588.staging-vw4kuiwf
related: specs/backend/requirements.md; specs/experiments/harness/replay/fixed-input-experiment.md; docs/perfomance/mini-replay-bisection/gpu-policy-qualification.03.md
---

# Explicit vertex-position invariance repairs the retained GT2 pixel cohort

## Current verdict

The normal, unmodified-host-metadata provider now reproduces all ten retained
GT2 failing originals exactly. Each runs twice with a cold device, authenticated
capture-source plan, ProductionSlot route, required actual path coverage and
normal renderer defaults. Both complete source and output RGBA artifacts match
the immutable originals byte-for-byte; GPU errors are zero. The public
`gpu_receipt_accepts` predicate accepts every result in r585.

After storage recovery, the fresh public `cpu-policy-compare --gpu-qualify`
transaction r593 completes and atomically publishes its accepted outer result.
Both exact policies pass CPU verification, two cold-device GPU iterations,
one CPU warm-up and three measured iterations. Post-publication r598
independently reauthenticates original/staged inputs, source plan, requested
policies, all child receipts, current runtime images and exact readback bytes.
The bounded GPU-correct policy-interface integration is now qualified.

This closes the retained **final source/output pixel cohort**, not every
intermediate attachment, arbitrary policy, workload or device. It is not an FPS
measurement or universal shader-equivalence proof. No original was replaced,
deleted or compressed, and no tolerance was relaxed.

| Original | Earlier differing final pixels | Current, each of two replays |
|---|---:|---:|
| r28 | 6 | 0 |
| r146 | 2 | 0 |
| r171 | 3 | 0 |
| r201 | 6 | 0 |
| r225 | 56 | 0 |
| r254 | 3 | 0 |
| r263 | 72 | 0 |
| r300 | 2 | 0 |
| r346 | 69 | 0 |
| r405 | 1 | 0 |

## Discriminator and production change

The original/replay MSL and observed PSO descriptors could be equal while
host-executable compatibility metadata changed the result. The initial
r555/r561 controls changed **both** minimum OS and SDK metadata to Wine's
minimum macOS10.7 / SDK12.3. They were not isolated SDK-version controls.
Although the compiler macro changed from MSL400 to MSL240, explicitly selecting
MSL2.4 alone retained the failure (r565). The simple compute arithmetic witness
also stayed fused under both hosts; it was not a vertex-position discriminator.

The split controls retain the modern minimum OS with SDK12.3 (r569), or retain
SDK26.5 with minimum OS10.7 (r575). The latter changes the calibrated scene
draw, including its entire output, to the old-host result even with explicit
MSL2.4. Thus language and getter equality alone cannot establish equivalence.

The bounded explicit source control `[[position, invariant]]` with
`MTLCompileOptions.preserveInvariance=false` reproduces the same full scene
image (r581). The similarly named compile option is **not** the source
attribute: setting it true returns the modern baseline in r582. These are
observations of this runtime, not a claim to have proved Apple's internal
compatibility implementation.

The implementation places the attribute in the shared translated/FFP/builtin
vertex-source emitters in `src/dxmt9/dxmt9_shader_sources.cpp`, and the bounded
partial-Clear vertex source in `dxmt9_blit_encoders.cpp`. It does not rewrite
arbitrary MSL text at the WMT boundary. The shared WMT source-library factory
selects the explicit versioned MSL2.4/fast-math/fast-functions profile with the
compile-option `preserveInvariance=false`. Actual options are recorded and
inherited through library/function/PSO lineage. Native probe clones share the
same factory. No executable metadata patch is used in the product.

Global `#pragma clang fp contract(off)` was tested and rejected: r577 changes
r405 to 76 incorrect final pixels. It changes fragment and other vertex
arithmetic too broadly. That production attempt was removed. Explicit FMA and
ordinary compute math remain available; invariance is scoped to position.

## Deterministic evidence

`dxmt9-metal-shader-compile-profile-spec` calls the actual WMT library and
pipeline entrypoints. It checks GPU-reported MSL240 and fast compute math, then
an independent 8x8 full-image position oracle. Exact uploaded cancellation
operands distinguish invariant position evaluation from an explicit-FMA
negative control that shifts half the image. This covers concrete pixels,
not merely successful compilation or non-black output.

r584 passes native compile-profile, shader-transform and compilation-lineage
tests (3/3). r587 passes the x86 compile-profile and Wine install-name audit
(2/2). Both complete native and x86 builds succeeded. The later r590 sweep
passed its first nine native/GPU targets, then disk exhaustion prevented
Python temporary directories and Meson log writes. It is **not** a successful
14-test final sweep. The r589 full-TLA attempt never started for the same
reason. Earlier lifecycle/TLA evidence remains historical, not a fresh result.
The position/compiler change is a value/source policy, not a new lifecycle
transition; no additional TLA model is claimed for its floating-point output.

After recovery, native/x86 full builds report no outstanding work. The fresh
r594 focused GPU/source/CLI set passes 14/14 and r595 x86 compile-profile plus
install-name audit passes 2/2. The r596 complete lifecycle gate passes all
190 configurations (58 safety, four progress, 128 expected counterexamples).
The expanded r599 regression set finds one stale capture-CLI exact-dictionary
expectation: it omitted existing CPU window-plan and buffer/event metadata.
The fixture still produces the same seven events and one Present. The test
now asserts those metadata fields exactly, including the Present's non-lease
coordinator span; it does not drop fields or weaken the comparison. The full
expanded set then passes 28/28 in r601. The failed r599 result remains retained.

## Public policy interface: positive children and a real rejected candidate

r588 runs the actual `cpu-policy-compare --gpu-qualify` interface on the
unchanged r405 bundle, comparing capture-source compatibility-exact against
production-exact. Both CPU verification children pass. Both GPU children pass
two cold iterations against the exact original source/output bytes. A later
read-only re-audit also accepts both GPU receipts and both CPU/GPU identity
bindings using the current public predicates and current loaded binaries.
`audit_profile_qualification_r592.py` reauthenticates the complete ten-original
cohort as well as those public children; its retained
`profile-reauthentication-r592.json` explicitly keeps overall completion false.

The wrapper then fails while saving the reference CPU measurement receipt
with `ENOSPC`. The staging tree and rejected child remain preserved at
`.public-policy-r588.staging-vw4kuiwf`; there is no published outer `result.json`.
Do not reconstruct or label this transaction accepted from its successful
children. It remains interrupted; the new r593 transaction supplies the
successful outer publication, rather than repairing or relabeling r588.

The published r593 report has `accepted=true`, `identity_bound=true`,
`gpu_evidence=true`, `qualification_scope=production-capture`, and no failures.
Each candidate GPU iteration executes 1,856 Direct draws with 182 leases,
10 compatibility draws and seven separately counted unbatched draws; both
policies encode 186 chunks and report zero GPU errors. Exact-capacity candidate
SoA growth remains observable (21), not a zero-allocation claim. The
`audit_public_profile_r598.py` post-publication audit independently invokes
the shared CPU/GPU predicates, verifies every child hash and current binary,
and confirms the original source plan digest is unchanged. The r597 cohort
audit separately reauthenticates all 20 r585 cold iterations; its
`public_outer_published=false` field refers only to interrupted r588.
Three measured CPU iterations demonstrate end-to-end publication, not a
statistically qualified performance ranking or wild FPS result.

r586 is a separate, meaningful policy negative. Its reference GPU route is
exact, but the production 5MiB-slot / 64MiB-aggregate headroom candidate fails
at raw30/span0 with `FailStopped/StructuralRejected`, before a final readback.
The public gate rejects it and skips measurement. The combination identifies
the qualified-capacity-receipt downgrade guard in
`CommandQueue::beginDirectChunkSlotReplay`; it does not yet identify which
provision/reuse precondition produced the unqualified new receipt. Preserve
this candidate as a transaction/admission counterexample, not as a shader
pixel failure or a qualified policy. CPU verification alone did not detect it.
The retained counters report 12 provisions, 85 reuses, nine lease denials and
zero allocation rollbacks, skipped-nonempty provisions or structural
continuation rejects. Retained capacity reaches 63,605,664 bytes and peak staged
credit is 5,222,072 bytes against the 67,108,864-byte aggregate cap. These
aggregate observations narrow the next native discriminator to denied
provisioning with an already-qualified payload; they do not prove the exact
last denial's ledger snapshot or authorize weakening receipt provenance.

## Qualified scope and remaining exclusions

The retained final source/output cohort, public exact-policy CPU/GPU join,
atomic publication, final reauthentication and fresh applicable native/TLA
gates are closed. The historical r586 headroom failure above is superseded by
[qualified ownership recovery](capacity-ownership-recovery.05.md): r610–r612
requalify the unchanged original with 1MiB/16MiB, 5MiB/64MiB and zero headroom,
including the later compatibility-growth boundary. r586 remains a preserved
negative rather than being relabelled. Other
workloads/devices, sequence tapes with per-interval original oracles, complete
intermediate attachments, arbitrary policies, concurrent worker scheduling
and performance promotion remain separate evidence scopes. No renderer
default or headroom policy is promoted by this qualification.
