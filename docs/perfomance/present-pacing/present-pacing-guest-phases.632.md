---
domain: present-pacing
workload: STALKER guest-phase observer preparation, synthetic PE32 and native tests
title: "H632 — qualify continuation and fail-closed phase pairing, including inactive bypass"
type: evidence
status: continuation-and-reducer-qualified-live-observer-open
updated: 2026-10-16
source: traces/stkcop-guest-phases-h632/continuation/{fixture.S,shim.S,test.c,build.json,run.json,fixture.stdout,mutant-replay.stdout,mutant-fp.stdout,mutant-flags.stdout}; traces/stkcop-guest-phases-h632/{phases.hpp,phases_spec.cpp,native.py,native.json,static.py,static.json,verify.py,verification.json,continuation-disassembly.txt}
related: present-pacing-guest-marker.631.md; present-pacing-guest-boundaries.630.md; present-pacing-owner-union-live.629.md
---

# H632 — a continuation is not necessarily a completed render attempt

H631's before-call marker now has a separately qualified continuation marker.
The original engine continuation is a six-byte `mov esi,[edi+0x398]` at RVA
`0x538ac`; the marker restores guest state, executes that displaced instruction
once and resumes at `0x538b2`. The instruction has no PC-relative operand.
The emitted PE code is checked against the exact original instruction bytes.

A read-only recheck also pins a distinct incoming path: `je` at `0x53840`
reaches this continuation **before calling Begin** when the device is inactive.
The already known `je` at `0x5384b` reaches it after failed Begin. Decoded
direct incoming edges do not enter the displaced instruction's interior.
Indirect incoming edges remain non-exhaustive. Original game images are
unchanged; no game process, runtime hook or performance population is started.

## Synthetic PE32 continuation result

The fixture varies normal return, a synthetic tail path and call bypass; all
128 arithmetic/DF flag patterns are compared across original, entry-only,
continuation-only and combined-marker arms. The original return address remains
visible to the target. The displaced load's ESI result and a stable caller-owned
stack sentinel are checked explicitly.

| Qualification | Result |
|---|---|
| Three paths × 128 flag patterns × three instrumented comparisons | 1,152 paired state cases pass |
| Entry/continuation/target invocation counts | Match each arm and path |
| Wrong sixth displaced byte | Rejected without changing the instruction |
| Restoration of original call and continuation | Pass |
| Displaced-load omission | Rejected, ESI stays `0x66778899` instead of `0xfaceb00c` |
| Continuation FP-restore omission | Rejected by FP comparison |
| Continuation flags/register corruption | Rejected by flags and register comparisons |

These are actual Sikarugir Wine runs of a private synthetic PE32 executable,
not a replay of the game. Sources, commands, executable hashes, stdout and
stderr are frozen under `continuation/`. No d3d9/provider image is staged, and
postflight finds no Wine processes. H631's observer exception, AVX, general
unwind and concurrent-installation exclusions still apply. The displaced load
is tested on valid memory, not for equivalent fault-PC behavior on invalid EDI.

## Phase reducer and bounded storage

The private reducer recognizes the following **observed event** grammar:

| Events | Classification |
|---|---|
| Begin → Dispatch → End → Continue | Normal render-path closure, not proof of one Present or pixel correctness |
| Begin → Continue | Begin-failed path, assuming the authenticated installed sites and loss-free event stream |
| Continue without Begin | Unpaired continuation; no render/Begin CPU duration is manufactured |
| Any incomplete or out-of-order attempt | Reject attribution |

The complete-word reference grammar is `(C | BC | BDEC)*`; it is independent
of the implementation's transition table. Single-writer fixed-capacity storage
rejects overflow without overwriting earlier rows. Process birth, process/thread
identity, observer epoch, event ordinal and monotonic stamp checks latch faults.
Readers are permitted only after writer quiescence; this is not a concurrent
ring-buffer protocol. `finish()` must succeed before interpreting any retained
rows. An unpaired continuation is not silently labelled inactive: incomplete
installation or a missing hook invocation would require separate diagnosis.

Native arm64 and x86_64 each pass all 87,381 event words of length zero through
eight against the grammar, plus ten explicit identity, sequence, clock,
capacity, incomplete-tail, unknown-kind and overflow corruptions. Both
architectures reject the intentional missing-End acceptance and identity-check
omission mutants. This bounded enumeration is not an unbounded proof or a
live model-code binding: the reducer is not yet called by a game observer.

## Consequence and next gate

This removes two concrete pre-live uncertainties: the six-byte continuation's
normal execution and ambiguous closure when Begin was never entered. It does
not identify the default-path largest CPU owner or any removable budget.

The remaining integration is substantive: connect the marker callbacks to
preallocated per-thread event storage and the qualified CPU/clock service;
qualify their clean FP environment, nonthrowing behavior, reentrancy and
identity binding; then install all sites before execution can reach them with
transactional rollback. Finally compare unmodified, inert-marker and armed
controls in a fixed live population. Do not infer hook transparency from the
separate native reducer or from normal-looking game frames.

For the main objective, coarse guest phases are only an enclosing attribution
aid. They include game and synchronous Wine/Unix work, do not cover every D3D9
root, and cannot establish a dxmt9-exclusive cost by themselves. H629's default
transfer and a correctness-qualified removal intervention are still required.
