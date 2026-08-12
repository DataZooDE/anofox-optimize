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
