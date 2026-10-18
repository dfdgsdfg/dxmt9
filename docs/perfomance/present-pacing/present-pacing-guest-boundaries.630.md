---
domain: present-pacing
workload: STALKER installed PE32 engine, static render-call boundary audit
title: "H630 — three guest call sites for coarse observation, with tail-call and ABI constraints"
type: evidence
status: static-sites-qualified-runtime-hook-open
updated: 2026-10-16
source: traces/stkcop-guest-boundary-h630/{collect.py,commands.json,provider.stdout,engine-exports.stdout,engine-disassembly.stdout,dispatch-function.stdout,end-function.stdout,correction.md,report.json,verify.py,verification.json}
related: present-pacing-owner-union-live.629.md; present-pacing-backend-cpu-camera.454.md; present-pacing-completed-workload-visibility.455.md; present-pacing-guest-attribution-feasibility.511.md
---

# H630 — guest observation must enclose the backend tail and preserve EAX

The installed engine has three concrete call sites suitable for investigating
a coarse render phase. Two binary properties constrain any observer:

1. `CRenderDevice::End` publishes its SASH sample before tail-jumping through
   the backend vtable. Observing the sample or the last instruction before
   that jump misses the backend tail. The normal return to the **caller**
   encloses it. Do not equate SASH publication with complete renderer return.
2. The render callback dispatcher receives `Device+0x288` in **EAX**, not
   ECX. Ghidra's nominal no-argument signature does not describe this hidden
   register input. A normal C++ `thiscall` wrapper would be wrong.

This is a static qualification, not an installed hook, new live CPU result or
demonstrated saving. It responds to the proposed game-DLL logging approach
without changing the target or repeating H629's timing population.

## Identified image and sites

Engine SHA-256 remains
`96a4b27dd8e01ca119ddf2775e1b5032d19fcd21370203b3133dc1bb47889e7b`;
R2 remains
`6e1914e047075409b727bf9fd3087ae6cb2404ac85fb8cfd2e797550ef3cc776`.
The engine is x86 PE32 with preferred base `0x00400000`. The table uses
**RVAs**, not file offsets or assumed runtime addresses.

| Meaning | Caller RVA | Callee RVA | Normal return RVA | Input/result contract |
|---|---:|---:|---:|---|
| Begin attempt | `0x53844` | `0x52d50` | `0x53849` | ECX is Device; EAX controls success branch |
| Render callback dispatch | `0x53853` | `0x53f30` | `0x53858` | EAX is Device+0x288 |
| End, including backend tail on return | `0x538a7` | `0x52df0` | `0x538ac` | ECX is Device |

All are complete five-byte `E8 rel32` calls in `CRenderDevice::on_idle`.
Actual exported names independently identify Begin/End. The dispatcher is
not exported: `OnRender@CLoadScreenRenderer+0x10` in objdump is a nearest-symbol
label, not its function name. Its instructions iterate callback entries and
invoke their vtable functions. This is an aggregate callback phase, not a
single R2 render pass or a promise of one callback per frame.

The caller first tests active state. A zero Begin result branches directly to
`0x538ac`, skipping both dispatch and End. A successful path may also execute
statistics work between dispatch and End. A reducer must preserve these gaps
instead of treating the three call durations as a complete frame partition.

Inside End, the SASH call is at RVA `0x52f66`; the last backend dispatch loads
vtable offset `0x70` and executes `jmp eax` at `0x52f7b`. The inspected End body
contains no `ret`. The identity and semantics of the live indirect backend
target are not proved by that slot offset alone; this audit does not label it
as a particular D3D9 method.

## Evidence and limits

REA 6.4.0, Node 24.22.0 and scoped Ghidra 12.1.4 with native arm64 decompiler
and JDK 25.0.4 pass provider preflight. Unrelated absent providers/integrations
remain informational; no agent configuration is changed. Two sequential
function queries preserve complete raw output and snapshot. Pseudocode is
checked against instructions, exports, branch destinations and original PE
section mapping; it is not treated as runtime execution evidence.

The collector checks 153,727 decoded instructions against on-disk bytes and
pins the three call instructions, branch, dispatcher input and End tail.
No decoded direct branch enters a call instruction's interior. This does not
prove every indirect incoming edge or address-taken code pointer is absent.
The independent verifier rederives the calls and rejects twelve corrupted
site/target/return/byte manifests. No executable memory or target file is written.

The first post-capture audit expected the wrong REA operation identifier:
`function_detail` rather than `analyze_function`. All six commands had already
succeeded. The corrected audit reuses their argument/status/output-hash-qualified
artifacts, adds exact entry-address checking, and reruns the static checks.
No provider query or runtime population is replaced.

## Minimal prospective observer contract

Use these caller sites rather than patching arbitrary function prologues or
searching for End's nonexistent `ret`. A private observer still needs:

- Original image SHA, loaded module/base identity and exact site-byte checks;
  installation before execution can reach the sites, with transactional
  rollback. Do not overwrite executing instructions from another thread.
- An x86 ABI-qualified shim preserving the dispatch EAX input, Begin's result,
  live register/flags and floating-point state across logger calls. Establish
  stack alignment, exception/nonlocal-exit handling, return-address visibility
  and original-call exactly-once before using game timings.
- Per-thread bounded binary storage; no hot-path formatting, file I/O,
  allocation or shared logging mutex. Detect overflow, missing pairs, nesting,
  wrong thread, frame changes and failed Begin explicitly.
- Sparse phase-boundary CPU endpoints using the qualified self-counter/clock
  protocol; wall duration alone is not CPU. Join process/thread/frame identity
  to existing source evidence without claiming one frame equals one source.
- Native/Wine ABI and fault controls, unmodified versus inert-hook control,
  then armed observation with a prospectively frozen live population. Preserve
  render/query/lifetime semantics; normal-looking screenshots are insufficient
  to prove the shim transparent or an optimization correct.

Even a successful coarse observer includes game callbacks, PE and synchronous
Wine/Unix work. D3D9 activity can exist outside these three sites. It therefore
cannot replace H628's renderer-root enclosure without a separate coverage
argument, nor provide exclusive game CPU by subtracting unrelated endpoints.

## Consequence for the full objective

Coarse guest boundaries can connect the game's production phases and waits to
downstream Replay/Encode supply, with fewer logging sites than per-COM roots.
This may explain which workload phase feeds H629's expensive draw loop; it
does not establish that loop's default cost or its removable fraction.

Proceed with a bounded observer ABI/installation fixture before touching the
game. Do not start broad DLL logging or insert per-draw strings. In parallel
conceptually, retain the default-transfer and actual-removal gates from H629;
neither is discharged by this static audit. The overall objective stays open.
