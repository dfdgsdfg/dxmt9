---
domain: present-pacing
workload: STALKER CoP archived guest context and frozen H510 x86 PE image
title: "Guest PC recovery does not establish non-renderer CPU ownership"
type: evidence
status: attribution-feasibility-rejected-no-new-live-run
updated: 2026-10-14
source: traces/stkcop-guest-attribution-h511/{audit.py,report.json,get_container.asm.txt,private_get.asm.txt,private_destroy.asm.txt,memcpy_thunk.asm.txt}; traces/stkcop-thread-identity-h462/default-r2/{post-tail-guest.json,post-tail-guest32.json}; traces/stkcop-thread-owner-h510/{inputs.json,summary.json}; src/util/com/com_private_data_iface.cpp; src/d3d9/d3d9_pe_thread_sampler.hpp
related: present-pacing-current-thread-owner.510.md; present-pacing-validation-live.509.md; present-pacing-root-game.477.md
---

# Do not turn a recovered guest PC into a CPU subtraction

**Subsequent correction:** [H512](present-pacing-cpu-counter-freshness.512.md)
also invalidates H510's instantaneous external-counter premise. The numerical
threshold below reproduces the old arithmetic; it is not an established
true-CPU bound. The PC/ancestry rejection does not depend on that threshold.

This is an offline feasibility audit, not another game or profiler episode.
It rejects using the existing PC sampler or 512-byte guest-stack dump to close
the mixed-Primary CPU attribution gap. It does not reject guest PC sampling
for symbol discovery, nor prove that a more capable observer is impossible.
No runtime/default change, new removable-cost estimate, commit or push occurs.

## Evidence checked against the actual image

`audit.py` checks H510's frozen x86 `d3d9.dll` SHA-256
`ace0376af896992b6407220fad1825c12ebbd55a70be64af6674875017f8e94b`
before disassembly. It also hashes the archived context/module captures and
rederives the H510 comparison threshold from its numerical endpoints.

| Surface | Observed evidence | Attribution consequence |
|---|---|---|
| H462 Primary guest context | TID 200, guest EIP maps to `xrParticles.dll+0x7baa`; 128 stack words, no d3d9/bridge-range values | One historical PC observation, not complete ancestry or a current-default CPU fraction |
| Private COM interface data | `ComPrivateDataEntry::get` calls vtable AddRef; `destroy` calls Release in the frozen image | Application code can execute below a D3D9 entry. A game-module PC alone cannot exclude an active D3D9 root |
| Surface `GetContainer` | The source-level `return container_->QueryInterface(...)` compiles to an indirect **call**, followed by return | Do not invent a root-eliding tail call merely from C++ return syntax |
| `memcpy` import stub | An indirect tail jump exists | The calling product frame can still remain; this is not evidence that the complete product ancestry disappears |

The private-interface calls are real capability surfaces, not evidence that
STALKER exercised them during H510. `GetContainer` does not by itself establish
an external application-owned container. The sampled x86 image and the older
guest observation serve distinct checks; their addresses are not transplanted
between processes. No exhaustive outward-edge or tail-call proof was made.

## Why neither current observer supplies the missing bound

The existing `PeThreadSampler::run` suspends the creating thread, reads its
control context, resumes it, then aggregates PC residency. It is not a running-
CPU-event sampler. Wine delivers suspension asynchronously, and its module map
is built once. It does not provide caller ancestry, complete stack coverage,
or a run-state-qualified CPU denominator. Increasing its frequency does not
repair those contracts and adds perturbation.

H462's separate helper reads only 128 stack words while suspended. It does not
read and certify the full active stack to its base, unwind frames, or resolve
cross-boundary ancestry. A product address beyond those 512 bytes is not ruled
out. Conversely, an arbitrary data word matching an image range would not prove
a return address. Context failures, unknown modules, truncated stacks and
unresolved ancestry must remain renderer-possible for an exclusion bound.

Inclusive D3D9-root CPU and exclusive dxmt9-module CPU are different quantities:
the former can include application callbacks and Wine/provider callees; the
latter misses synchronous work in other modules. Neither is automatically
removable CPU. Counting game PCs and multiplying their fraction by Primary CPU
would conflate residency, ancestry, running time and removability.

## Decision and next gate

H510's endpoint arithmetic still requires a same-default non-renderer lower
bound **strictly above 20.774947 ms/completed frame** to place Primary's renderer
share below that episode's Encode lower bound. This audit provides no such
bound. Encode and Replay remain the two identified dedicated workers at about
31.6 and 31.4 ms/frame, not a demonstrated global bottleneck ranking.

Do not launch a long ancestry-attribution campaign with either existing helper,
add per-call clocks again, or reinterpret H509's failed live-transfer gate.
Before another attribution run, a proposed observer must demonstrate bounded
suspend/resume safety, conservative callback/tail-call/unknown handling,
running-CPU attribution and an independently qualified perturbation limit.
The archived PC remains useful only for candidate discovery.

Work on a known dedicated worker can proceed without pretending this ranking
is closed, but it must name a distinct removable mechanism and retain original
input/output checks plus a default-live transfer gate. H508's local saving is
not that default-live saving. The original largest-owner/removal objective
remains open; no additional game episode was spent on an unqualified method.
