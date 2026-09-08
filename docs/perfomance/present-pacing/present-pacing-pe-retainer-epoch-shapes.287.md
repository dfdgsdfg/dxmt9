---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Sampled PE retainer expiry and rebuild shapes"
type: evidence
status: diagnostic-attribution
updated: 2026-09-08
source: experiments/output/pe-retainer-epoch-shapes-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-retainer-epoch-a285c4ba-gt2-n127-20260908; experiments/output/app-d3d9-3dmark{05,06}-pe-retainer-observer-a285c4ba-gt2-off-perf-r1-20260908
related: present-pacing-pe-binding-wire-authority.286.md; present-pacing-pe-layout-dynamic-economy.284.md; ../../../specs/d3d9/recorder/requirements.md
---

# Sampled PE retainer expiry and rebuild shapes

H287 closes the missing retainer-shape attribution in
[H286](present-pacing-pe-binding-wire-authority.286.md), without changing
retention, release, rollback or index maintenance. In the sampled GT2
population, expiry-driven rebuilding dominates; rollback tombstones do not.
This identifies a measurable candidate, not an implemented CPU saving.

## Observation boundary

R-CORE-REC-7.63 is implemented by `D3D9PePendingCommandRetainer::epochSnapshot`,
the pure `projectRetainerEpoch`, and the existing observed settlement in
`src/d3d9/d3d9_pe_semantic_owner.hpp`. Snapshots read existing scalar facts;
they contain no resource pointers, entry copies or ownership. The projector
checks entry/age/index conservation and unsigned epoch advance, including wrap.

Only sampled successful, nonempty owner settlements take before/after
snapshots. The existing `SettleRetainerEpoch` child timer encloses `endEpoch`
but not snapshots or shape aggregation. Those additions remain inside the
inclusive settlement parent and can perturb caches. N=0 and the ordinary
unobserved body do not construct snapshots. No new runtime knob is added:
the existing semantic-owner phase observer and PE decimation gates own this
optional cold storage.

A 256-row distinct-shape table records disposition, input entries, survivors,
expired entries, prior tombstones and index slots. Invalid samples and table
overflow remain explicit. Sums, maxima and disposition totals include valid
overflow observations; known rows continue accumulating after saturation.
Header/rows/footer are qualified by the PE Present ordinal and timed-child
event/sample denominators. The GT2 parser checks conservation, report cadence,
monotonic counters and maxima, complete framing and nonempty populations.
Thirteen synthetic parser cases cover valid reports and malformed/truncated
ones. Neither fixed decimation nor a complete bounded table proves random or
unbiased workload sampling.

## Actual PE32 GT2 distributions

Both catalogue runs use Sikarugir `sikarugir-cx-24.0.7`, the `perf` profile
with Info logging, phase diagnostics and decimation 127. Replay offload is ON;
inline constants, segmented transport, CPU-ready tape and index reorder are
OFF. No CPU sampler, concurrent build or second benchmark runs alongside.
These diagnostic FPS values are not used for performance conclusions.

Only differenced complete report groups inside the contiguous positive-wall,
`draw_calls > 100` scene are counted. The first group is a baseline snapshot,
not an additional population. `distribution-{05,06}-n127.json` retains every
selected count and shape:

| Interior population | 05 GT2 | 06 GT2 |
|---|---:|---:|
| PE Present endpoints | 60 → 1740 | 1020 → 2100 |
| Present intervals | 1680 | 1080 |
| Actual retainer epoch events | 26,516 | 20,331 |
| Events / Present | 15.783 | 18.825 |
| Sampled epochs | 209 | 160 |
| Clean / tombstone-only / expiry rebuild | 11 / 0 / 198 | 10 / 0 / 150 |
| Expiry share of samples | 94.74% | 93.75% |
| Mean input entries | 123.756 | 123.288 |
| Mean survivors | 100.359 | 101.725 |
| Mean expired entries | 23.397 | 21.562 |
| Index slots, every selected sample | 2048 | 2048 |
| Invalid / table-overflow observations | 0 / 0 | 0 / 0 |

Total selected entry/survivor/expiry counts are
25,865 = 20,975 + 4,890 and 19,726 = 16,276 + 3,450. The same table size can
serve small sets: the most frequent 05 shape is 24→23 entries (15 samples),
and the most frequent 06 shape is 6→5 (14 samples). Absence of sampled
tombstones is not proof that no rollback occurred anywhere in either run.

The existing child records 3.882 ms over 209 samples and 2.630 ms over 160:
18.574/16.438 microseconds per sampled epoch. Null-calibration means are
215/185 ns. These are elapsed QPC timings under diagnostics, including
expiry scanning, resource release, compaction and index rebuilding. They are
not exclusive PE CPU, isolated index cost, removable time or a per-shape
timing. Do not multiply them into a claimed FPS gain.

Both diagnostic harness verdicts pass with all eight reported chunk/GPU/
pipeline health counters zero. 05 has final health coverage through Present
1798; 06 through 2100, with 14 later frame rows outside that full snapshot.
The analyzed 06 distribution ends at 2100. A passing report does not prove
the unreported tail or every rendered pixel.

## Verification scope

The twelve public hot-entry metrics and qualified prepare/writer parent
metrics are unchanged on both PE widths. Eleven selected retainer/default-
settlement linked bodies per width have equal instruction tokens, call and
stack metrics after exact relocation qualification. PE32 inline switch-table
bytes and PE64 relative table entries are decoded separately; every case's
ordered target offset agrees. No arbitrary immediates or unmatched targets
are masked. Retainer fields and the ordinary settlement body are unchanged;
the optional diagnostics object grows, as do DLL files by 4608/3072 bytes.
This is not whole-image identity or a guarantee against layout/cache effects.

All four canonical full builds pass without compiler warnings; six focused
suites pass on each native arm64 and x86_64 build. The actual `endEpoch`
projection is checked during the existing 393,216 bounded serial-reference
traces, including rollback, expiry, clear and unsigned wrap. Native negatives
cover missing index entries, impossible counts and epoch/slot changes.
Observer tests pin N=0/1/2, empty settlement, malformed projection,
nonzero tombstone/expiry aggregates, 256-row saturation and exact sums across
overflow. This is bounded observation/refinement evidence, not a new TLA+
lifecycle or concurrent reentrancy proof. Product transitions are unchanged.

Six actual PE processes run the checked-in `d3d9_inline_constants` fixture:
baseline OFF, candidate OFF and candidate ON for each width. Each passes four
registered checks, 24 readback rows and two zero device-release receipts.
Candidate OFF's 192 pixel samples and candidate ON's 192 separately match
baseline exactly. The independent expected-color oracle retains tolerance 3
per channel. Each ON process also emits two complete, valid, nonempty retainer
distributions. These are bounded delayed-readback/teardown checks, not
all-family or whole-workload image equivalence.

## Separate diagnostic-OFF FPS spots

The final candidate is measured once per product with the canonical `perf`
Warn profile and frame sampling 1, but without PE phase diagnostics, PE
sampling or an external CPU observer. Policy flags and runtime match the
distribution runs. 06 uses focus-free named GT2 selection. These fresh runs
do not use the earlier user-reported desktop-interfered result as evidence.

| Product | Resolution | Frames | Positive wall interval | Scene FPS | Versus README |
|---|---|---:|---:|---:|---:|
| 05 GT2 | 1024×768 | 1939 (1–1939) | 64.555 s | 30.036 | −0.21% vs 30.1 |
| 06 GT2 | 1280×720 | 1168 (986–2153) | 64.076 s | 18.228 | +0.71% vs 18.1 |

05 uses all positive-wall frame rows; 06 selects contiguous rows with more
than 100 draws and retains the same loading-tail mixture as prior spots.
These are sampled scene averages, not `.3dr` benchmark-owned or process-
elapsed FPS. Both harness verdicts pass with eight reported health counters
zero. 05 exits normally with coverage through Present 1940. 06 reaches the
permitted post-scene GUI timeout, with counters through Present 2100 and
53 later rows outside that full health snapshot. The captured forest frames
are visible, not black; they do not prove whole-scene image equivalence.

No matched fresh baseline or exclusive CPU timing is available. This change
adds cold attribution, not a new default-path optimization. The differences
from H286's 29.607/18.176 FPS cannot be attributed to H287, nor do these
single spots establish durable recovery. README remains unchanged.

## Provenance

The frozen baseline is `a285c4ba`. Runtime source patch SHA-256 is
`d479078deefb10cf43031b9087ca718990403c5d1b6490bfae82eb63b6065744`.
Candidate PE32/PE64 prefixes are `77208323bbac5aad` and
`2a3aa4c27e4180bf9`; `candidate.json` retains full hashes. Both bridge DLLs
and the Unix provider remain byte-identical. Each run reconciles actual
Wine-staged images with frozen images before/after; the final Wine root
contains the candidate set.

`final-audit.json` binds builds, native suites, six pixel processes, both
diagnostic distributions, parser tests and both OFF spots. Analysis scripts,
raw logs and binary copies are ignored local artifacts, not new installed
runtime APIs. The initial codegen report is preserved: it decoded inline
table data as instructions. The corrected checker retains exception tails,
uses exact next-symbol bounds and compares table targets. The intermediate
`cold-codegen-qualified.json` was written before a metadata-iteration
exception; it remains retained, not a passing execution. The passing report is
`cold-codegen-qualified-r2.json`; no product change or rebuild was needed
to correct those analysis errors.

## Remaining economy gate

The clean shortcut already avoids scans when nothing expires. The next bounded
candidate is the expiry path: `RetentionIndex::rebuildFrom` clears all 2048
slots and reinserts survivors after the stable entry compaction. Updating
survivor index positions during that scan and erasing expired keys could avoid
the second pass, but adds lookups and tombstone/load-factor management. It
must be compared with the reference on actual PE32/PE64, including retained
collision chains, rollback, growth, release order and epoch wrap. Resource
release cost must remain distinct from index-maintenance savings.

Do not change warm lifetime, entry ordering, capacity policy or bridge
ownership based on these counts. A new index representation requires its own
bounded differential and linked/runtime economy evidence. Complete owner
admission and constant handling remain broader PE candidates; this small
diagnostic increment does not establish README recovery.
