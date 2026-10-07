---
domain: present-pacing
workload: Isolated PE32/PE64 native thread-clock service through Sikarugir
title: "Diagnostic clock crosses Wine on the caller's native thread"
type: evidence
status: isolated-clock-bridge-qualified-production-root-attribution-open
updated: 2026-10-08
source: traces/stkcop-root-bridge-h473/{contract.md,packet.h,bridge.c,native.c,probe.c,build.py,run.py,analyze.py,build.json,images.json,run.json,summary.json,i686.jsonl,x86_64.jsonl,i686.stderr,x86_64.stderr}; traces/stkcop-root-bridge-h473/recovered
related: present-pacing-normalized-clock.472.md; present-pacing-visibility-budget.468.md
---

# A usable diagnostic crossing, not yet a game CPU observer

An isolated `h473_clock.dll` / `h473_clock.so` pair exposes one versioned,
cookie-checked 96-byte pointer-free diagnostic packet. Native clock operations
stay in the Unixlib. PE resolves Wine's dispatcher and its own builtin Unixlib
with the same class-1000 lookup used by the production bridge. Production
generated tables, ABI, binaries and source are untouched.

One PE32 and one PE64 headless helper complete under Sikarugir CX24.0.7 using
the existing STALKER prefix, without loading the game or D3D9 renderer. Isolated
builds complete without warnings. Main-thread guest/native identity is stable;
a second guest thread joins to a distinct native identity and consumes over
200 ms native CPU while the caller sleeps. Native CPU is not wrongly assigned
to the idle caller.

The H472 host normalization passes native endpoint envelopes in all three
busy/idle/idle-with-peer phases in each architecture. All 8,192 fast reads are
monotone; their 4,096 disjoint pair deltas fit inside the outer native CPU
budget. These observations test the actual WoW64/x64 crossing, not just a
standalone host executable.

| Helper | Caller busy normalized CPU | Idle caller with busy peer | Whole read-loop CPU/call | Empty pair mean |
|---|---:|---:|---:|---:|
| PE32 | 202.974 ms | 22.208 us | 426.0–427.0 ns | 439.4 ns |
| PE64 | 200.507 ms | 58.375 us | 345.5–346.3 ns | 347.3 ns |

The interval between consecutive clock reads contains one call's remaining
return and the next call's entry, plus fixture work. A selected root using two
reads has two calls of total observer load; the table does **not** price two
calls as one. Loop figures include packet/result storage, first-touch and
fixture bookkeeping. They are neither isolated dispatcher costs nor an
additive observer correction to subtract from D3D9 measurements.

All three newly staged diagnostic files are moved into the experiment's
`recovered/` directory after normal helper completion. All five preexisting
renderer images are hashed before/after and remain byte-identical. The
diagnostic files are recoverable; no user or renderer file is deleted.

## Remaining attribution gate

This closes a concrete missing capability: a host-qualified native CPU reading
is accessible from both PE architectures on the actual calling native thread.
It does not close complete D3D9 API-root coverage, nesting/reload ownership,
sampling error, unmeasured entry/return work or default-path observer transfer.
No game is run or owner ranking claimed. In particular, multiplying an empty
pair cost by a historical call rate is not a measured current-frame budget.

Before a new STALKER attribution run, the root observer needs a finite
coverage/error contract. Keep observer cost uncorrected in any conservative
instrumented upper; do not subtract an average empty hook or transfer a
diagnostic ranking without qualifying its effect on the default workload.
Correctness-qualified removal is still separately blocked by H468/H469's
failed repeatability and indeterminate scene correspondence.
