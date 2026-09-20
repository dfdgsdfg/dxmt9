---
domain: present-pacing
workload: STALKER CoP Day
title: "H347: Exact rejected-pass resource cardinality and reuse"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-access-cardinality-h347-r695-day/{result.json,actual.png}; experiments/output/stalker-access-cardinality-h347-r695/{supply.csv,waits.csv,progress.json,native-tests.log}
related: present-pacing-stalker-pass-access.346.md
---

# H347: Exact rejected-pass resource cardinality and reuse

Base `1ab3a1f4` plus the diagnostic overlay. The production 256-identity
pass bound and 12-identity draw summary are unchanged. A cold TLS observer
counts canonical reads at the producer's actual candidate/DrawRun/seal
boundaries, only in the bounded IdentitySerial observer lane. It never provides
execution authority or changes producer acceptance, snapshots or certificates.

The diagnostic dictionary holds 4096 full 64-bit identities in 8192 buckets;
hash collisions compare full identities. Exhaustion or incomplete canonical
reads makes the result explicitly inexact/lower-bound. Five source-qualified
events describe each pass. The analyzer rejects missing, inconsistent or
malformed transcripts; absent events in historical traces remain unknown.
The roughly 98KB diagnostic dictionary and per-pass reset are intrusive:
reported CPU scopes are not observer-OFF product cost.

## R695 serial observation

Day/high 1280x720, Sikarugir cx24.0.7, perf. Metal occlusion, async fences,
covered Pending and publication dedup ON; Tape, CB join, END coalescing and
command/draw split OFF. IdentitySerial plus parallel observer ON. Supply
window 2450–2470, wait window 2452–2468. Builds/tests finished before runtime.

Runner PASS, 2817 encoded Presents, no capture error, zero resolved parallel
work and zero reported GPU command-buffer errors. Screenshot shows the game,
not a desktop fallback; this is not pixel equality. All five staged artifact
hashes match the canonical builds; unix SHA-256 prefix `cf792613ba24fdb2`.
Both traces close without overflow/incomplete writers; wait unknown identities
zero, CV entries 17. Two partial edge sources are excluded; all 1202 complete
sources have qualified observations.

| Source group | Sources | Inclusive encode CPU ms | Observer CPU ms | Accepted passes | Draws |
|---|---:|---:|---:|---:|---:|
| Query-scoped | 1140 | 209.598 | 19.164 | 0 | 0 |
| Unscoped with candidates | 20 | 352.391 | 4.914 | 56 | 39422 |
| Unscoped without candidates | 42 | 345.443 | 5.640 | 0 | 0 |

All 1528 pass transcripts are exact, with no diagnostic exhaustion or
incomplete reads. The 21 production-overflow passes are all rejected and
boundary-sealed:

| Metric | Per overflowing pass | Sum across 21 passes |
|---|---:|---:|
| Distinct canonical read identities | 433–458 | 9487 |
| DrawRun commands | 293–324 | 6626 |
| Canonical read touches | 1405–1539 | 31572 |
| Adjacent-command repeated reads | 402–448 | 9080 |
| Maximum uses of one identity | 38–43 | Not additive |

The distinct sum is per-pass, not globally unique resources. Repeated touches
are 22085/31572 (69.95%); adjacent repeated touches are 28.76% of touches.
Accepted passes contain at most 25 distinct reads. Rejection counters report
21 resource-incomplete/pass-union-overflow, 1140 query-state and 311 small-work
cases. These are not disjoint CPU cost categories or recoverable-time estimates.

## Verdict and next gate

The remaining bound failure is real distinct cardinality, not duplicate
counting. A 512-entry set covers this sample but is neither a workload maximum
nor evidence of executable parallel work. Do not enlarge every child and
certificate array on this basis alone.

The next representation candidate is a source/pass-owned canonical identity
table with exact child index/bitset views. Preserve unchanged draw/state SoA,
source/storage generation and range qualification, independent child coverage,
write-alias rejection and fail-closed overflow. This is a design hypothesis,
not an implemented sharing optimization. Price table construction, repeated
resolution and retained memory before promotion.

Native/analyzer focused tests pass 4/4; Python tests pass 28/28. Fixtures cover
accepted and rejected observer-OFF output equality, exact 266-resource overflow,
full-generation identities, reuse, dictionary exhaustion and scope restoration.
Native/unix and both PE builds pass; install-name audit passes. No fresh TLA
run is claimed for this diagnostic-only change; H346 retains prior execution
model evidence. Wide-resource GPU exact, executor binding/economics and child
imbalance remain open. No parallel policy promotion or FPS improvement claim.
