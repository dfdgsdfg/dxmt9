---
type: "Design"
title: "Trace and Analysis Artifact Retention"
description: "Bounded storage, recoverable compression and evidence-aware cleanup."
tags: [specs, experiments, tracing, storage, provenance]
---

# Trace and Analysis Artifact Retention

Defines R-HARN-TRACE-18.1–18.5 for trace/analysis artifacts under explicitly
selected `experiments/output/` and `traces/` roots. It does not authorize
blanket cleanup of all experiment outputs, worktrees, Wine prefixes or builds.
Current implementation limits are recorded in [gap](gap.md#artifact-retention).

## Retention classes and default review ages

Age makes an artifact a candidate, never permission to delete it. Measure age
from the later of sealed completion or explicitly recorded analysis use; do not
trust filesystem access time. Missing lifecycle/use data requires review.

| Class | Default action and review age |
|---|---|
| Active capture, open database/WAL, unjoined drain or unknown owner | Retain unchanged until exclusive ownership and quiescence are proven. PID absence or old mtime alone is insufficient. |
| Analysis `scratch/` and spill files | Eligible for removal after all owners stop and successful outputs are verified. Interrupted-run scratch remains until recovery needs are resolved. |
| Unreferenced, reproducible DuckDB/Parquet/query intermediates | Review after 7 days; evict only with retained inputs, query/tool versions and a verified regeneration recipe. |
| Sealed raw JSONL, CSV, logs, spool and databases | Review for lossless compression after 14 days. A spool listed by a receipt is evidence, not disposable scratch. |
| Failed or rejected attempts | Retain at least 30 days and through issue resolution; retain representative failure evidence and every input needed to explain rejection. |
| Unreferenced, unpinned evidence bundles | Review for permanent deletion after 90 days, including reverse dependencies and current documents/specs. |
| Referenced verdicts, baselines, regression reproducers and explicit pins | No automatic expiry. Retain the transitive evidence set; archive only with verified recovery and reader compatibility. |
| Requests, manifests, receipts/proofs, compact summaries and retention records | Retain after bulk-data eviction/deletion; record original identities/digests, action and availability without implying that deleted data remains revalidatable. |

Pins override every age threshold. References include uncommitted current
documents, `docs/perfomance/`, specs, test evidence and other analysis manifests.
An empty filename search is not proof of no references: resolve the artifact
graph, including digest references and indirect dependencies. Unknown references
or unreadable inputs block eviction. Deduplication also requires exact bytes
and preserved logical references, not matching filenames or similar traces.

## Storage and compression

Use `experiments/output/_retention/` outside sealed attempt/analysis roots:

| Path | Purpose |
|---|---|
| `archives/<archive-sha256>.tar.zst` | Immutable archive, or record a verified external archive location. |
| `operations/<operation-id>/plan.json` | Frozen candidate paths, sizes, source digests, retention class, references/pins, action, estimated savings and policy thresholds. |
| `operations/<operation-id>/contents.json` | Original paths, member sizes/digests, directory digest kinds and archive-to-source mapping. |
| `operations/<operation-id>/receipt.json` | Actual actions/bytes, archive digest/location, restore verification, errors and availability changes. |

Use zstd level 3 with one worker as the initial offline compression policy;
record actual codec/version/options and measured savings. JSONL/text/spool/SQLite
are candidates. Parquet should use column compression when originally produced;
DuckDB, images and existing archives may yield little additional saving. Skip
unhelpful recompression. Compression runs outside measurement windows and does
not add an engine dependency; the developer Brewfile supplies `zstd`.

Compress closed immutable bytes. Never VACUUM, checkpoint, rewrite or recompress
a sealed database in place: changing bytes invalidates its recorded digest.
An unresolved WAL is active/recovery state, not a standalone closed database.
Directory bundles retain member structure and their declared tree digest.

Archive creation uses a unique temporary destination followed by atomic
publication. Verify archive readability, extract into isolated staging, and
compare every original member digest/size and tree identity before considering
removal of materialized inputs. Bound expanded bytes and reject escaping paths,
duplicate members and links outside the bundle. Archive existence or compression
exit status alone is not recovery proof. A local archive is not an independent
backup; deleting the last materialized copy leaves only that archive unless a
second verified copy is explicitly recorded.

Sealed source manifests and proofs remain byte-identical. Retention records form
an external availability index: `materialized`, `archived`, `pruned-derived` or
`deleted`. Restore original bytes to their original expected paths before using
path-bound consumers; never replace a digest-bound file with a pointer stub.
Current readers and source audits require materialized paths and do not resolve
this index. Keep actively cited paths materialized until archive-aware resolution
and audits are implemented and qualified. Archived evidence is recoverable,
not immediately queryable; deleted evidence cannot support fresh verification.

## Capacity and cleanup ordering

Freeze analysis disk/spill and free-space budgets before a run. Starting review
defaults are 10 GiB per derived analysis and a 20 GiB free-space floor, with
explicit recorded overrides for larger workloads. These are proposed analysis
budgets, not existing runtime knobs or total repository limits. Archive/restore
preflight additionally reserves the new archive, extraction verification and
temporary spill footprint without counting projected deletion savings first.

Prefer stopped scratch cleanup, reproducible unreferenced intermediates, verified
cold archives, then reviewed obsolete evidence. Disk pressure must not remove
active or pinned data; report the shortfall or reject optional analysis instead.
Raw bytes and allocated disk usage are different measures, especially with
clones/hardlinks; a plan must distinguish measured sizes from estimated savings.

Cleanup defaults to inventory/dry-run. Applying a plan acts only on explicitly
selected candidates after rechecking identity, digest, references and ownership.
Use an exclusive mutation lease; reject symlinks/root escapes and changed
candidates. Persist the plan before mutation and per-action results as work
proceeds; interrupted cleanup must be diagnosable. Never remove an entire run
merely because its files look older than a threshold.

Verification must cover pinned/referenced inputs, live owners/WAL, stale plans,
incomplete manifests, corrupt/truncated archives, restore digest mismatch,
expansion limits, interrupted publication/deletion and exact reader revalidation.
Permanent deletion leaves a tombstone and updates dependent evidence claims;
follow [documentation rules](../../../../agents/rules/documentation.rules.md#the-outdated-key-for-docsperfomance-leaves)
when cited evidence is lost. Preserve unknown or failed outcomes explicitly.
