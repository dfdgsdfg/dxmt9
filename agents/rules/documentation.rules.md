---
description: Token-efficient documentation guidelines for agents/ directory files
paths:
  - "agents/**/*.md"
  - "CLAUDE.md"
  - "**/AGENTS.md"
globs: "agents/**/*.md"
alwaysApply: false
---

# Documentation Guidelines

All `@agents/` documentation should balance token efficiency with human readability.

## Language Policy

| Doc | Language |
|-----|----------|
| `agents/rules/*.rules.md` | **English only.** Loaded into every session — keep tokens predictable and grep-friendly. |
| `specs/{topic}/*.md` (`requirements.md`, `design.md`, `researches/*.researches.md`, `plan.md`) | **English by default.** Long-lived, cross-team artifact — stay in English even when the team works in Korean. |
| `**/AGENTS.md` (folder-level) | **English by default. Korean MAY be used as a supplementary aid** when a domain term, copy snippet, UI label, or product nuance is clearer in Korean. Keep the structural skeleton (headings, tables, code) in English so other agents and tools can parse it. |
| Commit messages, PR titles, code comments | Per [`git.rules.md`](git.rules.md) and [`code_style.rules.md`](code_style.rules.md). |

**Rules of thumb:**

- If the same idea reads cleanly in English, write English. Mixing per-sentence is noise.
- Reach for Korean only when the English would be lossy: brand names (`어스플러스`, `어스누리`), legal/policy language quoted verbatim, in-app copy under review, edge-case product nuance.
- When mixing, English first, Korean in parentheses or follow-up sentence: `Forbidden landing CTA copy ("멤버십 가입하기").`
- Headings, tables, code, and inline identifiers MUST stay English.

## Writing Principles

- **Concise Headers**: Short, descriptive titles without redundant words
- **Bullet Lists**: Prefer lists over paragraphs for structured information
- **Code Examples**: Include only essential parts, use comments for context
- **Abbreviations**: Use common abbreviations (impl, repo, DI, API) consistently
- **Tables**: Use for comparisons and mappings instead of verbose descriptions
- **Remove Filler**: Eliminate "it should be noted that", "basically", etc.

## Code Example Guidelines

- **Generic Names**: Use domain-agnostic names (`UserModel`, `ContentModel`, `ConfigModel`)
  ```dart
  // Good: Generic
  class GetDataRepository extends CachePolicyableGetableRepository<DataModel, GetDataParam>

  // Avoid: Domain-specific
  class GetHelpdeskTicketRepository extends CachePolicyableGetableRepository<HelpdeskTicketModel, GetHelpdeskTicketParam>
  ```
- **Concrete Code for Pitfalls**: Show full implementation for tricky cases, gotchas, workarounds
- **Pseudo-code for Common Patterns**: Use simplified syntax for standard patterns
- **Reference Real Examples**: Link to actual implementations when needed
  ```markdown
  See `features/misc_feature/lib/helpdesk/data/repositories/get_helpdesk_config_repository.dart`
  ```

## Structure Patterns

- **Problem/Solution**: State issue briefly, then solution directly
- **When/Then**: For conditional guidance without verbose explanations
- **Do/Don't**: Clear binary choices without justification paragraphs
- **Checklist Format**: For step-by-step processes

## Markdown Optimization

- Use `**Bold**` for key terms instead of explanatory sentences
- Link to other docs instead of repeating content: `See [DAO](dao.md)`
- Group related items with consistent formatting

## LLM Token Efficiency

- Write for LLM context window optimization
- Prefer pseudo-code over full implementation for examples
- Remove redundant explanations that LLM can infer
- Structure content for single-pass comprehension

## Document Cross-Reference

| Directory | Role | Required Meta |
|-----------|------|---------------|
| `agents/rules/` | Technical details, patterns, constraints (single source of truth) | `description`, `paths` (Claude Code) or `globs`/`applyTo` (Cursor/github) |
| `agents/prompts/` | Task-oriented workflows (reference rules/ for details) | `description`, `globs`/`applyTo`, `mode: "agent"` |
| `agents/skills/` | Executable workflows with shell scripts (`SKILL.md` + `scripts/`) | — |
| `agents/products/` | Non-technical product context | `description`, `globs`/`applyTo`, `alwaysApply` |

## AGENTS.md vs. rules vs. specs

Three different docs answer three different questions. Pick by lifetime and audience.

| Doc | Question it answers | Lifetime | Scope | Examples |
|-----|---------------------|----------|-------|----------|
| **`{folder}/AGENTS.md`** | "What's the *current* implementation detail of this folder, and what's about to change?" | Short — tracks the present and near future. Refresh as the code evolves. | The folder it lives in | `apps/insight_app/lib/views/original/AGENTS.md`, `app/overlay/AGENTS.md` |
| **`agents/rules/*.rules.md`** | "What patterns / pitfalls / constraints apply across the whole codebase, regardless of folder?" | Long — institutional memory. Should still be true a year from now. | Project-wide; loaded into every session | `cache.rules.md`, `view_model.rules.md`, `git.rules.md` |
| **`{anywhere}/specs/{topic}/*.md`** | "*Why* does this feature exist (`requirements.md`) and *how* is it built (`design.md`)?" | Long — survives until the feature itself is removed | One topic / feature | `specs/onboarding/requirements.md`, `features/club_feature/specs/cheering/design.md` |

**Decision flow:**
- Documenting code in front of you that may shift? → folder `AGENTS.md`.
- Encoding a rule that will outlive any one feature? → `agents/rules/`.
- Capturing the why/what/how of a feature? → `specs/{topic}/`.

**Cross-references:**
- `AGENTS.md` SHOULD link out to relevant `agents/rules/*.rules.md` instead of restating patterns.
- `specs/{topic}/design.md` SHOULD link out to `agents/rules/` for naming/architecture conventions, and to `AGENTS.md` for the folder it touches.
- `agents/rules/` MUST NOT duplicate `AGENTS.md` content — rules are general, `AGENTS.md` is local.

See [`documentation_spec.rules.md`](documentation_spec.rules.md) for the full spec authoring rules (requirements / design / research / plan).

## Related

- [documentation_memory.rules.md](documentation_memory.rules.md) — what to capture in rules: pitfalls, constraints, design decisions, edge cases
- [documentation_spec.rules.md](documentation_spec.rules.md) — requirements.md / design.md / researches/{target}.researches.md / plan.md authoring rules
