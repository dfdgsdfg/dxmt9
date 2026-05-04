---
name: "retro"
description: "Retrospect the current session and capture reusable learnings into agents/rules/ files. Use when the user says 'retro', 'retrospect', '회고', 'session review', 'what did we learn', or at the end of a work session when patterns, pitfalls, or conventions were discovered that should be remembered for future sessions."
---

# Session Retrospective

Review what was done in the current session, identify reusable patterns and pitfalls, and persist them into `agents/rules/` files so future sessions benefit.

For what to record, how to write it, and what to skip → see [@agents/rules/documentation_memory.rules.md](../../agents/rules/documentation_memory.rules.md).

## Workflow

### Step 1: Gather Session Context

Run in parallel:

```bash
# What changed this session
git log --oneline -20

# What files were touched
git diff --stat HEAD~5..HEAD

# Current branch context
git branch --show-current
```

Also review the conversation history for:
- Bug fixes (what was the non-obvious root cause?)
- Patterns that were established or discovered
- Mistakes that were made and corrected
- Design decisions with reasoning not captured in code

### Step 2: Filter for Rule-Worthy Learnings

Apply the filter from `documentation_memory.rules.md`:

**Record:** pitfalls, gotchas, constraints, design decision reasons, edge cases, trade-offs, assumptions, limitations, risks, exceptions to rules.

**Skip:** general patterns derivable from code, what was done (git log), standard usage, one-off obvious fixes.

**Litmus test:** "If I lost all memory and saw similar code next month, would I make the same mistake without this rule?"

### Step 3: Identify Target Rule Files

```bash
ls agents/rules/*.rules.md
```

Match each learning to an existing file. Prefer extending over creating new files.

### Step 4: Write the Rule Update

Follow the structure from `documentation_memory.rules.md`: Problem → Before/After code → Rules list.

### Step 5: Verify

1. Read back the edited section — does it fit the surrounding context?
2. Is the section under ~30 lines?
3. Does it state the "why"?

### Step 6: Report

| Learning | Rule File | Section |
|----------|-----------|---------|
| ... | ... | ... |
