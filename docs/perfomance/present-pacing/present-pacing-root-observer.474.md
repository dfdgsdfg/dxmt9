---
domain: present-pacing
workload: Isolated PE32/PE64 outer-call CPU scope through Sikarugir
title: "Nested root CPU aggregation survives reload and rejects clock faults"
type: evidence
status: isolated-root-aggregation-qualified-actual-d3d9-integration-open
updated: 2026-10-08
source: traces/stkcop-root-observer-h474/{contract.md,marker.hpp,body.cpp,nested.cpp,probe.cpp,build.py,run.py,analyze.py,build.json,images.json,run.json,summary.json,i686.jsonl,x86_64.jsonl}; traces/stkcop-root-observer-h474/recovered; traces/stkcop-root-observer-h474/{build-failed1.json,launch-failed1,launch-failed2,launch-failed3}; traces/stkcop-root-observer-h474/fault-scope/{probe.cpp,run.py,marker.generated.hpp,receipt.json,i686.jsonl,x86_64.jsonl}; traces/stkcop-primary-clock-h452/inventory.json; traces/stkcop-primary-inplace-h460/{interposer.hpp,generate.py,manifest.json}
related: present-pacing-root-clock-bridge.473.md; present-pacing-normalized-clock.472.md; present-pacing-primary-transfer-checkpoint.465.md
---

# Qualify the accumulator before another game attribution run

H473 supplies a same-caller native CPU clock. H474 implements an isolated
outer-call scope using that unchanged diagnostic clock service. It does not
modify or load the renderer, run STALKER, claim a default-path owner ranking,
or estimate removable CPU. Product source and all five shared renderer images
remain unchanged.

## Boundary decision

A read-only comparison at `c0199347` finds all eleven H452 inventory source
hashes unchanged. Its 350 bodies are not 350 public roots: the implemented
surface is 320 concrete COM methods, six C validator slots and twelve public
DLL exports, plus twelve nested forwarding/factory bodies. The current
`src/win32/d3d9_entry.cpp` also matches H460's archived baseline.

Reuse H460's typed original-address-point, process-private COW interposer for
the next integration gate. Its scope encloses the saved original function's
prologue, body and epilogue. Body insertion starts too late to cover those
edges; H458's cloned-vptr approach must not return. H460's typed output closure,
IUnknown/base/Ex aliases, validator, self-destroying Release, reload and image
protection checks remain necessary. Historical ABI tests do not certify the
new observer. This experiment exercises synthetic saved-call-shaped wrappers,
not the actual 338-entry D3D9 surface.

## Isolated fixture

One successful helper per architecture loads and unloads a separate scope DLL
twice. Each incarnation runs 8,192 empty roots, 1,024 cross-translation-unit
nested roots, 1,024 roots containing synchronous Unix clock-service work, a
200 ms sleeping caller with a measured busy peer, and 1,024 exception-unwinding
roots. One priming root precedes accepted measurement in each incarnation.

Both architectures pass:

- Exact root/entry populations and two observer reads per outer root; nested
  calls add entries but no extra clock pairs or double-counted root CPU.
- Stable guest/native caller identity, distinct busy-peer identity, coherent
  inactive phase endpoints, monotone raw counters and zero normal slot faults.
- Two attachment incarnations with distinct caller/peer slots, rather than
  reusing a guest TID's old counters; no live image after final unload.
- Four deliberately rejected cases: changed native identity, backwards raw
  time, unavailable clock function and exhausted slot capacity. Overflow is
  injected only after the positive phases; final mapping overflow is expected
  and does not stand for a zero-fault production run.
- Sum of nonoverlapping root raw deltas no greater than the enclosing raw
  delta. Independently, every normalized outer delta fits its native endpoint
  envelope with H472's predeclared 2 us outer-reference allowance.

| Quantity | PE32 | PE64 |
|---|---:|---:|
| Empty-root whole-loop CPU/root, two incarnations | 0.866–0.874 us | 0.675–0.676 us |
| Caller CPU inside the 200 ms sleeping root | 23.9–28.2 us | 17.8–19.8 us |
| Busy-peer enclosing native CPU | 204.1–210.1 ms | 200.1–207.4 ms |

The first row includes both clock calls, scope/mapping updates and fixture loop
work. It is not a pure dispatcher cost or a correction to subtract from game
measurements. The sleep row is CPU, not 200 ms wall residence. Actual game call
counts and perturbation have not been measured here.

Accumulate raw deltas first and normalize once with outward integer rounding.
The native 2 us envelope allowance applies to the outer endpoints, not every
short root. Conservation does not prove arbitrary root-boundary error or
sub-microsecond physical accuracy. Root CPU can legitimately be below the
native outer interval's lower edge because the latter includes unscoped work.

The initial three clock negatives directly exercise `readClock` on scratch
slots, so they alone do not test failure after successful accumulation. A
separate deterministic `fault-scope` extension retains byte-identical
Slot/Map/readClock/Scope code and replaces only clock-service resolution with
an injected clock. Both PE architectures pass all eight cases: service error,
changed native TID, backwards raw time and unavailable function, each at entry
and exit. An earlier successful root's total remains stored, but the failed
and subsequent roots add no CPU; depth balances and the fault stays latched.
This tests scope failure logic, not actual Wine service-fault delivery.

Consumers must reject the **slot fault**, not merely the map's coverage flag:
a faulted inactive slot intentionally retains a partial earlier total.
Missing/rejected mapping or absent slots must likewise invalidate evidence,
never mean zero API CPU. Abrupt thread termination, SEH, overlapping images
and active-root unload are not qualified by these fixtures.

## Retained bootstrap failures and recovery

The initial build fails because a negative-test patch lands in the wrong
function; its compiler log is retained. Three premeasurement helper attempts
are also retained: an unavailable clock DLL, rejection of a stale same-guest-
PID mapping, and an explicit loader error 126 identifying the missing app-local
builtin discovery stub. Use unique run mapping prefixes and the unchanged
H473 PE stubs beside both helpers, matching H473's successful layout. No clock
scale, acceptance tolerance or valid negative result is changed.

The final four fixture builds have no warnings. Both successful helpers exit
normally. After terminal/no-live-helper checks, all five newly staged diagnostic
copies are moved to `recovered/`; no renderer or user file is deleted. Renderer
hashes agree before/after every launch attempt.

## Remaining decision gates

1. Integrate this scope with H460's actual-object closure fixture and recheck
   original object identity, output closure, final Release, reload and COW
   isolation. H474 alone does not establish those runtime properties.
2. Read only coherent inactive live snapshots with exact attachment/thread
   identity around native CPU endpoints; handle partial roots at frame-window
   edges explicitly. No live concurrent snapshot reader is qualified here.
3. Use one finite, predeclared default-policy attribution comparison. Inclusive
   original-call CPU contains synchronous callbacks and observer work; it is
   not PE-exclusive CPU. Default transfer and Replay/Encode joins remain
   separate gates. Overlapping intervals must remain inconclusive.
4. A maximum-owner result still needs a correctness-preserving intervention
   with a measured removable amount. H468's failed repeatability and H469's
   indeterminate scene correspondence remain unchanged.
