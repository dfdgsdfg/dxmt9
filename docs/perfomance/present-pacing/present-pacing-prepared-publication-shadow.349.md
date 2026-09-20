---
domain: present-pacing
workload: Native x86_64 publication fixture (no GPU)
title: "H349: Prepared plan publication shadow connection and fixed storage cost"
type: evidence
status: partial-attribution
updated: 2026-09-21
source: experiments/output/prepared-encode-shadow-20260921/x86-{on-a,on-b,off-a,off-b}.csv; experiments/output/prepared-encode-shadow-20260921/x86-{on-a,on-b}.json; experiments/output/prepared-encode-shadow-20260921/x86-sha256.txt
related: present-pacing-shared-pass-indices.348.md
---

# H349: Publication shadow, not prepared execution

Base `6d0d98ad` plus the shadow overlay. `prepareSlotForPublish` now has a
default-OFF observer that builds/discards the bounded plan at actual ordinary
publication. Final Slot command ordinals, source/storage generations and sequence
are used; no wire reparse or execution-policy change. Draw identity resolution
is synchronous; unresolved entries never authorize parallel execution.

## Connection and synthetic cost

The `--publication-shadow-probe` mode of `dxmt9-prepared-encode-plan-spec`
uses actual queue submission/publication with no workers/GPU. Each process
publishes 18 sources, each containing 64 resource-empty DrawRun commands.
Exclude the first two shadow sources as warm-up; measure 16. Run order is
OFF-A, ON-A, ON-B, OFF-B with one x86_64 binary (hash receipt above). No build
or TLA job was running during these four probes. Normal desktop services were
not isolated. Earlier `native-on` / `x86-on` pilot files are not this evidence.

Both ON processes report 18 successful plans, 64 command rows and one candidate
range per source; OFF processes report zero prepared events. The plan is freed
before publication continues. No retained Slot capacity or Encode saving is
measured.

| Native thread CPU per source | A median | B median |
|---|---:|---:|
| Fixed allocation/initialization | 7 us | 7 us |
| Final-command/access projection | 6 us | 6.5 us |
| Seal/structural validation | 0 us | 1 us |
| Total shadow, including observation/destruction | 19 us | 20 us |

Total maxima are 21/82 us. The zero seal median is below the clock's effective
granularity, not proof of free validation. Every endpoint carries observer
cost; stage medians are not additive. The allocation is 149,576 bytes per
attempt (about 146 KiB), even for this small source. Do not extrapolate these
empty-resource numbers to STALKER's hundreds of canonical identities.

## Interpretation and next gate

This proves that the new path actually runs before publication and that OFF
does not build a plan. It does not prove the final design is faster: no Encode
work is removed, and the diagnostic adds another walk under the queue mutex.
The fixed initialization is a material component even with no identities.
Fused construction/reused or exact-sized storage deserves comparison before
turning preparation into normal production work.

Remaining: real-workload success/rejection population, resource-cardinality
cost, independent original-Encode differential comparison, async alias/owner
binding, Arena publication, and supply-critical-path measurements. The 256
source-identity ceiling remains conservative; H347's 433–458 per-pass identities
already exceed it. No FPS improvement or wide-resource readiness is claimed.
