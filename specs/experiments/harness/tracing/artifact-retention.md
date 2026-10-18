---
type: "Design"
title: "Trace and Analysis Artifact Retention"
description: "Bounded storage, recoverable compression and evidence-aware cleanup."
tags: [specs, experiments, tracing, storage, provenance]
---

# Trace and Analysis Artifact Retention

Defines R-HARN-TRACE-18.1–18.7 for trace/analysis artifacts under explicitly
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
or unreadable inputs block permanent deletion and derived pruning. Recoverable
archival uses the reader-compatibility admission below and preserves those
dependency edges; it does not infer that references are absent. Deduplication also requires exact bytes
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
Archive-aware readers may resolve a logical source to a verified temporary
extraction without changing source manifests or writing pointer stubs. Readers
without such an adapter must explicitly restore the original expected path
before use. Only qualified reader contracts admit referenced-file archival;
other cited paths remain materialized. Deleted evidence cannot support fresh
verification.

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

## Archive-aware resolution and admission

The availability record binds repository-relative logical path, original file
snapshot, archive digest/location, frozen plan digest, successful operation and
reader qualification. A resolver must not accept an archive-copy receipt as
permission to hide a missing source: source eviction must be explicitly recorded
as completed. Conflicting identities for a missing logical path are ambiguous;
expected digests may disambiguate, otherwise fail closed. Interrupted operations
require explicit recovery rather than guessing the last completed state.

Resolve under a shared retention lease; mutations hold the exclusive lease.
Prefer an existing regular source, retaining caller-supplied digest checks.
For an archived source, verify the operation binding and archive hash, fully
extract and validate original bytes in bounded private staging, and remove that
staging after the read. Do not silently leave restored bulk files at original
paths or cache unbounded copies. Capacity policy comes from the frozen operation;
a resolver may refuse a read if restore headroom is insufficient.

A source audit may accept archived evidence only after the same recovery
validation. A cited existing directory must also validate missing archived
members beneath it; directory existence alone does not prove that those bytes
remain available. Report the number of verified archived sources separately
from missing/deleted evidence. Do not rewrite historical source citations.

The initial qualified contracts are CPU supply v2 and wait trace v1 CSV inputs
through their shared readers. Qualification binds the source/archive digest,
reader/resolver/audit implementation identities, and parsed original/restored
result parity. A review names the contract and confirms that known readers use
that adapter or explicit original-path restore. Recheck qualification before
source removal; code drift, a mismatched source or an incompatible reader blocks
admission. Other formats retain the existing materialized-reference protection.

Referenced archival preserves the complete file, identity and logical reference;
it does not remove a dependency from the evidence graph. Global unrelated opaque
metadata is therefore not an absence-of-references test for this narrow lane.
Candidate-specific unresolved lifecycle/recovery, incompatible readers, pins and
age gates still block it. Permanent deletion and derived pruning continue to
require the conservative full dependency checks. Storage verification/parity
reads do not invent a historical analysis-use timestamp or reset scientific
analysis age; use the latest explicitly recorded analysis use, including prior
retention reviews. No age exception is introduced.
