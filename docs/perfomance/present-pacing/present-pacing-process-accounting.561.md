---
domain: present-pacing
workload: Native ARM64 and Rosetta x86_64 CPU accounting fixtures
title: "Process CPU accounting can bound unobserved workers; thread clock needs care"
type: evidence
status: finite-accounting-passed-endpoint-premise-later-rejected
updated: 2026-10-16
source: traces/stkcop-accounting-preflight-h561/{contract.md,followup-contract.md,process_cpu.c,process_cpu_self.c,self_clock.h,thread_clock.c,run.py,followup.py,verify.py,arm64.jsonl,x86_64.jsonl}; traces/stkcop-accounting-preflight-h561/self-oracle/{verification.json,raw-verification.json,process_cpu_self-arm64.jsonl,process_cpu_self-x86_64.jsonl,thread_clock-arm64.jsonl,thread_clock-x86_64.jsonl}
related: present-pacing-owner-clock-rejection.560.md; present-pacing-cpu-counter-freshness.512.md; present-pacing-clear-live.559.md
---

# Feasible process accounting, not a live ownership result

[H565](present-pacing-process-freshness.565.md) subsequently falsifies the
instantaneous `getrusage` endpoint premise. The finite accounting tests below
stand, but they do **not** authorize live process/residual CPU upper bounds.

The whole-process CPU counter offers a conservative way to cover workers not
measured by H559. For one common target interval:

```text
remaining-worker CPU upper
  = whole-process CPU upper
    - whole Primary CPU lower
    - Replay CPU lower
    - Encode CPU lower
```

The residual includes game Secondary, drivers, Rosetta and all other threads;
it bounds any individual unobserved worker only if the intervals, identities,
counter units and endpoint bounds are qualified. It may remain too wide to
establish a largest dxmt9 owner. Concurrent CPU totals are not frame latency.

## Initial fixture rejected on Rosetta

The first fixture used `CLOCK_THREAD_CPUTIME_ID` to stop each worker after
120 ms of its own CPU. ARM64 completed; x86_64 hit the three-second deadline
and exited 3. Preserve this failed population.

A separate twelve-sample busy probe compared that clock with enclosing
calling-thread `proc_pidinfo` readings:

| Architecture | Thread clock delta | Self-counter delta bracket |
|---|---:|---:|
| ARM64 | 550.123417 ms | 550.117–550.137 ms |
| Rosetta x86_64 | 13.204655 ms | 550.172–550.204 ms |

The Rosetta API value does not agree with the current self-counter units.
No fitted multiplier is used and no underlying OS cause is claimed. Existing
H514/H518 observers use self `proc_pidinfo`, not this thread-clock API.

## Self-counter follow-up

A separate wrapper reuses the unchanged original C fixture, replacing only
the worker CPU oracle with self `proc_pidinfo`. Two rounds of 0/1/2/4 workers
run on each architecture. Workers park after their measured CPU region;
process clocks are sampled while they run, while parked and after thread exit.

All **16 cases / 160 process samples** pass independent raw-row verification:

- `CLOCK_PROCESS_CPUTIME_ID` and `getrusage(RUSAGE_SELF)` remain monotonic.
- `getrusage` falls inside enclosing process-clock reads with the predeclared
  2 us timeval quantization allowance.
- Process CPU covers the sum of worker self-CPU, including concurrent workers,
  with the predeclared 4 us endpoint allowance. All observed residuals are
  actually positive.
- Counters progress while workers are live and retain their CPU after exit.

For four Rosetta workers, the summed measured worker CPU is about 480 ms in
125–127 ms wall time. Process-minus-worker residuals are 0.393–0.467 ms across
the two cases, including main-thread and worker setup/parking costs. This is
an accounting check, not a measured production observer overhead. Maximum
process-clock read brackets are 53 us ARM64 and 44 us x86_64; individual
syscall calls must not be assumed instantaneous.

## Integration gate

Use coarse identity-qualified process checkpoints enclosing the selected
spatial window, then subtract known threads' lower bounds within that same
window. Do not reuse a single-thread `CPU delta <= wall delta` interpolation
rule for whole-process CPU: multiple threads can consume CPU concurrently.
Do not clamp a negative residual and call it evidence; reject inconsistent
accounting. Production endpoint quantization/freshness and observer transfer
remain separate qualifications.

H560's explicit clock-domain repair precedes a new live ownership episode.
No Wine run, renderer modification, default promotion or FPS result was made
in this native-only step.
