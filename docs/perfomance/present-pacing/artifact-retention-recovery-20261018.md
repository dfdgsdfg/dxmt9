---
domain: present-pacing
workload: STALKER CoP Day trace storage
title: "Verified archive recovery with cited evidence kept materialized"
type: evidence
status: storage-recovery-verified-no-source-eviction
updated: 2026-10-18
source: experiments/output/_retention/inventory-20261018.json; experiments/output/_retention/operations/20261018T063357Z-2effb8c7e52e/{plan.json,contents.json,receipt.json,reader-validation.json}; experiments/output/_retention/operations/20261018T063515Z-50884ac7c3c1/{plan.json,receipt.json}; traces/query-ranges-h417/on-active1/{supply.csv,completion.json}
related: present-pacing-session-release-cost.417.md; ../../../experiments/analysis/retention.md
---

# Archive recovery qualification

The retention implementation compressed a closed H417 CPU supply trace and
verified exact recovery. Existing evidence was not evicted. This validates the
storage workflow, not engine performance or new timing conclusions.

| Measurement | Result |
|---|---|
| Original `supply.csv` | 2,617,437 bytes |
| zstd archive | 304,814 bytes; 11.65% of original logical size |
| Codec | zstd 1.5.7, `-3 -T1` |
| Source/restored SHA256 | `7f3cf6fb5fb1458238bb838835c2768083f6575698f2faf50cb9aa562fe73b3f` |
| Archive SHA256 | `68b5e55ffd487ea2aaaf2671c2d4f801a2a7f1683142e5f7b47a3ae9ace65267` |
| Consumer parity | `analyze_session_release.py` produced identical JSON for original and restored bytes |
| Existing evidence removed | 0 files / 0 bytes |
| Temporary validation extraction | Removed after successful parity verification |

The content-addressed archive remains under
`experiments/output/_retention/archives/`. Full member verification occurs before
publication. A second restore fed the existing reader, which reproduced all
results, including 545 complete release groups and zero clipped, terminal,
stopped or consumer-only groups. The original CSV remains at its cited path.
The archived copy consumed additional space; the compression ratio is potential
savings, not reclaimed disk space. The apply receipt observed a 331,776-byte
free-space decrease, which also includes operation records and concurrent
filesystem effects.

The source header reported `closed=1 overflow=0 incomplete=0`; the unchanged
completion record reported `returncode=0` and completion at
`2026-09-30T01:20:15.266134+00:00`. The review recorded current analysis use and
therefore selected `archive-copy`, without asserting age-based eviction safety.
Available disk space was below the 20 GiB policy floor. This bounded validation
explicitly recorded a 4 GiB floor override and reserved simultaneous compression,
extraction and control-record storage. No general capture/analysis budget changed.

## Cleanup assessment

Inventory covered 2,289 direct roots and counted 263,275,417,328 logical bytes
and 251,860,553,728 allocated file bytes (about 234.6 GiB). It recorded 304
symlink/access/identity scan errors rather than assuming those paths were safe.
These are observed file totals, not a complete deduplicated filesystem allocation
or a reclaimable-space estimate.

The explicit deletion plan selected four large roots:

- `experiments/output/gpu-tape-qualification-20260910`
- `experiments/output/fixed-input-harness-20260909`
- `experiments/output/gt2-policy-matrix-20260915-r617`
- `traces/app-d3d9-stkcop-tail-h367`

All four remained materialized. Current evidence references and missing explicit
lifecycle/use reviews block eviction; recursive directory deletion is also
outside the initial tool's supported actions. A bounded follow-up inspection
found no proven removable scratch/spill candidate. This is not a claim that all
2,289 roots are permanently necessary or that every dependency was resolved.

Large cited artifacts require qualified archive-aware consumers/audits before
materialized eviction can proceed. Opaque database dependency adapters,
automated lifecycle qualification and analysis-runner quota integration remain
tracked in the [retention gap](../../../specs/experiments/harness/tracing/gap.md#artifact-retention).
