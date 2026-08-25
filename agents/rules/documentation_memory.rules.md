---
description: How to update agents/rules/ files as project memory - what to record, what to skip, and how to write it
paths:
  - "agents/rules/*.rules.md"
globs: "agents/rules/*.rules.md"
alwaysApply: false
---

# Rules as Memory

`agents/rules/` is the project's institutional memory. Every session loads these files into context. A well-placed rule prevents the same mistake from recurring across developers and AI agents.

## What to Record

Rules capture the **non-obvious** - things code alone does not convey. Focus on:

| Category | Example |
|----------|---------|
| **Pitfall / Gotcha** | "`commit_chunk` import must validate payload bounds before retaining handles, or malformed PE data can leak backend refs." |
| **Constraint** | "Hot-path `Set*` and `Draw*` traffic must batch into `CommandChunk`; per-draw unix calls break DXMT merge readiness." |
| **Design Decision + Reason** | "dxmt9 uses POD wire records instead of DXMT lambda captures because chunks cross the Wine PE/unix boundary." |
| **Edge / Corner Case** | "Runtime shader probes do not prove packet-transform correctness; add native stateless assertions too." |
| **Trade-off** | "App-local provider fallback is diagnostic opt-in: safer isolation, less automatic recovery from missing files." |
| **Assumption** | "Wine-oracle D3D9 tests are behavior oracles, not a requirement to mirror Wine's implementation structure." |
| **Limitation** | "TLA+ covers queue invariants, but Metal encoder side effects still need implementation assertions or observers." |
| **Risk / Hazard** | "Missing `bridge_abi_hash` or version checks can pair incompatible `d3d9.dll`, `winemetal_dxmt9.dll`, and provider artifacts." |
| **Exception to a Rule** | "Runtime readback belongs in tests only for GPU-visible behavior that source/descriptor inspection cannot prove." |

## What NOT to Record

| Skip | Why |
|------|-----|
| General patterns derivable from code | Read the code |
| What was done | `git log` is authoritative |
| Standard library/framework usage | Docs exist |
| One-off fix with obvious cause | No recurrence risk |
| Implementation detail of one feature | Too narrow to be useful |

**Litmus test:** "If I lost all memory and saw similar code next month, would I
make the same mistake without this rule?" If no, skip it.

## How to Write

### Find the Right File

```bash
ls agents/rules/*.rules.md
```

Match the learning to an existing file. Prefer extending over creating new files. Create new only when no existing file covers the domain.

### Structure: Problem / Code / Rules

```markdown
### Section Title - Brief Description

Problem in 1-2 sentences. Version where discovered

\`\`\`cpp
// Before: decodes the payload before validating the range.
auto* draw = reinterpret_cast<const DrawRecord*>(payload + header.payloadOffset);
retainHandles(draw->firstHandle, draw->handleCount);

// After: validates range and handle table first.
if (!validatePayloadRange(header, payloadBytes))
    return E_INVALIDARG;
if (!validateHandleRange(header.firstHandle, header.handleCount, handleCount))
    return E_INVALIDARG;
retainHandles(header.firstHandle, header.handleCount);
\`\`\`

**Rules:**
- Concrete rule 1
- Concrete rule 2
```

### Writing Principles

- **Concrete code for pitfalls** - show the actual before/after, not pseudo-code
- **State the "why"** - future readers need the reasoning, not just the rule
- **Keep it scannable** - tables and bullet lists over paragraphs
- **Reference version** - note which commit, Wine version, or runtime exposed it
- **Link files** - `See specs/backend/requirements.md` for context
- **English** - all `agents/rules/` content in English
- **~30 lines max** per section - if longer, make it more concise

### Anti-patterns

| Don't | Do instead |
|-------|-----------|
| "MUST NOT do X" without reason | "Do not do X because Y breaks." |
| Vague rule ("be careful with bridge calls") | Specific pitfall ("`Set*`/`Draw*` hot paths must not emit per-call PE/unix bridge ops") |
| Duplicating existing rule | Link to the existing rule or spec section |
| Overgeneralizing one failure | Record the exact boundary, runtime, or test lane where it matters |

## Related

- [documentation.rules.md](documentation.rules.md) - writing style and structure
- `agents/skills/retro/SKILL.md` - session retrospective workflow
