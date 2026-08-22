---
description: Token-efficient documentation rules for dxmt9 agents, specs, and folder guidance
paths:
  - "agents/**/*.md"
  - "AGENTS.md"
  - "**/AGENTS.md"
globs: "agents/**/*.md"
alwaysApply: false
---

# Documentation Guidelines

Documentation in this repository should be concise, durable, and useful for both
humans and agents. Prefer concrete dxmt9 terms over generic app examples.

## Language Policy

| Doc | Language |
|-----|----------|
| `agents/rules/*.rules.md` | English only. These are long-lived project rules. |
| `agents/skills/*/SKILL.md` | English only. Skills are executable workflows. |
| `specs/**/*.md` | English by default. Specs are long-lived cross-session artifacts. |
| `AGENTS.md` | English by default. Korean may be added only when it clarifies a local note. |
| Commit messages and code comments | Follow the style already used in the repository. |

Rules of thumb:

- Keep headings, tables, code, and inline identifiers in English.
- Use Korean only for user-facing discussion or when quoting Korean source text.
- Do not mix languages inside a sentence unless the exact term must be preserved.

## Writing Principles

- **Be specific:** name the subsystem, ABI, test target, or file path.
- **Prefer tables:** use them for mappings, status, artifact roles, and tradeoffs.
- **Keep examples local:** use dxmt9, Wine, Metal, Meson, TLA+, and shader-corpus
  examples rather than unrelated application examples.
- **Remove filler:** avoid restating obvious context that the file path already gives.
- **Separate facts from plans:** committed docs record decisions and constraints;
  temporary sequencing belongs in scratchpads or ignored `plan.md` files.

## Code Example Guidelines

Use examples that match this repository:

```cpp
// Good: names the invariant and the boundary being protected.
bool validateRecordHeader(const CommandRecordHeader& header, uint32_t payloadBytes);

// Avoid: too generic to guide future changes.
bool validateStuff(const Header& header);
```

For shell examples, prefer the actual tooling used here:

```sh
meson test -C build dxmt9-core-spec
bash scripts/check/verify_tla.sh
```

## Structure Patterns

- **Problem / rule / why:** best for pitfalls and gotchas.
- **Artifact table:** best for spec ownership or test coverage.
- **Do / do not:** best for hard boundaries such as PE/unix bridge rules.
- **Traceability list:** best for requirement IDs, test targets, and gap rows.

## Markdown Optimization

- Link to existing docs instead of repeating them.
- In `docs/perfomance/`, use standard Markdown links with explicit relative
  `.md` targets; do not add Obsidian/wiki-style `[[...]]` cross-references.
- In `docs/perfomance/`, use `index.md` as the root entry point, keep
  `overview.md` for the general dxmt9 performance model, keep
  `overview-3dmark05-gt1.md` for the 3DMark05 GT1 investigation map, and use
  the shared root `log.md` for root-document maintenance history.
- Keep `docs/perfomance/overview-3dmark05-gt1.md` as a cross-domain map only:
  whole-experiment axes, current gates, and domain pointers. Move detailed
  verdict tables, long synthesis, and experiment chronology into the owning
  domain `overview.md` or `log.md`.
- In `docs/perfomance/`, domain pages live under
  `docs/perfomance/<domain>/`: use `index.md` as the domain landing page,
  `overview.md` for the current compact conclusion, and `log.md` for older
  rolled-up detail. Do not recreate top-level `docs/perfomance/<domain>.md`
  files.
- Domain `index.md`, `overview.md`, and `log.md` files should carry YAML
  frontmatter with `domain`, `workload`, `title`, `type`, `status`, `updated`,
  `source`, and `related` keys.
- Leaf documents under `docs/perfomance/<domain>/` may carry an optional
  `outdated:` frontmatter key. See
  [The `outdated:` key](#the-outdated-key-for-docsperfomance-leaves) below.
- Use inline code for requirement IDs, commands, files, and env vars.
- Keep Mermaid diagrams for ordering, ownership, and state machines where prose
  would hide the important dependency.
- Avoid long code listings in rules. Link to source when exact implementation
  detail matters.

## The `outdated:` Key For `docs/perfomance/` Leaves

A leaf document records an experiment. Over time the thing it measured can stop
existing: the knob is deleted from `src/`, the run directory is cleaned up, the
journal it cited is removed. The experiment is still worth keeping — a rejected
hypothesis is the reason nobody should retry that lane — but its numbers are no
longer something a reader can go and check.

`outdated:` is an optional leaf frontmatter key that says exactly that. It sits
beside `status:` and takes one of three values:

| Value | What it asserts |
|---|---|
| `knob-removed` | The `DXMT9_*` knob or code path this leaf measured is confirmed absent from `src/`. The experiment cannot be re-run at all. |
| `evidence-missing` | Every artifact path the leaf cites in `source:` is gone from disk. The numbers cannot be re-derived or re-checked. |
| `retired-journal` | The leaf's only `source:` is the deleted `specs/perfomance.plan.md` journal. |

Precedence when more than one applies: `knob-removed` > `evidence-missing` >
`retired-journal`. A removed knob is the strongest form of "cannot be measured
again".

A marked leaf also opens with a one-line body banner naming the ground, so the
reader sees it before any number. Do not delete a marked leaf and do not strip
the key; the leaf is history, and history is what this tree is for.

**Rules:**

- An overview must not present an `outdated:` leaf as current evidence. If a
  conclusion row's evidence link points at one, say so in the row — a short
  inline marker naming the ground is enough, because the leaf's own banner
  carries the detail. If every row in a table is outdated, say it once above the
  table instead of marking each row.
- Do not delete the row. A rejected hypothesis whose evidence is gone still
  records which lane was already tried.
- A figure that stays in `overview.md`, `overview-3dmark05-*.md`, or a domain
  `overview.md` must either come from a leaf that still has its artifacts, or be
  labelled plainly as a last measurement that cannot be re-checked. Never carry
  an unverifiable number as though it were current.
- `outdated:` is per-leaf and mechanical. A leaf whose `source:` is only
  *partly* gone stays unmarked; if an overview quotes a figure from the missing
  part, label that figure where it is quoted.
- New leaves should not need the key. If a new leaf would already qualify, its
  `source:` is wrong — cite a concrete surviving artifact instead
  (`scripts/check/audit_perf_docs_sources.py` enforces this for new or changed
  current leaves).

`scripts/check/audit_perf_docs_sources.py` audits the changed performance leaves
and all current `accepted-verdict` leaves, so a clean tree still checks the
current verdict surface. Its supported `source:` syntax uses semicolon or
comma separators and simple brace expansion such as `{gt1,gt3}`; every
expanded path must exist. Historical leaves outside that scope remain legacy
debt and are not silently rewritten. If a historical leaf must be touched while
its evidence is gone, record the applicable `outdated:` value explicitly.

## Citing Code From Specs And Docs

**Cite `path` plus a symbol name. Do not cite `path:LINE`.** Line numbers in
prose rot silently and nothing gates them.

Measured 2026-08-22 while decomposing `d3d9_pe_device.cpp`: 193 citations of
the form `d3d9_pe_device.cpp:<line>` exist across 9 spec/doc files, 167 of them
in `specs/d3d9/gap_d3d9.md` alone — and they were **already wrong before the
refactor touched anything**. Checked against the pre-decomposition file,
`gap_d3d9.md` cited `:2203-2226` for `D3DRASTER_STATUS::ScanLine` when
`computeRasterStatusEstimate` sat at line 13243, and `:276-279` for the FVF
`FLOAT1..4` decode when it sat at 1273/1300/1329. Off by ~11,000 lines with no
code motion involved.

Do **not** answer this with a line-number audit. A gate that fails on every
source insertion becomes a docs edit tax, and those 193 stale citations are the
measured proof that the tax does not get paid — it converts a silent
inaccuracy into a loud one that gets routinely bypassed. The repo already has
the correct shape in two audits: `audit_thread_ownership_declarations.py`
asserts the cited file exists and still contains a named marker, explicitly not
a line, and `audit_perf_docs_sources.py` strips any `#L1-L4` fragment before
validating a `source:` path. Extend that shape — path exists ∧ symbol present
— if a gate is wanted.

Existing `:LINE` citations are known-stale; rewrite one to path + symbol when
you are editing that passage anyway, not as a sweep.

## AGENTS.md vs. Rules vs. Specs

| Doc | Question it answers | Lifetime | Scope |
|-----|---------------------|----------|-------|
| `AGENTS.md` | What local repository context should agents know before working? | Medium | Workspace or folder |
| `agents/rules/*.rules.md` | What recurring project rule prevents future mistakes? | Long | Project-wide |
| `specs/{area}/*.md` | What must dxmt9 do, and what design owns it? | Long | One subsystem or concern |
| `specs/gap.md` + `specs/<topic>/gap.md` | What is not implemented, partial, or newly accepted? | Active | Root rollup + topic owner |
| `docs/perfomance/` | What performance bottleneck, experiment, evidence, and next gate is known? | Active | Performance model and experiment graph |

Decision flow:

- Local implementation detail that may change with code: update `AGENTS.md`.
- Reusable rule, pitfall, or project convention: update `agents/rules/`.
- Requirements, architecture, verification, or compatibility contract: update
  `specs/`.
- Implementation status or missing evidence: update the owning
  `specs/<topic>/gap.md`; update `specs/gap.md` only for root rollup or
  navigation changes.
- Performance bottleneck model, 3DMark05 GT1 experiment result, Xcode/gputrace
  proof, no-gputrace smoke, cleanup provenance, or next performance gate:
  update `docs/perfomance/`.

Cross-reference rules:

- `agents/rules/` should not duplicate full spec content. Link to the spec.
- Specs may link to rules for authoring conventions, but the spec remains the
  source of truth for dxmt9 behavior.
- `docs/perfomance/` is the source of truth for performance investigation
  history. New leaves should cite concrete artifacts in `experiments/output/...`,
  `traces/.../analysis`, exported Xcode counter CSVs, or generated reports in
  `source:`; do not use the deleted/retired `specs/perfomance.plan.md` journal
  as a new provenance target or maintenance surface.
- If a rule mentions a helper file, skill, or script, that file must exist in this
  repository or the rule must say it is external.

## Related

- [documentation_memory.rules.md](documentation_memory.rules.md) - what to
  capture in rules.
- [documentation_spec.rules.md](documentation_spec.rules.md) - dxmt9 spec
  authoring and traceability rules.
