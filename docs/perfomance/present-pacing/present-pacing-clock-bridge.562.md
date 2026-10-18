---
domain: present-pacing
workload: Sikarugir PE32/PE64 diagnostic Unix-call bridge
title: "Explicit clock domains pass the actual PE bridge preflight"
type: evidence
status: bridge-domain-preflight-passed-live-owner-open
updated: 2026-10-16
source: traces/stkcop-clock-bridge-h562/{contract.md,packet.h,native.c,bridge.c,probe.c,domain.hpp,domain_spec.cpp,build.json,run.json,run-v2.json,verification.json,verify.py,i686-v2.stdout,x86_64-v2.stdout}
related: present-pacing-owner-clock-rejection.560.md; present-pacing-process-accounting.561.md; present-pacing-owner-domains.563.md
---

# Explicit-domain bridge qualification

H562 separates absolute wall, continuous wall, self-thread CPU and process CPU
in a 160-byte pointer-free diagnostic packet. Each call carries two
absolute/continuous/absolute mapping triplets, native PID/birth/TID and
absolute before/after brackets for both CPU counters. Self CPU uses calling
thread `proc_pidinfo`; process CPU uses `getrusage(RUSAGE_SELF)`.

The actual Sikarugir PE32 and PE64 bridge fixtures both exit 0:

| Check | PE32 | PE64 |
|---|---:|---:|
| Calls with outer ordered RDTSC and native continuous enclosure | 64/64 | 64/64 |
| Null/magic/version/size/reserved schema rejections | 5 | 5 |
| Common continuous-minus-absolute mapping | 10,024,639,924,875 ns | 10,024,639,924,875 ns |
| Self CPU delta | 453.655 ms | 455.052 ms |
| Process CPU delta | 453.655 ms | 455.051 ms |

The microsecond process/self difference is covered by declared counter
quantization; the intervals were not used as performance comparisons. A common
singleton offset in this finite population is not universal clock precision.
Identity and cumulative counters remain consistent and monotonic. The existing
100 ns RDTSC endpoint allowance was not enlarged.

The pure mapping reducer also passes ARM64 and x86_64 native cases for explicit
large offsets, stale identity, changed offset (synthetic sleep), backward time
and arithmetic underflow/overflow. No actual machine sleep was induced.

The first launcher attempt failed before process creation because this installed
runtime has `bin/wine`, not `bin/wine64`. Its receipt is preserved as `run.json`;
three temporary files were recovered before the corrected `run-v2.json` attempt.
That correction is not a retry of a game-performance population. After both
fixtures and the Wine server exited naturally, all three temporary services were
moved back into evidence storage. Five renderer file hashes stayed unchanged.

This resolves the PE bridge prerequisite exposed by H560. It does not establish
root-observer overhead, current renderer Primary cost or default-live removal.
