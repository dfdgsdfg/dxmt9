---
domain: present-pacing
workload: STALKER H502/r5 capture, synchronous scratch preparation
title: "Synchronous preparation observer fails the original pixel oracle"
type: evidence
status: rejected-pixel-mismatch
updated: 2026-10-16
source: traces/stkcop-prepare-local-h604/{contract.md,build.json,runs.json,reject.py,rejection.json,O0.log,P0.log,P0.csv}
related: present-pacing-queue-partition.603.md; present-pacing-prepare-deferred.605.md
---

# Timing rejected after the original pixel oracle fails

H604 attempts actual-input preparation attribution inside the synchronous queue
call. Product output always follows the original code; scratch work compares
timed, untimed, payload-only and pair-only variants.

O0 passes. P0 exits 5 with an output mismatch: its source and output agree with
each other, but differ from the original by 25,100 of 3,686,400 bytes. Query
transcript, seven source boundaries, 670 wrapper lifetimes and normal-state
restoration pass. These do not override the pixel rejection.

Original pixel SHA-256 is
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`;
P0 produces `4e654c482870a66d419cd2263a04aabb60e949b39859f2b8c41dbdddfb54a569`.
The cause is not isolated; observer timing alone is not a proven explanation.

`rejection.json` preserves the evidence and excludes all CPU timing. P1 and O1
were never launched; there are no replacement runs. `runs.json` remains
incomplete intentionally. H605 is a new deferred experiment, not a repair or
reinterpretation of this population. No product change or promotion follows.
