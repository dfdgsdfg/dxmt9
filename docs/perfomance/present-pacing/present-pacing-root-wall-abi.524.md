---
domain: present-pacing
workload: Actual PE32/PE64 objects and epoch-qualified diagnostic activation
title: "Root-wall observer covers real COM calls without per-call CPU queries"
type: evidence
status: actual-abi-and-activation-qualified-no-default-cost-result
updated: 2026-10-14
source: traces/stkcop-root-wall-abi-h523/{contract.md,layout.hpp,marker.hpp,fixture.hpp,build.json,run.json,analyze.py,verify.py,verification.json}; traces/stkcop-root-wall-epoch-h524/{contract.md,epoch.hpp,control.cpp,spec.cpp,build.json,run.json,verify.py,verification.json}
related: present-pacing-root-wall-preflight.522.md; present-pacing-primary-activation.520.md; present-pacing-primary-gap-live.521.md
---

# Actual ingress and activation, not yet default attribution

H523 integrates H522's thread-confined reducer into H518's isolated diagnostic
ingress wrappers. H524 then ports the epoch-qualified activation controller to
the root-wall schema. Canonical binaries and production sources are unchanged.
These tests enable a live utility check; they do not establish the largest
default CPU owner, default-transfer tolerance or removable work.

## H523 actual-object evidence

Four runs (baseline/diagnostic × PE32/PE64) terminate normally. Both baseline
DLLs are byte-identical to the canonical images. The existing original-vtable-
address, output closure, object identity, cold reload and cross-process COW
checks remain intact: 320 COM methods, six validator slots and 12 exports.

Each run checks 8,192 short QPC/RDTSC endpoint enclosures with the predeclared
100 ns allowance. The reducer carries 200 ticks per completed outer call in
its upper sum. This is finite host/runtime qualification; neither clock units
nor these observations establish universal nanosecond accuracy. Each Present
checkpoint independently brackets the native clock service and checks identity.

Each diagnostic terminal map has four epochs/slots, four cold binding queries,
20,073 outer roots, 40,146 cheap root-clock reads, and three Present self-CPU
queries. Device Present, PresentEx and SwapChain Present publish one row each.
An application-owned private-IUnknown AddRef sleeps and calls Present inside
SetPrivateData: it is counted in **one outer root**, retains the sleep duration,
and publishes no nested Present row. Twenty thousand getters add exactly
20,000 roots / 40,000 cheap reads and **zero self-CPU queries**.

The strict terminal parser passes both maps and rejects 44 mutated inputs
across the architectures: stale identity/epoch, lost or unpublished rows,
inconsistent root/uncertainty totals, clock/CPU reversal and inactive/retired
activity. It consumes terminal data only; mutable slot counters are not a live
coherent-snapshot authority. Append-only rows use a final release-ready store.

## H524 activation evidence

The controller freezes native process birth, guest process identity, attachment
epoch, slot count and all thread keys around the sole `0 → 1` request CAS.
Only cold empty slots are admissible. After arming, active counters may advance;
retired counters must remain empty. In particular, the new Present self-query
counter must not accidentally be required to stay zero after activation.

Thirty-one predicate cases pass on each native architecture. Actual PE32 and
PE64 helpers each reject wrong process birth and accept epoch 4, then preserve
the three checkpoint selectors and nested-callback/getter checks. The helper
does not write the request itself. Terminal rows and all slot keys are checked
against the immutable activation receipt.

Both campaigns restore five pre-existing renderer files and recover three
temporary service files after helper termination. No Wine/helper remains live.
The root-wall parser and activation controller are diagnostic artifacts under
`traces/`, not a new runtime feature. No commit/push or FPS update occurs.

## Remaining gate

Use enclosing whole-root checkpoints for fixed scene boundaries; subtracting
cumulative upper sums must cancel only the roots before the first checkpoint,
not arbitrary endpoint uncertainty. Include waits and callback work. A bound
that overlaps worker CPU is inconclusive, not grounds for estimated wait
subtraction. Even a successful diagnostic ordering still needs a default-path
interference comparison and a separate correctness-preserving removal test.
