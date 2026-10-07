---
domain: present-pacing
workload: STALKER CoP Day/high 1280x720, max-threads 2, snd_targets 16
title: "Default tail CPU: same-run guest/native identity joins"
type: evidence
status: game-helper-identified-dedicated-worker-ranking-bounded
updated: 2026-10-07
source: traces/stkcop-thread-identity-h462/{run.py,guest.c,guest.exe,guest32.exe,analyze.py,calibrate.c,calibrate,calibrate.py,calibration.json}; traces/stkcop-thread-identity-h462/{default-r1,default-r2}
related: present-pacing-primary-inplace-bound.460.md; present-pacing-xray-worker-yield.369.md; present-pacing-stalker-cross-renderer-waits.406.md; present-pacing-completed-workload-visibility.455.md
---

# The unnamed leader is an X-Ray helper; Encode and Replay are close

H460's unnamed native CPU leader must not be assigned to Replay by a repeated
handle value. H462 establishes same-run guest/thread/stack connections while
keeping the archived default renderer binaries unchanged. No H460 marker or
runtime observer is enabled. This is an external identity/CPU diagnostic,
not an observer-OFF FPS comparison.

## Identity chain

Installed Sikarugir CX24.0.7 x86_64 `ntdll.so` SHA-256:
`81f81cf1bb70069e94fa89d53fb7327cfb52fb9629c5b84134c851e681a20235`.
At image RVA `0x54009`, pthread_self is called; `0x5400e` stores its result at
TEB64 + `0x3a0`. The same TEB is published with the TLS key used by
NtCurrentTeb. Cleanup reads that field and calls pthread_join. A 64-bit helper
uses ThreadBasicInformation and read-only target memory access to obtain it.
Client PID/TID, successful full reads and native process identity are checked.

The pthread value is not literally the libproc handle. A standalone Rosetta
x86_64 main-thread/created-thread calibration observes
`libproc handle = pthread_self + 0xe0`. Seven nonzero target guest threads
have unique matches using that relation at early/pre-tail/post-tail censuses,
including independently matching named audio threads. This is specific to the
installed runtime/platform; do not generalize the offset to another pthread
implementation. The initial Primary's stored value is zero and is rejected.
The separate 32-bit helper supplies guest module ranges and start-address
ownership; the 64-bit module list does not cover the required 32-bit modules.

For r2, native process identity is `[78768,1791380434,626431]`, guest PID 196.
Post-tail native stack TIDs are joined to libproc handles using unique
same-process counter values bracketed by two complete snapshots:

| libproc handle | Native TID | Same-run evidence |
|---|---:|---|
| `17248866528` | 60467972 | Guest TID 264; start `xrCPU_Pipe.dll+0x3730` |
| `17256571104` | 60468717 | Named `dxmt9-encode`; encodeChunk stack |
| `17258180832` | 60468792 | ReplayOffloadWorker::start/run stack |

The helper's post-tail guest context lies in ntdll; the first stack words map
to kernelbase, kernel32, and `xrCPU_Pipe+0x3773`. The installed worker's indirect
yield call returns exactly to `+0x3773`. The same native TID has 33 of 40
post-tail observations in NtYieldExecution → cthread_yield. This joins H369's
already-known game helper idle protocol to the present native CPU leader.
It is not a newly discovered dxmt9 worker. The worker can also run callbacks;
neither the sampled yield population nor its whole CPU delta prices every
cycle as removable idle work.

## Tail CPU and the precise worker comparison

R2 has 18 surrounding and 16 wholly contained native snapshots in the requested
102–110 s window. The 27 nonzero persistent handles are unique with monotone
user/system counters. Zero handles and unobserved transient threads are not a
complete census. Whole-window contained/enclosing bounds are:

| Owner | CPU seconds |
|---|---:|
| X-Ray helper above | 6.997501–7.925938 |
| Primary, including game/Wine/renderer | 6.969415–7.913489 |
| Encode | 4.222515–4.794626 |
| Replay | 4.107525–4.665180 |
| WinMM-start thread, post-tail wrap_oal context | 3.481010–3.941539 |

The Encode/Replay bounds overlap for the full eight seconds. A common
subinterval bounded by the first contained snapshot's end and the last one's
start is **102.088526–109.827471084 s**, duration 7.738945084 s. Each returned
CPU delta includes at most the sum of endpoint snapshot widths outside this
common interval, 0.000178750 s, assuming the ordinary maximum single-thread
rate of one CPU second per wall second. Subtract that sum for the lower bound:

| Dedicated worker | CPU in the common interval |
|---|---:|
| Encode | 4.222336250–4.222515000 s |
| Replay | 4.107346250–4.107525000 s |

Encode exceeds Replay by at least 0.114811250 CPU seconds in this interval,
about 2.8% of Replay's cost. This is not an ordering proof for the excluded
edges or every late frame. Nearby SASH observations advance by 139; division
gives approximately 30.4 and 29.6 CPU ms per observed increment, not exact
frame-boundary attribution. No times from overlapping threads are added into
frame latency.

## Failed first attempt and measurement limits

R1 exits normally but its doubled-backslash native-TID regex yields an empty
native join. Preserve that failure; r2 fixes the parser and requires nonempty
TIDs. R1 also lacks a 32-bit module census. It is not used for the identity
conclusion above. R2 performs no thread suspension before the CPU window;
brief guest context capture and native sample collection occur after 111 s.
Early and pre-tail censuses are read-only. External observer scheduling cost
still exists. Post-tail collection delays stop observation, so the observed
123-second elapsed duration is not a benchmark FPS denominator.

Both runs restore shared Wine D3D9 SHA prefix `534ea6f17128354e`; the Unix
provider remains `7f45561ddf0fd22f`. H461's local native stretch fixes are not
staged into these runs. Tail screenshots are spot checks, not whole-scene
equivalence. Installed xrCPU_Pipe SHA matches H369 exactly:
`4f13b61ade8ee04585c03631662fce44b09b105a1e0d188e96e11c03c7999303`.

## Decision for the original CPU objective

Do not pursue the seven-second helper as a newly discovered renderer cost or
change Wine/game yielding as a dxmt9 optimization. The leading identified
dedicated dxmt9 workers are close: a large Encode-only CPU saving need not
produce a comparable pipeline throughput gain. H455's real-visibility
candidate remains relevant because it can reduce incoming work across stages.

The full objective is not closed: default Primary renderer cost still lacks
an unconditional separation from game/Wine, and H455's 4.60–6.11 ms/frame
Encode reductions are not yet a correctness-qualified removable budget.
H460's conditional marker bound must not be transferred silently. This leaf
closes the newly raised unnamed-worker identity gap, not those two gates.
