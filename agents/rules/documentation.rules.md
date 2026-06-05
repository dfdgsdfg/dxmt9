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
- Use inline code for requirement IDs, commands, files, and env vars.
- Keep Mermaid diagrams for ordering, ownership, and state machines where prose
  would hide the important dependency.
- Avoid long code listings in rules. Link to source when exact implementation
  detail matters.

## AGENTS.md vs. Rules vs. Specs

| Doc | Question it answers | Lifetime | Scope |
|-----|---------------------|----------|-------|
| `AGENTS.md` | What local repository context should agents know before working? | Medium | Workspace or folder |
| `agents/rules/*.rules.md` | What recurring project rule prevents future mistakes? | Long | Project-wide |
| `specs/{area}/*.md` | What must dxmt9 do, and what design owns it? | Long | One subsystem or concern |
| `specs/gap.md` | What is not implemented, partial, or newly accepted? | Active | Whole project |
| `docs/perfomance/*.md` | What performance bottleneck, experiment, evidence, and next gate is known? | Active | Performance model and experiment graph |

Decision flow:

- Local implementation detail that may change with code: update `AGENTS.md`.
- Reusable rule, pitfall, or project convention: update `agents/rules/`.
- Requirements, architecture, verification, or compatibility contract: update
  `specs/`.
- Implementation status or missing evidence: update `specs/gap.md`.
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
