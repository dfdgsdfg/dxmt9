---
domain: present-pacing
workload: STALKER CoP current controlled default Day high 1280x720
title: "Fresh current-default guest joins identify the large unnamed workers"
type: evidence
status: current-guest-entry-joins-closed-primary-renderer-unresolved
updated: 2026-10-14
source: traces/stkcop-thread-owner-h510/{contract.md,inputs.json,calibration.json,run.py,census.py,analyze.py,verify.py,summary.json,verification.json}; traces/stkcop-thread-owner-h510/A1/{receipt.json,progress.jsonl,thread-join.json,setup-x64.json,setup-x86.json,setup-census.json,tail-x64.json,tail-x86.json,tail-census.json,post-tail.png,restore/restored.json}; experiments/output/app-d3d9-stkcop-bench-thread-owner-h510-a1/{result.json,daybenchmark.result}
related: present-pacing-validation-live.509.md; present-pacing-default-thread-identity.462.md; present-pacing-root-game.477.md
---

# Current process joins close the large unnamed-worker identity gap

**Endpoint correction:** [H512](present-pacing-cpu-counter-freshness.512.md)
falsifies instantaneous external CPU-counter freshness. The CPU ranges and
0.206--0.223 ms ordering below are historical reported-counter arithmetic,
not certified true-CPU bounds; the >20.775 ms exclusion threshold is likewise
unqualified. The guest/TEB joins remain separate identity evidence.

H509's large unnamed native workers must not be assigned to Replay or Encode.
One fresh current-default episode reuses H462's read-only guest/TEB technique,
without borrowing any earlier process's handles. The large workers again have
X-Ray helper and WinMM entry provenance. This is a fresh identity link, not a
new discovery, exclusive CPU attribution, or a game/Wine tuning proposal.

## Qualified identity and measured scope

The renderer is H509 A: H506 Unix image `0bf898b61b9da524…` and identical
current frozen PE images. Day/high, 1280x720, max-threads 2, snd_targets 16,
production-counter occlusion, product observers OFF. All seven spatial knots
qualify with H509's unchanged 1 m / 1.5 degree endpoint and 2 m / 3 degree
corridor gates. Native CPU endpoints enclose exactly **133 completed frames**.
This is an external identity diagnostic, not an observer-free FPS run.

Fresh Rosetta main/created-pthread calibration confirms the runtime-specific
`opaque libproc handle = pthread_self + 0xe0` relationship. Installed Sikarugir
`ntdll.so` SHA-256 remains
`81f81cf1bb70069e94fa89d53fb7327cfb52fb9629c5b84134c851e681a20235`,
the exact H462 binary with its audited TEB64+0x3a0 pthread field. The separate
x64 and x86 helpers collect TEB and 32-bit module/start information respectively.
They use no `--contexts`, suspension, debugger attach, target write or root hook.

Setup and post-interval censuses each uniquely join seven nonzero guest
threads, with matching guest PID/TID, TEB, pthread, handle, start address and
native name. Native process identity is **[62134, 1791904114, 82536]**. Both
censuses bracket their reads with native counters; joined handles persist
with monotone counters across the interval. Zero Primary TEB pthread is not
invented into a guest join: Primary and Replay keep their independent setup
sample/numeric-TID/counter-enclosure identity links.

| Current-process entry/worker | Guest TID | Opaque native handle | CPU ms/completed frame |
|---|---:|---:|---:|
| Mixed X-Ray Primary | Separately identified | See receipt/join | 52.406--52.419 |
| `xrCPU_Pipe.dll+0x3730` | 264 | 17248866528 | 51.066--51.077 |
| `dxmt9-encode` | Native worker | See receipt/join | 31.644--31.660 |
| `ReplayOffloadWorker::run` | Native worker | See receipt/join | 31.438--31.438 |
| `winmm.dll+0x13b00` | 316 | 17251082464 | 28.561--28.570 |
| X-Ray Secondary, `MSVCR80.dll+0x5c980` | 324 | 17252733152 | 13.526--13.533 |
| Named `wine_dsound_mixer`, `dsound.dll+0x12b80` | 312 | 17251074272 | 1.390--1.392 |

Ranges are reported-counter endpoint/rounding calculations, not confidence
intervals or, after H512, certified executed-CPU enclosures. CPU on
different threads overlaps and cannot be summed as frame latency. Entry
provenance is not proof of every later instruction: the game helper can run
callbacks, and a WinMM-start thread can enter audio/application code. Neither
whole CPU total is a removable dxmt9 budget. Their repeated numeric handles
in historical runs are irrelevant to this fresh proof. One zero-handle row
remains unpriced, so process-wide coverage is still false.

## What is now narrower, and what is not solved

Encode exceeds Replay by **0.206--0.223 ms/frame in this interval**. This is
less than one percent of either worker, not a robust universal ranking: H509
already observed the order change between live episodes. The large unnamed
workers are no longer unidentified native candidates here, but Primary still
contains game, Wine, PE and synchronous Unix work.

To rule Primary's renderer share below the leading dedicated worker by
subtraction in this very run requires a justified same-default non-renderer
lower bound greater than **20.775 ms/frame**. H477's intrusive-clock residual
and H460's conditional marker result cannot supply it after their missing or
failed default-transfer gates. No new root instrumentation was introduced.

H508's same-input local removal estimate and H509's failed live-economy gate
remain unchanged. H510 performs no candidate comparison and establishes no
new removal amount. It does not finish the original largest-owner/removal
objective. Do not retest this already-resolved helper identity as a new
renderer bottleneck, or change the game scheduler/audio policy to claim a
default dxmt9 improvement.

One read-only follow-up on existing evidence distinguishes a possible next
capability from repeating H477 clocks: H462 default-r2's saved x64 helper
successfully recovered Primary's **32-bit guest EIP** with
`Wow64GetThreadContext`, at `xrParticles.dll+0x7baa`, despite the poor native
Rosetta unwind. Its 128 captured stack words contain no d3d9/bridge image
address, but that is only a truncated raw stack, not a complete ancestry proof.
Do not price it as non-root CPU. A conservative guest-context/ancestor
classifier would need complete stack coverage, tail-call/callback exclusions,
unknown-as-root handling and an independent perturbation gate before any use
in current-default ownership. No such classifier or new context-sampling run
is implemented or qualified here.

## Verification and restoration

The game exits zero and writes its original result; catalogue `missing_capture`
is the sole administrative failure, predeclared because automatic capture is
delayed outside the run. An independent post-interval screenshot is visually
coherent, not a full-sequence or exact pixel oracle. Computer-use restores
focus after setup helpers and captures only outside the selected interval.

The analyzer checks scene/identity/counter monotonicity and no census/UI overlap.
`verify.py` separately rederives native arithmetic and raw TEB/start joins and
checks frozen images, ntdll, xrCPU_Pipe and all restoration hashes. The existing
H462 xrCPU_Pipe SHA matches exactly. Fourteen pre-existing shared Wine/prefix/
result files are restored, and no game/helper remains. No runtime source,
default, README value, commit or push changes in this experiment.
