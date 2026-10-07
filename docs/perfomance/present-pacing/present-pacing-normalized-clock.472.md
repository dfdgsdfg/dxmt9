---
domain: present-pacing
workload: Host-qualified arm64/Rosetta native thread CPU normalization
title: "Hardware-frequency normalization passes bounded native CPU envelope checks"
type: evidence
status: synthetic-host-clock-normalization-qualified-not-attribution
updated: 2026-10-08
source: traces/stkcop-normalized-clock-h472/{contract.md,probe.c,run.py,analyze.py,receipt.json,summary.json,arm64-intervals.json,x86_64-intervals.json,arm64.jsonl,x86_64.jsonl,arm64-build.log,x86_64-build.log}
related: present-pacing-posix-clock-recheck.471.md; present-pacing-root-clock-feasibility.470.md; present-pacing-root-clock-bridge.473.md
---

# A source-derived clock conversion, not a fitted timing multiplier

H471 rejects interpreting Rosetta POSIX thread-clock results directly as
nanoseconds. H472 tests a separate normalization hypothesis on macOS 27 build
26A428, Apple M1. No renderer or Wine files are changed by this experiment.

Apple's published [Libc clock implementation](https://github.com/apple-oss-distributions/Libc/blob/71bbe350ab79eef58113991d817ccc6165061a64/gen/clock_gettime.c)
converts `__thread_selfusage` with the process Mach timebase. XNU revision
`f6217f891ac0bb64f3d375211650a4c1ff8ca1ea` connects `thread_selfusage` in
`bsd/kern/kern_resource.c` to `thread_get_runtime_self` in `osfmk/kern/thread.c`
and current user/system Mach runtime accounting in `osfmk/kern/recount.c`.
`bsd/kern/kern_mib.c` supplies `hw.tbfrequency` from the platform timebase.
These sources explain a hypothesis; they do not authenticate the installed
Libc/Rosetta implementation. No Apple implementation is copied into project code.

For raw POSIX value L, reported process timebase n/d, and independently queried
hardware frequency F, the diagnostic transformation is:

`N = floor(L * d * 1,000,000,000 / (n * F))`.

Use a single wide-integer calculation, not staged truncation or a fitted
floating-point ratio. Both measured processes report F = 24,000,000 Hz.
arm64 reports n/d = 125/3; Rosetta reports 1/1. Thus this host's transformation
is identity on arm64 and 125/3 on Rosetta. This is not a portable replacement
for the documented POSIX API and does not retroactively qualify H355/H471 data.

## Finite qualification

At each endpoint the probe reads native libproc CPU, then POSIX CPU, then native
CPU. The normalized difference must fit the enclosing native interval with the
declared 2 us allowance for two integer-microsecond user/system reference fields.
No scale or tolerance is adjusted after collection.

Each architecture retains 162 intervals: twenty each at seven busy durations
from zero through 100 ms, twenty syscall-heavy 1 ms intervals, one idle 200 ms
interval and one idle interval with a separately measured busy peer. Transcript
identity/population and endpoint monotonicity checks pass.

| Architecture | Normalized failures | Unmodified POSIX failures | Busy-peer native CPU |
|---|---:|---:|---:|
| arm64 | 0 / 162 | 0 / 162 | 204.415 ms |
| Rosetta x86_64 | 0 / 162 | 122 / 162 | 200.148 ms |

Tiny intervals have envelopes too wide to reject every incorrect raw value;
the raw negative control is discriminating on longer intervals. Both fixture
builds warn about the deprecated `syscall(SYS_getpid)` used only to generate
syscall-heavy work. It succeeds here and is retained as experiment evidence,
not adopted in runtime code. Summed native CPU does not separately quantify
the kernel contribution.

## Accepted scope

Accept normalization only for this host/architecture/synthetic population.
Reference envelopes do not prove sub-microsecond accuracy, clock-portability,
every interval's error, PE boundary overhead, default-path perturbation or
complete API-root coverage. The source-unit model and actual observed values
support a better clock candidate; they do not establish the largest renderer
CPU owner or its removable amount.

[H473](present-pacing-root-clock-bridge.473.md) separately checks an isolated
PE32/PE64 Wine crossing before considering any real D3D9-root instrumentation.
