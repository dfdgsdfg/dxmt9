---
domain: present-pacing
workload: Current-host ARM64/Rosetta self-query accounting preflight
title: "Self-query accounting passes the current-host endpoint fixture"
type: evidence
status: synthetic-self-query-qualified-live-owner-unresolved
updated: 2026-10-14
source: traces/stkcop-self-counter-preflight-h513/{probe.c,run.py,receipt.json,arm64.jsonl,x86_64.jsonl,arm64-build.txt,x86_64-build.txt,native_only.c,native_only.py,native-only-receipt.json,arm64-native-only.jsonl,x86_64-native-only.jsonl,arm64-native-only-build.txt,x86_64-native-only-build.txt}; src/dxmt9/dxmt9_cpu_supply_trace.cpp; traces/stkcop-root-bridge-h473/native.c
related: present-pacing-cpu-counter-freshness.512.md; present-pacing-normalized-clock.472.md; present-pacing-root-clock-bridge.473.md; present-pacing-root-game.477.md
---

# Separate self-reporting from external stale-counter reads

H512 falsifies instantaneous **external** thread-counter freshness. It does not
establish that a counter queried by the running target thread is equally stale.
H513 tests that distinction on the current macOS 27.0.1 build 26A434, without
Wine, a game episode, renderer changes or a profiler.

## Fixed qualification and negative control

Two freshly built fixtures, ARM64 and x86_64/Rosetta, each execute 100 intervals
in seven fixed classes: empty, 20 us/500 us/5 ms busy, 1 ms syscall-heavy,
1 ms idle and 1 ms idle with a separately measured busy peer. Rows are stored
in fixed storage and printed after the measured work.

Every endpoint is read **by the measured thread** in this order:

wall-before → POSIX self CPU → libproc self CPU → POSIX self CPU → wall-after.

The existing H472 unit hypothesis is rechecked, not refitted: hardware frequency
24 MHz, process Mach timebase 125/3 on ARM64 and 1/1 under Rosetta. Thus the
POSIX result is unchanged on ARM64 and multiplied by 125/3 under Rosetta using
one integer operation. The libproc self counter retains its reported nanosecond
units. This is host-qualified diagnostic normalization, not a portable API fix.

| Architecture | Self-counter endpoints inside normalized POSIX brackets | CPU intervals within enclosing wall | Unnormalized POSIX endpoint failures |
|---|---:|---:|---:|
| ARM64 | 1,400 / 1,400 | 700 / 700 | 0 |
| x86_64/Rosetta | 1,400 / 1,400 | 700 / 700 | 1,400 |

The predeclared allowance is 2 us for the two integer-microsecond user/system
fields; no tolerance was fitted after collection. All counters are monotone.
An independent rational-arithmetic check of the raw rows reproduces the results.
Both builds are warning-free.

An additional finite test addresses a confound in that bracket: the preceding
POSIX call could itself refresh kernel accounting before the libproc call.
`native_only.c` instead performs busy/idle work, reads **libproc first**, then
reads POSIX CPU. Each architecture supplies 400 such endpoints. The normalized
CPU advance to the subsequent POSIX reading must fit the enclosing wall bracket
plus the unchanged 2 us allowance. All 800 endpoints pass. This distinguishes
a self-libproc query from merely reading a counter primed by an earlier POSIX
CPU syscall. Its source, binaries and transcripts are separate; the first
fixture's evidence was not overwritten.

In the idle-with-peer population, the caller reports 0.474 ms CPU over about
148.56 ms enclosing wall on ARM64, and 0.604 ms CPU over about 150.89 ms wall
under Rosetta. The busy peers separately report 148.342 and 150.646 ms CPU.
The self counter does not charge the peer's busy time to the sleeping caller.
The 100 five-millisecond busy intervals report approximately 499 ms CPU in
each architecture. These are fixture checks, not game performance.

## Consequence for existing instrumentation

`src/dxmt9/dxmt9_cpu_supply_trace.cpp`, `record`, obtains the calling native
thread ID and passes it to `nativeCpuNs` on that same thread. It is a **self
query**, unlike H509/H510's external process observer. H473's diagnostic bridge
also reads its actual calling thread. Do not invalidate all those self-reported
CPU values merely by citing H512's external-counter counterexample.

This does not retroactively qualify every old trace. Their own identity,
clock-version, units, observer overhead, nesting, source alignment and default-
transfer requirements still apply. H477's all-call diagnostic remains rejected
on default transfer, and unnormalized Rosetta POSIX values remain unqualified.

## Next measurement boundary, not a completed owner result

The supported next mechanism is a **low-frequency target-thread self report**,
published with process/thread/source identity and wall-time brackets. Replay
and Encode can report at their own source or frame boundaries; an external
reader must not invent a common instant or interpolate across unmatched cuts.
Primary still needs a distinct D3D9-root/non-root authority: a whole-thread
self report alone does not separate game, Wine, PE and synchronous Unix work.

Before a live run, the reporter must reject stale requests, identity reuse and
missing endpoints, and establish how corresponding work boundaries are joined.
Do not expand the existing per-event supply trace or repeat the all-call root
clock campaign just to obtain two coarse endpoints. There is no hot-path clock
change, new runtime switch or live run in H513.

The finite fixture supports proceeding with self-report boundary design; it
does not prove a universal clock-error bound or observer-free transfer. The
largest default dxmt9 owner and correctness-qualified removable amount remain
unresolved. Canonical native/provider SHA-256 values remain unchanged.
