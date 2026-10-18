---
domain: present-pacing
workload: Native ARM64 and Rosetta continuously busy workers
title: "Own-process getrusage also fails instantaneous CPU endpoint qualification"
type: evidence
status: process-endpoint-assumption-falsified
updated: 2026-10-16
source: traces/stkcop-process-freshness-h565/{contract.md,probe.c,run.py,result.json,arm64.jsonl,x86_64.jsonl}; traces/stkcop-owner-live-h564/qualification.json
related: present-pacing-process-accounting.561.md; present-pacing-owner-live.564.md; present-pacing-cpu-counter-freshness.512.md
---

# Aggregate accounting is not endpoint freshness

H561 correctly demonstrates finite whole-process accounting and retention of
exited-thread CPU. It does not establish that a `getrusage(RUSAGE_SELF)` value
belongs within the narrow syscall wall bracket. H565 directly challenges that
separate premise and falsifies it on both architectures.

One and four workers continuously consume CPU for two seconds. The main thread
samples its own process CPU every 250 us, enclosed by scaled
`mach_absolute_time`. Workers remain alive through the last sample. Each
adjacent CPU delta is compared with **eight times the full enclosing wall
interval**, plus 4 us quantization: even the entire machine cannot supply more
process CPU inside that interval. This loose ceiling does not require guessing
how many Rosetta helper threads exist.

| Architecture / busy workers | Samples | Capacity-ceiling violations | Largest CPU jump | Enclosing wall |
|---|---:|---:|---:|---:|
| ARM64 / 1 | 5,230 | 192 | 10.054 ms | 0.383 ms |
| ARM64 / 4 | 5,161 | 611 | 40.021 ms | 0.389 ms |
| Rosetta / 1 | 5,216 | 228 | 10.133 ms | 0.382 ms |
| Rosetta / 4 | 5,137 | 592 | 36.618 ms | 0.391 ms |

The broad total CPU accounting remains plausible: approximately two CPU
seconds for one worker and eight for four. The failure is the **timestamp
authority of intermediate samples**, not proof that the totals are meaningless.
Monotonicity and agreement with another aggregate API do not repair that gap.

## Consequence

- Reject H564's process/residual upper-bound interpretation. Preserve its
  raw arithmetic and separate self-thread/root evidence.
- Do not infer a universal staleness allowance from these finite maxima or
  silently add a few milliseconds to make the bound pass.
- Do not switch to `CLOCK_PROCESS_CPUTIME_ID` merely because H561 observed
  agreement with getrusage; that is not independent freshness evidence.
- Cover remaining renderer workers through qualified self-thread endpoints,
  with explicit thread identity and lifecycle coverage. External sampling can
  help locate work but must not replace a certified CPU endpoint.

No game rerun, product-code change, system-service termination or runtime
promotion occurs in this check.
