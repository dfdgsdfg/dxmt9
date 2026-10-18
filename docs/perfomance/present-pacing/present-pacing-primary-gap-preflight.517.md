---
domain: present-pacing
workload: STALKER Primary attribution, offline boundary preflight only
title: "Sparse post-Present root-gap prototype, not live Primary attribution"
type: evidence
status: prototype-tested-live-integration-open
updated: 2026-10-14
source: traces/stkcop-primary-gap-h517/{contract.md,probe.hpp,spec.cpp,verify.py,verification.json,failures.md,spec-arm64.log,spec-x86_64.log}; traces/stkcop-clockless-h457/tables.json
related: present-pacing-source-self-thin.516.md; present-pacing-root-game.477.md; present-pacing-guest-attribution-feasibility.511.md
---

# A narrower Primary boundary, not an ownership result

H516 leaves Replay and Encode co-sized on its diagnostic image. The next
unresolved owner is mixed Primary. H517 implements and tests only a bounded
diagnostic reducer for a **successful outer Present return → next outer public
D3D9 entry** envelope. No Wine/game run, staging, production-source change,
runtime integration, CPU saving or FPS measurement occurs here.

## Existing cadence is not that boundary

`src/d3d9/d3d9_pe_device_impl.hpp::D3D9DeviceImpl::Present` calls
`markPePresentReturnedForCadence` before three diagnostic notifications,
capture completion/arming and local scope destruction. The corresponding
`PresentEx` implementation in `src/d3d9/d3d9_pe_device.cpp` also completes or
arms capture after the marker. Swap-chain Present in
`src/d3d9/d3d9_pe_device_child_misc.cpp` does not use the device cadence marker.
This is not a defect in the cadence diagnostic; it forbids repurposing it as
an exact public-return/non-root CPU boundary.

The archived H475 public-table inventory identifies three candidate methods:

| Concrete table | Slot | Method |
|---|---:|---|
| Device (`IDirect3DDevice9Ex`) | 17 | Present |
| Device (`IDirect3DDevice9Ex`) | 121 | PresentEx |
| Swap chain (`IDirect3DSwapChain9Ex`) | 3 | Present |

That inventory contains 320 COM slots, plus six validator slots in the H475
generator. The current PE entry TU still defines the twelve inventoried
exports. These counts are integration inputs, **not a new runtime coverage
proof**. The next public entry might be IUnknown, a getter or an export;
watching only the next draw/device setter would include renderer work in the
gap. Nested application callbacks remain inside their enclosing D3D9 root.

## Prototype result

`traces/stkcop-primary-gap-h517/probe.hpp` uses a thread-confined fixed nesting
stack, non-copyable/non-movable scope owner and injected noexcept clock/sink.
It performs no clock query, request read or sink write for ordinary root
calls. A successful outer Present reads the request and, if armed, opens one
gap; the next root closes it before executing its body. Disarming cannot
truncate an already admitted pair. An unclosed tail remains pending.

ARM64 and Rosetta x86_64 builds pass **26 cases each**, including one million
ordinary calls, all three Present selectors, failed/nested Present, unwinding,
request changes, four identity mismatches, counter/wall reversals, impossible
CPU delta, clock failure, lost coverage, sink exhaustion, nesting order and
depth overflow, and independent probe instances. Compilation uses
`-Wall -Wextra -Werror`. The clocks are synthetic: this proves reducer behavior,
not measurement accuracy or observer overhead. Both preflight development
failures are preserved in `failures.md`; neither was a game/CPU gate result.

## Attribution limits and next gate

Even after integration, this envelope includes clock/bridge return and next-hook
entry work. It is only a diagnostic outside-public-root subset, **not exclusive
game CPU or a default non-renderer lower bound**. Subtracting it from whole
Primary would not yield exclusive renderer CPU. Low clock frequency does not
prove low perturbation: every public call would still carry a nesting hook.
Unhooked renderer callbacks, coverage failures and image reloads must reject
broader attribution, not become zero-cost entries.

Next integrate a new isolated PE diagnostic with H475's original-address-point
and output-object closure, then requalify actual-object ABI, nested callbacks,
identity/reload behavior, self-counter packet units and bounded trace parsing.
H473's old mode-1 POSIX packet cannot be silently used as this reducer's native
nanosecond/wall-bracket clock. Only then test whether the observed gap is useful
and whether the diagnostic transfers to default behavior. Do not combine
Primary from a new episode with H516 workers to claim a same-episode ranking.

The largest default dxmt9 single-thread CPU owner and correctness-qualified
removable amount remain unresolved. H477's all-call clock campaign stays
closed; H517 does not rerun it or relax its gates. Canonical runtime sources
and Unix binaries retain the H516 restoration hashes. Existing H501–H516
worktree changes are preserved; no commit or push is made by this step.
