# anofox-optimize — notes for agents

Combinatorial **decision** algorithms as DuckDB functions.

## Why this exists

anofox-evolve's LLM-driven search could not cross algorithm families on
its combinatorial pilots. With only plain SQL available, a search on a
Falkenauer triplet bin-packing instance produced 15 textually distinct
candidates that all scored the same first-fit-decreasing result — 12 bins
against an optimum of 10. The premise *a text edit is an algorithm
change* needs the algorithms to exist as callable functions.

## The rule that shapes everything here

**A family shares one signature.** Swapping `opt_pack_first_fit_decreasing`
for `opt_pack_bin_completion` must change the method and nothing else, or
the search cannot explore methods by editing text.

**A family needs spread.** If every member returns the same answer on the
instances that matter, the family is decorative. Four greedy heuristics
all returned 12 bins on the triplet instance; `bin_completion` returning
11 is what makes the family worth exposing. Check spread when adding one.

**Include a deliberately weak member.** A search needs a poor-but-valid
option to move away from — hence `opt_pack_next_fit`.

## Non-negotiables

- **Never return an infeasible answer.** A search optimises against these
  functions, so a member that quietly returns an over-capacity packing
  simply wins. `test/sql/packing.test` checks feasibility for *every*
  algorithm: one assignment per item, bin ids in range, no bin over
  capacity, never below the theoretical lower bound.
- **Never accept nonsense silently.** Negative and non-finite sizes each
  produced confident wrong answers before they were rejected.
- **A family must name its objective when another family shares its
  signature.** `assortment_*` and `assortment_recapture_*` take identical
  arguments and return an identical struct, but score a shelf under
  opposite economics — cannibalisation among the listed, versus recapture
  from the delisted. Nothing in the data says which one a caller means.
  This is not hypothetical: a live search called the wrong one in 6 of 6
  runs and converged BELOW a trivial top-(margin*demand) ranking, 63% short
  of the optimum, because optimising one objective while being scored on
  the other is worse than not optimising at all. Every such description now
  opens with `MODEL:` and names the sibling family as the alternative. A
  shared boilerplate sentence would have hidden exactly this.
- **A fixture that cannot separate the members proves nothing.** Three
  pilots looked like "the search adds nothing" until their instances were
  checked: 43 distinct policies were scoring one identical value. Build the
  instance so the obvious heuristic is measurably far from optimal, compute
  the optimum independently (brute force, DP, enumeration), and assert on
  the gap — a test that only checks "runs without error" passes on a
  degenerate instance.
- **Descriptions must not overclaim.** `duckdb_functions()` is the single
  source of truth that anofox-evolve derives its prompt vocabulary from,
  so a description that overstates the algorithm poisons the search. Say
  what the code does, including its weaknesses.

## Conventions

- Canonical `anofox_optimize_*` names with short `opt_*` aliases, as
  anofox-statistics does (`anofox_stats_aic` + `aic`).
- Version comes from the build (`EXT_VERSION_ANOFOX_OPTIMIZE`), never a
  hardcoded constant.
- BSL 1.1, `datazoo-banner`, DuckDB v1.5.5.

## Not yet present

posthog telemetry, `docs/`, `scripts/`, CI workflows, and the Rust
`crates/` layout the sibling extensions use. This extension is pure C++
because the algorithms need no external crate.
