---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H378: Many sampled late streams mix at small nonzero coefficients"
type: evidence
status: partial-audibility-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-audio-gain-h378/{analyze.py,analysis.json}; traces/app-d3d9-stkcop-audio-owner-h375/{snapshot.txt,static.json}; traces/app-d3d9-stkcop-audio-workload-h376/{snapshot-1.txt,snapshot-2.txt}; traces/app-d3d9-stkcop-audio-identities-h377/{snapshot-1.txt,snapshot-2.txt}
related: present-pacing-audio-source-progress.377.md
---

# H378: Low-gain mixing is observed, but inaudibility is not established

Reuse H375–H377's preserved stack snapshots instead of another invasive run.
Only the known push-free mixing loop at RVA `0x1bf84..0x1c0d4`, valid contexts,
balanced suspend/resume and matching lock ownership qualify. Decode the four
float words at ESP + `0x44/0x48/0x4c/0x50`, and the saved source node at + `0x38`.

The saved installed-binary disassembly explicitly loads those four words into
the x87 stack immediately before the sample loop (`0x1bf74..0x1bf80`). The loop
multiplies interpolated samples by them and accumulates into two interleaved
float arrays. The preceding helper at `0x1b370` computes outputs through the
corresponding stack pointers; it consults source/listener and spatial state.
These are applied mixing coefficients, not merely raw `AL_GAIN` settings.
No recomputation of attenuation or private-structure guess is needed.

For each snapshot, classify by the maximum absolute value of **all four**
coefficients, avoiding misclassification from a quiet single output channel.

| Late capture | Qualified snapshots | Sampled source nodes | Peak coefficient median | All coefficients <0.01 | All coefficients <0.001 | All zero |
|---|---:|---:|---:|---:|---:|---:|
| H375 | 118 | 31 | 0.006102 | 90 | 4 | 0 |
| H376 | 62 | 30 | 0.005242 | 44 | 1 | 0 |
| H377 | 68 | 30 | 0.005246 | 52 | 1 | 0 |

Thus 71–76% of qualified late snapshots execute with maximum coefficient below
0.01 (−40dB relative to unity multiplication). Median maxima correspond to
about −44 to −46dB. Peaks range from 0.000405 to 0.804 across these captures.
Early H376/H377 have only 2/1 qualified samples, with maxima 0.0256–0.1414;
that is too little evidence for a representative early/late gain comparison.

## Interpretation and limits

This is direct evidence that small-coefficient streams still execute the full
scalar sample loop while owning the global lock. It identifies work worth
studying for a separate audio implementation or explicit quality tradeoff.
It does **not** establish that the sounds are inaudible or safe to discard:

- coefficient dB is not output dBFS, acoustic loudness or signal-to-noise ratio;
- source PCM amplitude, downstream gain, accumulation and correlated streams
  affect the output, and no audio waveform was captured;
- PC sampling is duration-biased; snapshot counts are neither distinct callback
  counts nor CPU-time/FPS savings;
- source node addresses lack allocation generations and some nodes are not
  sampled, so these are not exact per-voice lifetime statistics;
- no snapshot has all four coefficients exactly zero, so a strictly zero-gain
  arithmetic skip has no demonstrated opportunity here.

There was no new wild run, runtime change, provider replacement or sound-setting
change. Historical run qualification remains in H375–H377. The offline decoder
rejects missing stack fields and nonfinite coefficients and preserves bit
patterns alongside decoded floats. Artifact audit and diff checks apply.

## Next decision

Do not silently cap voices or introduce a nonzero culling threshold as a
semantics-preserving dxmt9 fix. For causal performance evidence, use a separately
declared audio-quality intervention with an original baseline and sound checks,
or compare an explicitly selected audio provider with compatible behavior.
Further observational attribution can bind source/queue callers to game events
and measure actual PCM contribution, but these data alone cannot prove which
streams are redundant. The established bottleneck lead is outside dxmt9's
rendering pipeline; no encode/queue change follows from it.
