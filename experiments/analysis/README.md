# Offline experiment analysis

This directory owns reusable analysis definitions and SQL for comparisons
across experiment or conformance attempts. Keep study-specific queries under
`experiments/analysis/<study>/`. General-purpose extraction and validation tools
belong in `scripts/tools/`; launching workloads remains with the existing runner.

DuckDB is provided by the root [Brewfile](../../Brewfile) for developer analysis.
`brew bundle` installs the CLI along with other development tools. Engine builds,
capture, Trace Processor qualification and per-run SQLite publication do not
require it. SQLite extensions are separate from the CLI; select them explicitly
and record their versions when used. No integrated DuckDB/Parquet exporter is
provided yet.

## Storage layout

Run from the repository root. Each analysis gets a new unique directory under
`experiments/output/analysis/<analysis-id>/`, which is already gitignored:

| Path relative to the analysis directory | Contents |
|---|---|
| `manifest.json` | Selected input attempts and artifact digests, qualification/exclusion reasons, query/extraction revisions, tool/extension versions, parameters, output schemas and digests. |
| `analysis.duckdb` | Optional derived analytical database, closed before its final digest is recorded. |
| `tables/*.parquet` | Optional exact-value table projections with run/attempt and source-record identity. |
| `results/` | Query results, charts, comparison reports and measured query time/memory. |
| `scratch/` | Temporary spill/intermediate files; exclude from the sealed output inventory. |

These are directory conventions, not files that the harness creates today.
Only create the outputs needed by the analysis. Set DuckDB's temporary directory
to that analysis's `scratch/` when spilling is needed. Do not put generated
databases, WAL files, Parquet or bulk results beside committed SQL queries.

Source `.pftrace`, `observations.sqlite`, receipts and attempt manifests stay in
their original run directories. Read them without mutation; bind inputs by
path and digest rather than moving them into the analysis directory. Per-run
SQLite row IDs need run/attempt and source artifact scope before cross-run joins.
An analysis has its own manifest and must not amend a sealed source manifest.
Keep analyses distinct from workload entries in `experiments/CATALOGUE.toml`.

For a shared result, commit the reusable query/definition here and record the
durable performance finding in the owning `docs/perfomance/` domain with links
to retained analysis artifacts. Do not silently overwrite an earlier analysis.

The [analytics contract](../../specs/experiments/harness/tracing/spec.md#optional-cross-run-analytics)
owns qualification, exact values and adoption gates. Use the shared
[cputrace/gputrace analysis workflow](../../agents/skills/cputrace/references/perfetto.md#optional-cross-run-analysis)
to decide whether DuckDB improves a measured query bottleneck.
