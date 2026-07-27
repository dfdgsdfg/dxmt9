---
type: "Spec Requirements"
title: "Harness Reduce Requirements — Log Reduction"
description: "Requirements for the reduce domain: turning dxmt9's own logs into the CSV/Markdown artifacts join and gate consume by column name."
tags: [specs, experiments, harness, reduce, requirements]
---

# Harness Reduce Requirements — Log Reduction

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `reduce` domain
named in `specs/experiments/harness/spec.md` §1. The reduce domain
participates in the `log-reduce` pipeline stage (parent spec.md §0)
only: it turns dxmt9's own stderr/log lines and JSON dumps into the
CSV/Markdown artifacts that `join`-domain and `gate`-domain scripts
consume by column name across the `dump-extract → log-reduce` and
`log-reduce → compare-gate` boundaries (parent spec.md §2).
Requirement IDs in this file use the prefix `R-HARN-REDUCE-`.

---

## 1. Scope Is a Rule, Not Only the Three Named Examples

**R-HARN-REDUCE-1.1** Unlike the `runner`, `probe`, and `replay` rows
of the parent domain map, the `reduce` row is stated as a rule with
examples, not an exhaustive enumeration: "the
`scripts/tools/summarize_*` that read dxmt9's own logs, including
`summarize_3dmark05_perf.py`, `summarize_index_cache_runtime.py`,
`summarize_framegraph_dag.py`" (parent spec.md §1, emphasis on
"including"). This document verifies and covers exactly those three
named scripts. It must not be read as certifying that every other
`scripts/tools/summarize_*` script belongs to this domain, and a
future edit must not silently expand this document's scope to another
`summarize_*` script without independently applying the origin test in
§2 to it first. Instantiates R-HARN-1.1.

**R-HARN-REDUCE-1.2** This domain's own `spec.md` names only
`log-reduce` (parent spec.md §0) as a stage it participates in; a
change to this domain's scripts must not describe `dump-extract`,
`offline-replay`, `external-join`, or `compare-gate` as this domain's
own responsibility, even though this domain's scripts are invoked
inline by scripts belonging to two other domains (`probe`'s
`run_3dmark05_perf_probe.sh` and `join`'s
`finalize_3dmark05_perf_probe.sh` each independently invoke
`summarize_3dmark05_perf.py` and `summarize_index_cache_runtime.py`).
Instantiates R-HARN-1.1 (parent spec.md §1, "Why the domain axis is
harness families, not stages").

---

## 2. Input Origin, Not Output Shape, Places a Script in This Domain

**R-HARN-REDUCE-2.1** A script belongs to this domain only if its
*primary* input originates from dxmt9 itself — a `[dxmt9-perf*]` or
`[dxmt9-bridge-perf]` stderr line, `result.json`, or a
`DXMT9_RENDERER_DUMP_DAG` JSON dump — and not from an external tool's
export (an Xcode "Export Encoder Counters" CSV, an `xctrace` XML/TSV
export), per the reduce-versus-`join` rule stated in parent spec.md
§1. A change that adds a new script to this domain's inventory, or
that changes an existing script's primary input source, must
re-apply this origin test rather than assuming a script belongs here
because its output is a CSV or Markdown file — the parent rule is
explicit that "the distinction is the origin of the input artifact,
not the shape of the output." Instantiates R-HARN-1.1 (parent
spec.md §1's reduce-versus-join rule).

**R-HARN-REDUCE-2.2** A script whose immediate input is itself a
`reduce`-domain-produced CSV, rather than a raw dxmt9 log line or
`result.json` directly, still belongs to this domain as long as no
external-tool export sits between it and dxmt9's own log — the
determining question is still "is the ultimate origin dxmt9 or an
external tool," applied by elimination when the parent rule's literal
wording ("a `[dxmt9-perf*]` stderr line, a `result.json`, a
`DXMT9_RENDERER_DUMP_DAG` JSON file") does not itself enumerate a
second-order case. A future change must not read this second-order
allowance as license to place a script here merely because it
consumes *some* CSV without checking that CSV's own origin.
Instantiates R-HARN-1.1.

**R-HARN-REDUCE-2.3** This document does not assert a domain for any
`scripts/tools/summarize_*` script other than the three named in
R-HARN-REDUCE-1.1, including ones whose docstring suggests a
dxmt9-produced input (for example, one that summarizes "dxmt9 color
attachment dump sidecars"). A reader must not treat this document's
silence about those scripts as either an inclusion or an exclusion;
classifying them requires the same origin test in §2, independently
applied, before either this document or
`specs/experiments/harness/join/*` may claim them. Instantiates
R-HARN-1.1.

---

## 3. Declared Line Prefixes; Malformed Matches Must Not Be Silently Absorbed

**R-HARN-REDUCE-3.1** Every distinct log-line family this domain
parses is bound to one literal, fully-anchored stderr prefix string
(for example `[dxmt9-perf-encoder ]`, `[dxmt9-perf-indexed-probe-draw
]`), declared in this domain's own `spec.md`, per parent R-HARN-5.1.
A change that adds a new line family must add its exact prefix string
to that declaration before any parser change ships, not leave the
prefix implicit in a regular expression a downstream reader must
reverse-engineer.

**R-HARN-REDUCE-3.2** A line that matches a declared prefix (§3.1) but
whose fields fail that family's own row-validity predicate must be
excluded from the emitted aggregate with a count or diagnostic that
distinguishes "excluded because unparseable" from "excluded because
the family had zero occurrences" — per parent R-HARN-5.2/2.2, a
harness must not silently fold an unparseable match into "zero
matches occurred" without a trace that a match was seen and rejected.
This is a `must`, stated as the contract this domain's parsers are
held to; it does not itself assert that every parser in this domain
satisfies it today — `spec.md` §4 records, per parser, whether it
does.

**R-HARN-REDUCE-3.3** A JSON-shaped reduce input (a
`DXMT9_RENDERER_DUMP_DAG` dump) whose top-level shape does not match
this domain's declared schema (not a JSON object; missing a required
list field) is a hard failure that names the file and the specific
missing or malformed field, not a partial or best-effort parse that
silently proceeds on whatever fields happen to be present. Instantiates
R-HARN-5.3/2.1.

---

## 4. An Empty Reduction Is a Distinct, Reported State

**R-HARN-REDUCE-4.1** When a log-line family this domain tracks has
zero matching lines in a given run's log, the emitted artifact for
that family must carry, in band, a record distinguishable from "some
lines matched but every field happened to be zero" — per parent
R-HARN-3.1/3.2, a header-only CSV with zero data rows is exactly the
kind of artifact whose validity a downstream consumer must be able to
read from the artifact itself rather than re-deriving by opening the
file and counting rows. Instantiates R-HARN-3.1/3.2.

**R-HARN-REDUCE-4.2** A zero row count for one line family must not,
by itself, be conflated by a downstream reader with "this family's
gating environment variable was never set for this run" — the same
zero count also results from a real parsing regression that drops
every line of that family. Per parent R-HARN-3.3, a harness that
cannot distinguish these two causes has not executed the validity
assertion R-HARN-3.1 requires for that family, even if it reports a
row count of `0`; a bare count is necessary but not sufficient
evidence of validity. `spec.md` §5 records, with a verified example,
that today's row-count reporting does not yet make this distinction.

**R-HARN-REDUCE-4.3** A diagnostic flag that adds its own validity
assertion (for example, one that fails the run when a specific
counter never went positive) is bound by the same discipline as any
other validity assertion in this domain: it must exit non-zero with a
diagnostic naming the counter and its observed value when the
assertion fails, per parent R-HARN-3.3/6.1. It must not be treated as
a substitute for the general per-family emptiness assertion R-HARN-
REDUCE-4.1/4.2 describe, because it checks one specific counter, not
every emitted family.

---

## 5. Emitted CSV Column Names Are the Downstream Contract

**R-HARN-REDUCE-5.1** Every column name this domain's scripts write
into an emitted CSV is a stable, load-bearing identifier that a
`join`-domain or `gate`-domain script reads by name (per parent
spec.md's `log-reduce → compare-gate` boundary: "Column names and
units are the in-band contract; a `gate` script must fail on an
unrecognized or missing column rather than silently treating it as
zero"). A change to this domain's scripts must not rename, remove, or
change the meaning of an existing column without checking every
downstream reader of that CSV by name, because a renamed column is
indistinguishable, from the reader's side, from a column that was
never populated. Instantiates R-HARN-4.1/4.2 applied to this
boundary's coordinate system, which is "column name," not "byte
offset."

**R-HARN-REDUCE-5.2** The ordered column list for each CSV this
domain emits is defined in exactly one place in the owning script (a
module-level constant tuple/list), and this domain's own `spec.md`
states the verified column count for each, derived by importing the
script and reading the constant's length rather than by hand-counting
prose. A future column addition or removal must update both the
script's constant and this domain's `spec.md` count together.
Instantiates R-HARN-1.3 (this domain's own descriptive obligation,
mirrored from `agents/rules/environment_variables_*.rules.md`'s
"descriptive, not a behavioral spec" boundary applied to column
lists instead of env vars).

---

## 6. This Domain Sets No Contract-Relevant Environment Variable

**R-HARN-REDUCE-6.1** No script in this domain sets, exports, or
forwards any `DXMT9_*`/`DXMT_*` environment variable into a
subprocess it launches — this domain's scripts read only file paths
given on their own command line (a log path, a `result.json`
directory, CSV paths, JSON dump paths). A change that adds an
`os.environ`/`getenv` read or write to a script in this domain is a
deviation from this requirement and must either be reverted or must
first update this document to state, and justify, the new ownership.
Instantiates parent spec.md §4 Rule 1 (single owning domain) by
declaring this domain's own share of that rule to be exactly zero
variables.

**R-HARN-REDUCE-6.2** The presence, absence, or shape of a given
log-line family in a run's `dxmt9.log` is controlled entirely by
whichever `runner`- or `probe`-domain script set that family's gating
environment variable before the run started; this domain never
observes or depends on the gating variable's own value, only on
whatever lines the already-completed run's log actually contains. A
change to this domain's parsers must not add a fallback that
re-derives, guesses, or defaults a gating variable's value from the
log content, because that would duplicate a decision that belongs to
the `runner`/`probe` domain's own environment-ownership contract.
Instantiates R-HARN-1.3.

**R-HARN-REDUCE-6.3** When this domain's `spec.md` states that a
given log-line family's gating variable is set by the `probe` domain,
that claim must be checked against the `probe` domain's own actual
flag surface (`run_3dmark05_perf_probe.sh --help` and its `env_args`
forwarding), not assumed from the variable's name alone — a variable
can be genuinely gate-able only by direct caller environment, with no
corresponding `probe` wrapper flag, and this domain's `spec.md` must
say so plainly when that is what verification finds, rather than
implying every family this domain can parse has an equivalent `probe`
flag to turn it on. Instantiates R-HARN-5.1/5.2 (a harness's
dependency on another domain's gating surface must be verified, not
inferred).

---

## 7. Diagnostic Paths Carry the Primary Contract

**R-HARN-REDUCE-7.1** Every command-line flag accepted by any script
in this domain that alters its emitted output — a required validity
assertion, a `--stage` filter, a `--run` label, an output path
override — appears in this domain's own `spec.md` mode table.
Instantiates R-HARN-6.2.

**R-HARN-REDUCE-7.2** A flag whose entire purpose is to add or narrow
a validity check (this domain's one example being
`--require-uniform-compact-saved-bytes-present`) is bound by
R-HARN-3.3/6.1 the same as the domain's primary path: it must actually
execute the check it advertises and exit non-zero on failure with a
diagnostic naming the checked counters, not merely accept the flag and
otherwise proceed as if the flag were absent. Instantiates R-HARN-6.1.
