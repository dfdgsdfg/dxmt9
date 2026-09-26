---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H377: Late audio sources advance through streaming buffers"
type: evidence
status: partial-audio-semantic-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-audio-identities-h377/{snapshot.c,run.py,receipt.jsonl,snapshot-1.txt,snapshot-2.txt,analyze.py,analysis.json}; traces/app-d3d9-stkcop-audio-owner-h375/static.json; experiments/output/app-d3d9-stkcop-bench-audio-identities-h377/result.json
related: present-pacing-audio-workload-growth.376.md
---

# H377: Progressing streams, not visibly stuck OpenAL looping sources

Extend H376's bounded, lock-owner-qualified traversal to preserve each source's
raw 296-byte record. The same-process early/late windows start after observed
frames 603/2701. Each window has 80 balanced suspend/resume/context records.
One early and 68 late snapshots satisfy the inner-loop/list-completeness gates.
Early coverage remains too sparse for a representative distribution.

The decoder records node address, source identifier field at `+0xb8`, state,
looping byte, current buffer, integer/fractional cursor, queue position and
queued/processed fields. Installed disassembly binds `alSourcei`'s `0x1007`
(`AL_LOOPING`) handling to byte `+0x4d`; `alGetSourcei` reads the same byte.
The queued/processed reads at `+0x68/+0x6c` and mixer cursor/queue accesses are
preserved in H375's full static receipt. These offsets are binary-specific.
Node/source names have no captured allocation generation: reused addresses
cannot prove continuous semantic identity or one unchanged game sound event.

| Observation | Early qualified snapshot | Late qualified snapshots |
|---|---:|---:|
| Source nodes | 32 | Same set of 32 addresses |
| Concurrent playing sources | 1 | 24–32 |
| Sources observed playing at least once | 1 | 32 |
| Playing sources with OpenAL looping enabled | 0 | 0 |
| Sources with changing buffer/cursor/queue progress tuple | Not measurable from one snapshot | 32 / 32 |
| Distinct current buffers per playing source | 1 | 2–3 |
| Queued count while playing | 3 | Usually 3; one source also observed at 2 |

Thirty-one late source identities show processed-count values both zero and
one; one shows zero only. All 32 show changing progress tuples, with 40–68
distinct tuples per identity across the sampled window. The tuples combine
current buffer, integer/fractional sample position, queue index and processed
count; they are not independent completion events or an elapsed-time rate.

## Interpretation

There is no observed signature of a frozen cursor, growing source-list leak,
or an OpenAL `AL_LOOPING=true` population left running indefinitely. The
observations instead support active queued streaming across a fixed source
pool. This reinforces H376's workload-growth lead, but does not establish
that every stream is useful or audible.

An application can repeat or restart sounds by requeueing buffers with
`AL_LOOPING=false`. These data cannot exclude duplicated sound events, stale
game-side ownership, inaudible sources consuming work, or address reuse between
snapshots. The same node set does not prove no allocations occurred between
windows. Do not label the 32 streams semantically correct solely because their
cursors advance.

The Day/high 1280x720 `-max-threads 2` diagnostic passes with zero reported GPU
errors and unchanged staged binaries versus H376. No audio settings, DLLs or
runtime code were changed. Intrusive source sampling is not FPS evidence.

## Next discriminator

Further attribution must cross from OpenAL source slots to game sound events:
track creation/restart/queue callers and buffer identity, or first quantify
effective gain/attenuation to find expensive inaudible streams. A sound-source
cap would change quality and cannot yet be promoted as a semantics-preserving
fix. Replacing the audio backend likewise needs an explicitly separate,
sound-qualified comparison rather than an implicit dxmt9 optimization.
