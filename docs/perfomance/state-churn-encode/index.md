---
domain: state-churn-encode
workload: 3DMark05 GT1
title: "State-Churn Encode — the CPU encode path and draw-run batching"
type: domain-index
status: current
updated: 2026-07-20
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.203.md
related: docs/perfomance/state-churn-encode/overview.md; docs/perfomance/state-churn-encode/log.md
---

# State-Churn Encode — the CPU encode path and draw-run batching

Latest tracked row: [phase 203](state-churn-encode-encode-phase.203.md) - the
direct-cbuf dirty-rebind correctness gate is deterministic and the
constants-only Stage 2 path is now default-on. Explicit value `0` retains the
slot-30 rollback lane; repeated GPU-phase sampling remains a watchpoint.

Current status: the commit-replay offload is engine-default ON (`d45af067`, H216 in [present-pacing](../present-pacing/index.md)), and the rejected replay-carrier lanes documented in this domain's history (chunk-end carry + `AndRun`/`WithResourceMarking` family, draw-run preflush merge/mixed-carrier, compact uniform submission carrier, canonical draw-run fast path, publish-time PSO prefetch) were removed from the tree in the H217-H220 cleanup waves — see the [overview](overview.md) current-status section.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [state-churn-encode-append-decomposition.16 - The Setup Prologue Is Half A Per-Draw Metal Debug Group](state-churn-encode-append-decomposition.16.md)
- [state-churn-encode-append-decomposition.17 - Current-HEAD Producer-Wall Resize: PE Layer 10.3 ms, Drain Fence Harvested, Chunk-Seal Cadence Is The Lead](state-churn-encode-append-decomposition.17.md)
- [state-churn-encode-append-decomposition.18 - Chunk-Seal Cadence A/B: 2 ms Of Producer CPU Removed, Zero FPS, And No Stage Owns The 37 ms Frame](state-churn-encode-append-decomposition.18.md)
- [state-churn-encode-append-decomposition.19 - Thread Attribution: The Game Thread Is Genuinely Saturated, And The Measurement Host Is Not Clean](state-churn-encode-append-decomposition.19.md)
- [state-churn-encode-append-decomposition.20 - Cadence Re-Verification: Aliasing Refuted, The Saving Is Real, FPS Conversion Is Environment-Split](state-churn-encode-append-decomposition.20.md)
- [state-churn-encode-append-decomposition.21 - Deciding Clean-Host ABBA: The Cadence Win Is Real, Median +2.0%](state-churn-encode-append-decomposition.21.md)
- [state-churn-encode-append-decomposition.22 - Cadence Promotion: All Gates Green, Default Flipped To 256 Records / 1.25 MiB](state-churn-encode-append-decomposition.22.md)
- [state-churn-encode-append-decomposition.23 - In-Process PE Sampler First Light: Game 60%, Crossings 17%, Recorder 10%](state-churn-encode-append-decomposition.23.md)
- [state-churn-encode-append-decomposition.24 - Crossing Decomposition: Lock Round-Trips, A 78 µs Getter, And Refcount Churn Over The Bridge](state-churn-encode-append-decomposition.24.md)
- [state-churn-encode-append-decomposition.25 - Getter-Cache + Warm-Epoch Harvest: Bridge −2.21 ms/present Converts To +7.2% GT2](state-churn-encode-append-decomposition.25.md)
- [state-churn-encode-append-decomposition.26 - Post-Harvest Triage: Drain Is Solved, Mark Owns Commit, Locks Are Unix-Side Work](state-churn-encode-append-decomposition.26.md)
- [state-churn-encode-append-decomposition.27 - DOD Batch + Recorder-Mutex Gating: Correctness Kept, FPS Claim Retracted Into Layout Noise](state-churn-encode-append-decomposition.27.md)
- [state-churn-encode-append-decomposition.28 - Mark Decomposition: 61% Is Queue-Mutex Wait, And It Is Not Frequency Contention](state-churn-encode-append-decomposition.28.md)
- [state-churn-encode-append-decomposition.29 - Segment Holds Unmask The Owner: The Worker's Slot Append Copy Holds 3.4 ms/present](state-churn-encode-append-decomposition.29.md)
- [state-churn-encode-append-decomposition.30 - T2a' Lands: Both Mark Paths Leave The Queue Mutex, Model-First](state-churn-encode-append-decomposition.30.md)
- [state-churn-encode-append-decomposition.31 - Surface-Lock Shadow: A 1.5 ms Address Scan, A 93% Counter Win, And An FPS Null](state-churn-encode-append-decomposition.31.md)
- [state-churn-encode-append-decomposition.32 - T2b/T2c + Scan Batch Land: Counters All Win, FPS Null, And The Lock-Path Slack Rule](state-churn-encode-append-decomposition.32.md)
- [state-churn-encode-append-decomposition.34 - buildSparseState Phase Split + touchConstShadow Bulk Path: Flat Attribution, Call-Volume Verdict](state-churn-encode-append-decomposition.34.md)
- [state-churn-encode-append-decomposition.33 - Series Closure: Evidence Availability Correction, Multi-Workload Gate Deferred](state-churn-encode-append-decomposition.33.md)
- [state-churn-encode-append-decomposition.15 - Per-Call-Site Counters Make The Split Computable](state-churn-encode-append-decomposition.15.md)
- [state-churn-encode-append-decomposition.14 - Partitioning encodeDraw Works, But The Residual Was Mostly Mine](state-churn-encode-append-decomposition.14.md)
- [state-churn-encode-append-decomposition.13 - A Third Of encode_draw Is Unattributed, And I Priced The Instrument Wrong](state-churn-encode-append-decomposition.13.md)
- [state-churn-encode-append-decomposition.12 - After The Hoist The Bottleneck Moved To The Encode Thread (~1.7x)](state-churn-encode-append-decomposition.12.md)
- [state-churn-encode-append-decomposition.11 - The SWVP Hoist Is +29% GT2 Scene FPS, And CPU Converts 1:1 To Wall Clock](state-churn-encode-append-decomposition.11.md)
- [state-churn-encode-append-decomposition.10 - De-phasing Confirms The Nested-Instrument Echo Directly](state-churn-encode-append-decomposition.10.md)
- [state-churn-encode-append-decomposition.09 - 62% Of The Draw Entry Is An SWVP Probe That Reads Indices First](state-churn-encode-append-decomposition.09.md)
- [state-churn-encode-append-decomposition.08 - dxmt9's D3D9 Entry Points Are ~40% Of The Frame](state-churn-encode-append-decomposition.08.md)
- [state-churn-encode-append-decomposition.07 - Gating Two Wasted Clock Reads Removes 12% Of Append](state-churn-encode-append-decomposition.07.md)
- [state-churn-encode-append-decomposition.06 - The Fixed Per-Commit Cost Is Queue-Mutex Contention](state-churn-encode-append-decomposition.06.md)
- [state-churn-encode-append-decomposition.05 - The Flush Is Half Bridge-Crossing And Half Commit, And Its Fixed Cost Is The Pipeline's Clock](state-churn-encode-append-decomposition.05.md)
- [state-churn-encode-append-decomposition.04 - Cutting Chunk Flushes 2.9x Costs 4% FPS](state-churn-encode-append-decomposition.04.md)
- [state-churn-encode-append-decomposition.03 - Append Splits Three Ways And The Chunk Flush Leads](state-churn-encode-append-decomposition.03.md)
- [state-churn-encode-append-decomposition.02 - Retiring The Legacy Record Format Buys +2.1% GT2 Scene FPS](state-churn-encode-append-decomposition.02.md)
- [state-churn-encode-append-decomposition.01 - appendRecordDirect Is A Serialize-Parse-Reserialize Round Trip](state-churn-encode-append-decomposition.01.md)
- [state-churn-encode-encode-phase.203 - Direct-Cbuf Payload-Source Dirty-Rebind Regression](state-churn-encode-encode-phase.203.md)
- [state-churn-encode-encode-phase.201 - Uniform Append Residual After Fixed Handle Carry](state-churn-encode-encode-phase.201.md)
- [state-churn-encode-encode-phase.200 - Uniform Fixed Payload Handle Carry](state-churn-encode-encode-phase.200.md)
- [state-churn-encode-encode-phase.199 - Stage-Level Uniform Append Split Counters](state-churn-encode-encode-phase.199.md)
- [state-churn-encode-encode-phase.198 - Append Uniform CPU Residual Reanalysis](state-churn-encode-encode-phase.198.md)
- [state-churn-encode-encode-phase.197 - Draw Batch Submit Residual Reanalysis](state-churn-encode-encode-phase.197.md)
- [state-churn-encode-encode-phase.196 - Queue Lock Attribution Runtime](state-churn-encode-encode-phase.196.md)
- [state-churn-encode-encode-phase.194 - Forced Resource-Marking Flush Attribution](state-churn-encode-encode-phase.194.md)
- [state-churn-encode-encode-phase.191 - Forced Resource-Marking Submit Prerequisite](state-churn-encode-encode-phase.191.md)
- [state-churn-encode-encode-phase.190 - Chunk-End Carry Feasibility Audit](state-churn-encode-encode-phase.190.md)
