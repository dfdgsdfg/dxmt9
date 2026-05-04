---
description: How to update agents/rules/ files as project memory — what to record, what to skip, and how to write it
paths:
  - "agents/rules/*.rules.md"
globs: "agents/rules/*.rules.md"
alwaysApply: false
---

# Rules as Memory

`agents/rules/` is the project's institutional memory. Every session loads these files into context. A well-placed rule prevents the same mistake from recurring across developers and AI agents.

## What to Record

Rules capture the **non-obvious** — things code alone doesn't convey. Focus on:

| Category | Example |
|----------|---------|
| **Pitfall / Gotcha** | "Supervisor `onFailure` already reports failure — delegate `initialize()` must not throw, or it double-reports + crashes" |
| **Constraint** | "Native player FIFO limit: max ~4 concurrent players before eviction" |
| **Design Decision + Reason** | "Primary ID stays NumberId for SQLite backward compatibility; MongoDB hash is secondary `hashId` field" |
| **Edge / Corner Case** | "`getIsInitialized` completes `false` (not exception) when native player is evicted mid-init" |
| **Trade-off** | "Pausing old players instead of disposing: avoids stutter on quick switch, risks pool exhaustion on rapid switch" |
| **Assumption** | "`VideoPlayerSupervisor` assumes one active supervisor per route — multiple concurrent supervisors share the same native player pool" |
| **Limitation** | "`fix_generated_enums.sh` must run after every `swagger_parser` generation — the tool doesn't fix the `$unknown` escape bug itself" |
| **Risk / Hazard** | "Rapid episode switching (< 200ms) can exhaust native player FIFO → `PLAYER_EVICTED` → cascade crash" |
| **Exception to a Rule** | "`HashNumberIdable` uses HashId as primary, but `ClubContentNumberIdModel` keeps NumberId as primary for cache compat" |

## What NOT to Record

| Skip | Why |
|------|-----|
| General patterns derivable from code | Read the code |
| What was done | `git log` is authoritative |
| Standard library/framework usage | Docs exist |
| One-off fix with obvious cause | No recurrence risk |
| Implementation detail of one feature | Too narrow to be useful |

**Litmus test:** "If I lost all memory and saw similar code next month, would I make the same mistake without this rule?" — if no, skip it.

## How to Write

### Find the Right File

```bash
ls agents/rules/*.rules.md
```

Match the learning to an existing file. Prefer extending over creating new files. Create new only when no existing file covers the domain.

### Structure: Problem → Code → Rules

```markdown
### Section Title — Brief Description

Problem in 1-2 sentences. Version where discovered

\`\`\`dart
// Before — the broken pattern:
Future<void> initialize() async {
  ...
  throw StateError('failed');  // ← crash
}

// After — the correct pattern:
Future<bool> initialize() async {
  ...
  return false;  // ← graceful
}
\`\`\`

**Rules:**
- Concrete rule 1
- Concrete rule 2
```

### Writing Principles

- **Concrete code for pitfalls** — show the actual Before/After, not pseudo-code
- **State the "why"** — future readers need the reasoning, not just the rule
- **Keep it scannable** — tables and bullet lists over paragraphs
- **Reference version** — note which version the issue was found in
- **Link files** — `See path/to/file.dart` for implementation context
- **English** — all `agents/rules/` content in English (per `documentation.rules.md`)
- **~30 lines max** per section — if longer, make it more concise

### Anti-patterns

| Don't | Do instead |
|-------|-----------|
| "MUST NOT do X" without reason | "Don't do X because Y happens" |
| Vague rule ("be careful with players") | Specific pitfall ("native FIFO limit evicts oldest player when >4 concurrent") |
| Duplicating existing rule | Link: `→ see [cancel.rules.md](cancel.rules.md)` |
| Domain-specific names in examples | Generic names unless the pitfall IS domain-specific |

## Related

- [documentation.rules.md](documentation.rules.md) — writing style, token efficiency, structure patterns
- `/retro` skill — session retrospective workflow that uses this guide
