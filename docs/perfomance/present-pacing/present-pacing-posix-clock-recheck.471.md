---
domain: present-pacing
workload: Standalone current-OS arm64/Rosetta POSIX thread CPU clock revalidation
title: "Rosetta POSIX thread CPU still returns a unit-mismatched value"
type: evidence
status: rosetta-documented-nanosecond-gate-rejected
updated: 2026-10-08
source: traces/stkcop-posix-clock-h471/{contract.md,probe.c,run.py,analyze.py,receipt.json,summary.json,arm64.jsonl,x86_64.jsonl}
related: present-pacing-root-clock-feasibility.470.md; present-pacing-prepared-comparison.355.md
---

# The old POSIX clock exclusion still applies on the current host

H470 does not qualify libproc for short-root estimation. Before treating the
public `CLOCK_THREAD_CPUTIME_ID` API as an alternative, H471 rechecks H355's
retained Rosetta units failure on the current macOS 27 host. It performs one
standalone run per architecture, three 200 ms phases, and a 100,000-read loop.
No game, Wine staging, renderer source, ABI or runtime policy is changed.

| Phase | arm64 native CPU / POSIX value | Rosetta native CPU / POSIX value |
|---|---:|---:|
| Busy caller | 200.008 ms / 199.997 ms | 200.004 ms / 4.800 ms |
| Idle caller | 19 us / 17.125 us | 84 us / 1.846 us |
| Idle caller with busy peer launched | 12 us / 9.083 us | 27 us / 0.544 us |

The Rosetta busy value fails the declared 5% + 100 us documented-nanosecond
agreement check. Its ratio is again approximately 0.024, or 1/41.67. The
in-process Mach timebase query reports 125/3 on arm64 but 1/1 on Rosetta;
blindly applying the Rosetta process's reported timebase does not correct it.
This is observed behavior, not a claim about the underlying kernel cause.

The timed read loop consumes about 133 ns/read native CPU on arm64 and
287 ns/read on Rosetta, including loop and sample-storage work. Faster reads
do not fix incorrect units. Raw absolute values and native endpoints are
retained. These phases do not qualify individual short-root error, kernel/user
coverage or peer CPU accounting: the peer is launched but its CPU is not
separately measured, and no positive cross-thread isolation claim is needed
to reject the busy-phase units mismatch.

Do not multiply historical/current clock values by a fitted 41.67 factor and
call them qualified CPU. A calibrated normalization would be a separate
measurement contract requiring evidence of units, identity, interval error
and runtime applicability. No such correction or native clock bridge is
implemented here. Coarse libproc CPU totals remain the accepted authority.

The original default-path maximum-owner and correctness-qualified removable
budget objective is still unresolved. H470/H471 narrow the missing capability;
they do not replace it with an Encode-only ranking or a synthetic CPU budget.
