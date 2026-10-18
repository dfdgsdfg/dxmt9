---
domain: present-pacing
workload: STALKER CoP Day trace storage
title: "Referenced CPU traces archived with original-path reader parity"
type: evidence
status: storage-archival-verified
updated: 2026-10-18
source: experiments/output/_retention/reviews/20261018-archive-readers.json; experiments/output/_retention/reviews/20261018-archive-readers-{baseline,copy-plans,archive-plans,result}.json; experiments/output/_retention/operations/20261018T070207Z-{99fc30c93276,773ceee45662,299789cb36ee,a44acdd6669b}/{plan.json,contents.json,receipt.json}; experiments/output/_retention/operations/20261018T070129Z-{82eaf1962e82,6fec16871bec}/reader-qualification-cpu-supply-v2.json; experiments/output/_retention/operations/20261018T070130Z-{ccd9faa5549a,77d1cabafa36}/reader-qualification-cpu-supply-v2.json; traces/query-ranges-h417/{off-active1,on-early1,on-h424-active1,on-narrow1}/supply.csv
related: artifact-retention-recovery-20261018.md; present-pacing-session-release-cost.417.md; present-pacing-range-reuse-activation.424.md; ../../../experiments/analysis/retention.md
---

# Referenced trace archival

Four closed CPU supply CSVs were compressed, fully restored and checked before
their materialized originals were removed. The shared reader now resolves their
unchanged logical paths through verified temporary extraction. This verifies
storage and consumer compatibility; it makes no new engine timing claim.

All sources are under `traces/query-ranges-h417/`:

| Run | Original bytes | Archive bytes | Parsed rows |
|---|---:|---:|---:|
| `off-active1` | 3,155,917 | 373,183 | 40,968 |
| `on-early1` | 7,942,994 | 1,012,994 | 115,823 |
| `on-h424-active1` | 3,384,390 | 400,768 | 44,183 |
| `on-narrow1` | 7,200,219 | 903,882 | 101,216 |
| **Total** | **21,683,520** | **2,690,827** | **302,190** |

The retained archives are 12.41% of the original logical size, a reduction of
18,992,693 bytes (18.99 MB). File allocation accounting reports 21,692,416 bytes
for the removed originals and 2,699,264 bytes for the archives, a difference of
18,993,152 bytes before compact operation/evidence records. These file totals
do not prove the same amount of physical filesystem space was reclaimed.
Content-addressed archives created by `archive-copy` were reused by `archive`.

The four eviction receipts observed a combined free-space increase of
21,561,344 bytes after the archives already existed. Across the broader baseline
through final validation interval, filesystem free space decreased by
106,450,944 bytes, ending at 12,022,267,904 bytes. Concurrent filesystem activity
prevents attributing either observation entirely to this cleanup.

## Qualification and preserved provenance

- Each completion record has an explicit September 30 completion timestamp and
  `returncode=0`. Headers report `closed=1 overflow=0 incomplete=0 cpu_clock=1`.
- Digest-bound historical analysis documents were updated September 30. The
  lifecycle reviews conservatively use September 30 at 23:59:59 UTC as last use;
  current storage/parity checks do not claim a new scientific analysis. The
  14-day age gate passed, with no pins or open source handles.
- Each source first passed `archive-copy` extraction and `cpu-supply-v2` reader
  qualification. Proofs bind source/archive digests and reader, resolver and
  audit implementations. New `archive` plans rechecked every gate before removal.
- After removal, both `analyze_cpu_supply.load` and
  `analyze_session_release.analyze` returned exactly the same canonical result
  hashes through the original logical paths. All four directory audits passed.
- The source audit passed for the unchanged H417 and H424 historical documents,
  verifying four archived sources. Reader staging was empty afterward. Original
  manifests and cited logical paths were not rewritten.

Compression used zstd 1.5.7 with `-3 -T1`. Each plan explicitly froze a 4 GiB
free-space floor because available space was below the default 20 GiB. The
small selected workload reserved archive, extraction and control-record capacity
before removal. This override does not change the general policy.

## Scope and remaining protection

The previous `on-active1/supply.csv` archive-copy remains materialized: its
earlier review explicitly recorded October 18 use and was not backdated. Large
captures, opaque database dependencies, unqualified consumers and sources with
unknown completion/use histories remain protected. No permanent deletion of
the archived evidence or recursive run-folder removal occurred.

The implementation admits individual CPU supply v2 and wait trace v1 CSVs;
this real-data exercise covers CPU supply only. The seven focused Meson targets
for archive primitives, retention policy, resolver, qualification, CPU analysis,
wait traces and document sources passed. Native Perfetto/SQLite readers and
directory-bundle eviction remain outside this admission scope.

Follow the [retention workflow](../../../experiments/analysis/retention.md) for
qualification, original-path reads and explicit restoration. Operation records
and bulk archives remain local ignored artifacts under
`experiments/output/_retention/`; the committed report is not a backup of them.
