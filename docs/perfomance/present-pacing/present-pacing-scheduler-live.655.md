---
domain: present-pacing
workload: STALKER Day high late camera corridor
title: "H655 — original-game CPU evidence survives; recovered scheduler coverage rejected"
type: evidence
status: scheduler-coverage-rejected-observed-owner-ranking-retained
updated: 2026-10-17
source: traces/stkcop-scheduler-live-h655/{contract.md,inputs.json,derivation.json,recover_trace.py,assess_recovery.py,recovery-assessment.json}; traces/stkcop-scheduler-live-h655/R0/{receipt.json,analysis.json,self.bin,workers.bin,retirement.bin,foreign.bin,scheduler-receipt.json,scheduler-record.log,trace-recovery.json,recovered.ktrace,recovered-toc.xml,recovered-os-signpost.xml,recovered-thread-info.xml,post-tail.png}
related: present-pacing-external-clock.654.md; present-pacing-retirement-live.650.md; present-pacing-snapshot-performance.641.md; cleanup-20261017.md
---

# H655 — file recovery is not full-window evidence

The fixed original-game R0 episode completed normally, reached all seven camera
knots and restored all14 shared files. The selected knots1→6 contain90 completed
frames over6.870042–6.874461s. The H650 analyzer revalidates the original CPU
endpoints, runtime identities, image/restoration hashes and composed owner bounds.

| Observed owner | Native TID | CPU ms/completed frame |
|---|---:|---:|
| Replay | 3818791 | 42.470–42.774 |
| Encode | 3818774 | 34.213–34.836 |
| PE/root envelope | 3818239 | upper24.968 |
| Finish | 3818775 | upper1.136 |
| Completion | 3818776 | upper0.353 |

Replay's lower exceeds every **observed composed owner** upper. These are
System-Trace-ON diagnostic measurements, not observer-OFF product costs or a
new removal experiment. Concurrent CPU times must not be summed as latency.
The post-tail screenshot shows the building/characters without an obvious
black/repeated-geometry defect; this does not establish pixel-exact or transient
rendering correctness.

## Recovery and rejection

The original xctrace record completed collection but returned17 while saving.
The saved `.trace` was absent. After cleanup restored disk headroom, the closed
2193643952-byte raw file was hard-linked to `R0/recovered.ktrace` without removing
its original temporary pathname. This preserves a nonvolatile name without
another full copy. Its SHA-256 is
`d28ecd3a4ea7ccec4b813baf5520911c4ca83f8e930d37d903a745422eaf1d72`.

`xctrace import --template 'System Trace'` completed with exit0 and saved a
554MiB bundle. The raw hash was unchanged. However:

- The imported TOC covers only2.788579s, less than the selected6.87s corridor.
- Exported `os-signpost` tables contain zero rows. All three external helper
  files retain three enabled bracketed anchors, but neither required marker
  triplet can be joined to the recovered trace.
- The imported issue table is empty. This is not proof that the original
  collection was lossless or that missing logging streams can be reconstructed.

Both full-window and clock-mapping gates therefore reject this recovery for
global scheduler attribution. No scheduler upper is fabricated for the missing
interval, no wall-clock guess replaces the calibrated offset, and no shortened
subwindow is substituted for the selected corridor. The exact cause of the
shortened import is not independently isolated; disk exhaustion and original
rolling-window collection are context, not a proven causal decomposition.

## Decision and next gate

Do not rerun H655 or extend H641/H650's frozen populations. A new explicitly
separate scheduler population should omit rolling-window trimming, require at
least16GiB free before launch, retain the full saved trace and both external
marker triplets, and reject a save failure before attempting owner composition.
Use the same original game/camera contract and coherent private binaries; no
game-DLL instrumentation is required. Trace-OFF controls remain necessary for
default-path transfer. A raw-only import is not an acceptable fallback for
missing clock/whole-window evidence.

The strongest removable-cost evidence remains H641's separate snapshot
ownership/materialization treatment: block reductions5.645–6.029 and
6.295–6.670 Replay CPU ms/frame. H655 neither strengthens that removal amount
nor proves an FPS gain. Exhaustive default-path owner coverage and observer-OFF
transfer remain open; the full goal is not complete.
