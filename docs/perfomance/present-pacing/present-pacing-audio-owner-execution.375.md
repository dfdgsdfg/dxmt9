---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H375: Audio owner samples concentrate in the locked scalar mixing loop"
type: evidence
status: partial-audio-cost-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-audio-owner-h375/{snapshot.c,snapshot.txt,run.py,receipt.jsonl,analyze.py,analysis.json,summarize.py,summary.json,static.py,static.json}; experiments/output/app-d3d9-stkcop-bench-audio-owner-h375/result.json
related: present-pacing-openal-wake-edge.374.md
---

# H375: Pre-wake work is predominantly wrapper-side arithmetic in this sample

Following H374's wake/return binding, sample only the guest thread whose start
address matches installed `winmm.dll+0x13b00`. A bounded external helper takes
160 guest context snapshots after observed frame 2700, with varied requested
17–31ms gaps. Each snapshot reads the wrapper's global critical-section header
while the selected thread is suspended, then resumes before formatting output.
All 160 have prior suspend count zero, resume result one and valid context.
No Wine sync logging was enabled for this run.

Day/high 1280x720, `-max-threads 2` passes with exit zero and zero reported GPU
errors. All five staged artifact hashes equal H374. No game, Wine or dxmt9
runtime code changes. These are intrusive PC samples, not CPU-time shares,
unbiased wall fractions, exact hold durations or FPS qualification.

## Observed owner execution

| Selected-thread PC class | Samples | Global lock owned by selected thread |
|---|---:|---:|
| `wrap_oal` internal routine RVA `0x1bc30..0x1c363` | 118 | 118 |
| Other `wrap_oal` code | 6 | 6 |
| ntdll wait/dispatch | 36 | 0 |

All 124 lock-owned samples execute in the wrapper; 118/124 (95.2%) are in the
same internal mixing routine. No sampled PC lands in dsound, mmdevapi or a
downstream audio call. That absence does not exclude short downstream calls.
The 36 ntdll samples show the WinMM timer wait stack, not the audio global-lock
wait. A representative stack returns through `winmm+0x13d36` with the global
lock unowned. Thus callback idle time must not be charged as held-lock work.

## Installed-binary evidence

The saved disassembly and binary SHA-256 bind the PCs to code rather than to
misleading nearest exported-symbol labels. Routine RVA `0x1bc30` calls the
known global Enter wrapper at `0x1bc41`, then the matching Leave wrapper at
`0x1c348`. Its hot inner loop includes:

- signed 16-bit sample loads and integer weighted interpolation;
- x87 `fildl`, `fmulp`, `faddp`, `fstps` into interleaved float accumulators;
- sample-index advancement and a backward branch to RVA `0x1bf84`.

Observed hot PCs include `0x1bfd5`, `0x1bfe0`, `0x1c004` and `0x1bfc7`.
This is scalar sample-processing work consistent with mixing/resampling, not
a syscall wait or memcpy-only loop. Do not call this private routine
`alcCreateContext`: that export is merely the disassembler's preceding label.

The caller around RVA `0x89f0..0x8c05` brackets downstream buffer operations
and calls to this routine with the same global lock; its calls at `0x8b6e`
and `0x8b8c` invoke the mixing routine. The inner routine therefore recursively
acquires the outer lock, consistent with observed recursion count two (and
three at some nested OpenAL getter samples). The static shape does not price
downstream calls, and this inspection does not establish exact function names
for the private callback or its indirect calls.

## Decision

Combined with H374, prioritize bundled OpenAL scalar mixing under a global
lock as the concrete pre-wake cost candidate. A generic Wine scheduler fix or
dxmt9 encode parallelism is not the direct next intervention for this evidence.
Do not claim x87/Rosetta translation alone is responsible: algorithmic work,
active sound count, resampling ratios and translation cost are not separated.

Next distinguish workload growth (active sources / output frames per callback)
from per-sample implementation cost. Any audio-provider or sound-quality A/B
needs its own explicit configuration, sound-preservation checks and reversed
observer-OFF runs. This evidence neither authorizes silently replacing a DLL
nor proves audible glitches or an FPS improvement.
