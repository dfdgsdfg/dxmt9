# Offline artifact retention

Policy: [trace and analysis artifact retention](../../specs/experiments/harness/tracing/artifact-retention.md).
Implementation: `scripts/tools/retain_trace_artifacts.py` and
`scripts/tools/retention_archive.py`. These tools run outside measurement windows.
They do not enable automatic expiry or change engine capture behavior.

## Inventory and explicit selection

```sh
scripts/run_python.sh scripts/tools/retain_trace_artifacts.py inventory
scripts/run_python.sh scripts/tools/retain_trace_artifacts.py plan \
  --candidate traces/example/run/supply.csv --action archive
```

Inventory reports logical and allocated bytes separately. A plan without a
lifecycle review is blocked; filenames, mtime and absent PIDs cannot supply that
review. Plans are persisted under
`experiments/output/_retention/operations/<id>/plan.json`. Inspect `entries`,
`blockers`, `reference_scan`, `pins`, `policy`, and the frozen source snapshot
before applying. Inventory and planning never remove source data.

| Action | Behavior |
|---|---|
| `archive-copy` (default) | Create and fully extract/verify an archive while keeping the original. Suitable for validating recovery of cited evidence. No minimum age because it does not evict anything. |
| `archive` | Compress a closed, reviewed bulk file after 14 days; references additionally require qualified reader admission. Remove it only after full verification and a second safety check. Skip eviction if compression does not reduce logical size. |
| `delete` | Permanently remove an explicitly reviewed, unreferenced bulk file after 90 days. Preserve a digest tombstone and compact review evidence. |
| `prune` | Remove stopped scratch or reproducible derived bulk files after output/input verification. Derived data requires 7 days and a verified regeneration record. |

Eviction supports individual bulk files. It does not recursively delete run
folders. JSON manifests, requests, receipts, proofs, summaries and unknown file
classes remain materialized. Directory bundles support `archive-copy` and
verified restoration to a new directory. Failed/rejected source eviction also
requires issue resolution and at least 30 days. Pins override age. References require the qualified reader workflow below for
recoverable archival; they still block permanent deletion and derived pruning.

## Lifecycle review and pins

A review JSON maps exact repository-relative source paths to records:

```json
{
  "traces/example/run/supply.csv": {
    "reviewer": "reviewer identity",
    "reason": "closed capture and resolved recovery, supported by the attached evidence",
    "owner_closed": true,
    "recovery_resolved": true,
    "reference_scope_complete": true,
    "completed_at": "2026-07-01T12:00:00+00:00",
    "last_used_at": "2026-07-02T12:00:00+00:00",
    "outcome": "passed",
    "evidence": [
      {"path": "traces/example/run/completion.json", "sha256": "actual file SHA256"}
    ]
  }
}
```

This is an explicit human/agent review backed by unchanged producer records,
not an automatic seal inferred by the tool. Record actual times and ownership
findings. `reference_scope_complete` attests that all dependency locations are
covered by the scan scope; it does not override detected references or unknown
metadata. Do not assert it when dependencies outside this repository are unknown.
`outcome` is `passed`, `failed` or `rejected`; the latter two need
`issue_resolved: true` for eviction. Lifecycle evidence must be compact (at most
16 MiB per file); exact copies are retained outside sealed run folders.

For `prune`, also record `class: "scratch"` or `"derived"`, digest-bound
`verified_outputs` and `retained_inputs` arrays in the same path/SHA256 format.
Derived pruning needs `regeneration` describing the already verified recipe,
query/tool versions and result. The tool verifies the retained files' hashes;
it does not execute a command embedded in a review or independently certify
that recipe. Keep its verification evidence among the lifecycle evidence files.

Optional `experiments/output/_retention/pins.json`:

```json
{"pins": [{"path": "traces/example", "reason": "regression baseline"}]}
```

A pin may instead identify `sha256`. Path pins include ancestors/descendants;
digest pins include bundle members. Invalid pin records fail closed.

## Apply and recover

```sh
scripts/run_python.sh scripts/tools/retain_trace_artifacts.py plan \
  --candidate traces/example/run/supply.csv --action archive --review review.json
scripts/run_python.sh scripts/tools/retain_trace_artifacts.py apply \
  --plan experiments/output/_retention/operations/OPERATION/plan.json
scripts/run_python.sh scripts/tools/retain_trace_artifacts.py restore \
  --receipt experiments/output/_retention/operations/OPERATION/receipt.json \
  --destination traces/restored-example/supply.csv
```

The restore parent must already exist; the destination must not exist. CLI
restore currently selects a receipt with exactly one archived source. Use a
single source per operation when independent restore commands are needed.
Restore to the original expected path before using path-bound analysis readers.
A restore record is written beside the original immutable operation receipt.
A local archive is not an independent backup.

Compression uses `zstd -3 -T1`. To select the Homebrew executable, put
`--zstd /opt/homebrew/opt/zstd/bin/zstd` before the subcommand on Apple Silicon.
The executable version/options, archive digest, original member hashes,
verification outcome and actual filesystem free-space change are recorded.
The latter includes concurrent filesystem activity and is not attributed savings.

The default free-space floor is 20 GiB. Preflight reserves simultaneous archive
and extraction space, allocation overhead, and evidence/journal writes without
counting future source removal. A nondefault floor requires both
`--free-space-floor-gib` and a concrete `--budget-note` on `plan`; the frozen
policy also applies to restore. The 10 GiB derived-analysis budget remains a
policy default; this tool does not enforce it inside DuckDB or capture runners.

Apply holds an exclusive retention lease, rechecks lifecycle hashes, source
identity, pins, references and open handles, verifies a full isolated extraction,
and rechecks before eviction. It quarantines one file with an atomic
no-replace rename, journals intent, revalidates and unlinks that exact file.
A repeated apply is refused. After interruption, inspect `receipt.json`,
`contents.json` and any recorded quarantine path; preserve both paths if a
producer recreated the source. No automatic retry or quarantine expiry occurs.
The lease coordinates retention operations only. Producers must actually be
stopped before the reviewer asserts exclusive ownership.

## Reference scan limits

The conservative deletion/pruning scan includes current/untracked documents in `docs`, `specs`, `tests`,
`agents`, `scripts`, `experiments/analysis`, root-level text documents/configs,
and textual artifact metadata under `experiments/output` and `traces`.
Path, basename, ancestor run-name and member-digest matches protect the source.
Every external manifest reference protects its input even if the referring
manifest is not itself cited; this conservatively covers indirect dependencies.
Retained-input/output claims in previous retention contents records also protect
those files. The tool stops scanning when it finds a protecting reference or
unknown input: a blocked plan is not a complete reference inventory.

Unreadable, malformed, oversized or opaque metadata blocks eviction. SQLite,
DuckDB and Parquet dependency metadata do not yet have qualified graph adapters.
Symlinks, hardlinked eviction sources, open handles, unresolved WAL/SHM/journal,
missing lifecycle data and unknown ownership also block eviction. This is not a
generic binary dependency parser or live-writer lock.
Source audits now verify recoverable archives, including missing archived files
below a cited directory. CPU supply and wait CSV shared readers resolve original
logical paths automatically. Other analysis consumers require explicit restore;
an archive-copy alone never makes an unexpectedly missing source acceptable.

Real-data recovery and blocked cleanup evidence is recorded in the
[2026-10-18 qualification](../../docs/perfomance/present-pacing/artifact-retention-recovery-20261018.md).
Subsequent qualified source eviction and original-path consumer parity are
recorded in the [reader archival result](../../docs/perfomance/present-pacing/artifact-retention-readers-20261018.md).

## Archive referenced CPU/wait CSV evidence

The `cpu-supply-v2` and `wait-trace-v1` contracts admit recoverable archival after
original/restored parser parity. All ordinary ownership, recovery, pin and age
gates remain in force; this does not authorize deleting referenced evidence.

1. Create and apply a single-source `archive-copy` plan with a truthful lifecycle
   review and the required staging capacity. Keep the original source in place.
2. Qualify that copy with the matching reader:

   ```sh
   scripts/run_python.sh scripts/tools/retention_readers.py \
     --receipt experiments/output/_retention/operations/COPY_OPERATION/receipt.json \
     --contract cpu-supply-v2
   ```

3. Add `reader_contract`, `reader_scope_complete: true`, and
   `reader_qualification: {"path": "experiments/output/_retention/operations/COPY_OPERATION/reader-qualification-cpu-supply-v2.json", "sha256": "actual report SHA256"}`
   to the lifecycle review. Record known readers and their adapter or explicit
   restore workflow. Add `analysis_use_evidence` as path/SHA256 records naming
   performance documents with an `updated` date; the reviewed last-use time must
   be at least the end of that date in UTC. Include any later known use. A newer
   prior retention review for the same source or an ancestor cannot be backdated.
4. Generate a new `archive` plan and inspect it, then apply it. Qualification
   binds original bytes, archive, reader/resolver/audit code and parsed parity.
   Any code drift before removal requires fresh qualification. Unrelated opaque
   metadata does not prove missing references: this lane retains the complete
   byte content and logical dependency instead of pruning that dependency.
5. Re-run the existing reader against the original logical source path and run
   the source audit on its citing documents. The reader/audit verifies the ledger,
   archive and exact extraction under a shared lease, then removes its private
   staging copy. No bulk file is left behind at the original path.

Keep all operation plans, receipts, contents and qualification reports. Missing,
changed, ambiguous, interrupted or tombstoned records fail closed. Historical
qualification identity is checked on reads; later code changes do not by
themselves make a previously verified archive unreadable. Original source paths
and manifests remain the provenance identifiers; temporary paths never replace
them. A missing logical path with only an `archive-copy` receipt remains missing.

Materialized reader inputs keep their usual relative-to-current-directory
semantics. A missing input outside the selected repository cannot resolve to
that repository's archives. Shared readers exclude concurrent retention
mutations; they do not stop an independent live capture producer.
