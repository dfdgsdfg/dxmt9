---
domain: present-pacing
workload: STALKER attribution observer, real-object ABI preflight only
title: "Sparse Primary root-gap observer passes actual PE32 and PE64 closure"
type: evidence
status: actual-object-gate-passed-live-owner-open
updated: 2026-10-14
source: traces/stkcop-primary-gap-liveabi-h518/{contract.md,build.py,build.json,run.py,run.json,marker.hpp,packet.h,native.c,bridge.c,layout.hpp,fixture.hpp,analyze.py,test_analyze.py,verify.py,verification.json,parser-tests.log,failures.md}; traces/stkcop-primary-gap-liveabi-h518/{i686,x86_64}-diagnostic
related: present-pacing-primary-gap-preflight.517.md; present-pacing-source-self-thin.516.md; present-pacing-root-objects.475.md
---

# Actual public-root closure, not game attribution

H517's reducer now executes through isolated PE32/PE64 diagnostic D3D9 DLLs
and a separate current-thread Unix clock service. Four actual-object runs
(baseline/diagnostic per architecture) pass. This completes the bounded ABI
integration gate; **no STALKER episode, default CPU-owner verdict, removable
budget or FPS claim** follows.

## Build and clock authority

The builder reuses current canonical frontend archives and H475's typed
320 COM + 6 validator + 12 export wrappers, preserving original vtable address
points and output-object closure. Three selectors identify device Present,
PresentEx and swap-chain Present. Only `S_OK` at an outer return opens a gap;
the next public root closes it before running its original body.

Both unmodified-entry relinks are byte-identical to their canonical DLLs.
The initial build rejected timestamp-only differences; the corrected linker
invocation explicitly supplies the canonical PE timestamp to both arms. The
strict full-byte gate remains unchanged, and the failed first build survives
under `attempt1/`. No post-link masking or production rebuild occurred.

The new 96-byte pointer-free H518 packet carries native PID/TID/process birth,
self `proc_pidinfo` CPU nanoseconds, and Mach wall timestamps bracketing that
query. It does not reuse H473's mode-1 POSIX raw units or external thread CPU
snapshots. One cold query binds each thread/image epoch; ordinary calls do not
read the CPU clock. H513's finite current-host allowance is explicit at 4 us
for CPU-over-wall rejection, not asserted as a universal OS bound.

The fixed mapped format has 64 thread/epoch slots and 4096 completed-gap rows.
Wine PID/creation time protects the map; native PID/birth/TID plus image epoch
qualify rows. Reload increments epoch and clears the request. Lost coverage,
clock fault, overflow and row loss are fatal to attribution. The parser reads
only terminal-process maps, not concurrently changing slot fields.

## Actual-object and negative-control evidence

| Check | PE32 | PE64 |
|---|---|---|
| Baseline helper | Pass | Pass |
| Diagnostic COW parent/child helper | Pass | Pass |
| Load/unload epochs | 4 | 4 |
| Concrete tables / exports covered | 16 / 12 | 16 / 12 |
| Cold binding queries | 4 | 4 |
| Admitted gaps / clock queries | 3 / 6 | 3 / 6 |
| Incomplete gaps / loss / faults | 0 / 0 / 0 | 0 / 0 / 0 |

The helper retains H475's base/Ex/IUnknown/container identity, cached zero-ref
resurrection and final Release checks. Its parent confirms unchanged private
`.rdata` after the child patches its own table pages. Coverage means concrete
table installation and representative calls, **not every method executed**.

New cases execute all three successful Present roots, close via a surface
AddRef, an export or QueryInterface, and disarm before closing an admitted
pair. A real private-IUnknown AddRef callback calls Present reentrantly under
SetPrivateData: that nested Present succeeds but opens **no gap** and issues
no new observer clock queries. Twenty thousand actual GetRenderState calls
also issue no observer clock queries while the request remains enabled.

An independent parser reconciles all six original rows with their thread/epoch
slots, final CPU cursors and helper-reported native identity. Sixteen parser
tests include clipped files, live headers, fault/loss, incomplete coverage,
unpublished rows, stale epoch/TID, nonadjacent roots, counter/wall reversal,
impossible CPU, duplicate binding and slot/row disagreement. All pass.
The few fixture microsecond durations include hook/service work and a Sleep
case; they are not observer calibration or STALKER performance evidence.

## Restoration and remaining work

All five shared Wine renderer files are restored to their exact prelaunch
hashes. The three new Wine-root service files are moved into `recovered/`,
not deleted. Diagnostic images and raw maps remain under `traces/`. No Wine,
game or helper process remains. The three previously restored product source
files and both canonical Unix images still have their H516 hashes.

Next obtain **same-episode** Primary gap/whole-thread cursors and H516 sparse
worker reports at the fixed late camera knots. Check whether the contiguous
post-Present subset is substantial before any longer campaign. A hook on every
public call still adds work despite sparse clocks: ABI success is not low
interference or default transfer. Do not label the measured envelope exclusive
game CPU, subtract a synthetic timer cost, or combine it with workers from a
different episode. The largest default dxmt9 single-thread owner and a
correctness-qualified removable amount remain the active objective.
