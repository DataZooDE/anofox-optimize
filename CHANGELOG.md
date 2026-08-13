# Changelog

## 0.1.0 — unreleased

First release. Combinatorial **decision** algorithms as DuckDB functions,
so an LLM-driven search can change algorithm by editing text.

### Added
- Bin-packing family, all sharing one signature so swapping an identifier
  swaps the method and nothing else:
  `first_fit_decreasing`, `best_fit_decreasing`, `worst_fit_decreasing`,
  `next_fit`, `local_search`, `bin_completion`, `best_of` — each under a
  canonical `anofox_optimize_pack_*` name and a short `opt_pack_*` alias.
- `anofox_optimize_version()`, stamped by the build.
- Anonymous usage telemetry, off with `SET anofox_telemetry_enabled=false`.
- `docs/API_REFERENCE.md`, generated from `duckdb_functions()` rather than
  hand-written, by `scripts/generate_api_reference.sh`.

### Added since the initial packing family
- Knapsack (`greedy_ratio`, `greedy_value`, `exact`, `best_of`), wave batching,
  sequencing, scheduling, assortment and portfolio families — eight families,
  40 functions, each with a canonical `anofox_optimize_*` name and a short
  `opt_*` alias registered as a TRUE alias (`alias_of`) carrying no duplicate
  documentation.
- `anofox_optimize_assortment_recapture_*`: the delisting economics, where a
  dropped product's demand flows to listed substitutes. Distinct from
  `assortment_*`, which models cannibalisation among listed products. The two
  take identical arguments and answer opposite questions, so every description
  now opens with its MODEL and names the sibling family.
- `anofox_optimize_schedule_exact`: the proven optimum for at most 14 jobs,
  refusing larger inputs with an error naming the heuristic to use instead.
- `anofox_optimize_matrix_from_triples`: build a row-major matrix from
  `(from, to, value)` rows, the shape tables actually store.
- `docs/guides/` (how-to) and `docs/theory/` (algorithms, guarantees, failure
  modes), plus `test/sql/pilot_scenarios.test`, which asserts against optima
  computed OUTSIDE this extension.

### Fixed
- Scheduling local search only tried adjacent swaps and returned 168 against a
  proven optimum of 60. It now also swaps arbitrary pairs and relocates a job
  to any position, and reaches 60.
- The first `schedule_exact` kept one label per (set, last job), which is wrong
  under sequence-dependent setups because the elapsed clock depends on the path
  taken; it returned 91, losing to a heuristic. Replaced with a Pareto frontier
  of (time, cost) labels per state.
- `portfolio_greedy_sharpe` could not improve when the holding limit equalled
  the minimum support; a swap phase was added (0.6156 -> 0.9004 on the fixture).
- Assortment local search could return an over-capacity packing by moving an
  item into the bin being dissolved.
- CI could not have passed: the pipeline enabled Rust and ran cargo jobs in a
  repository with no `Cargo.toml`, and referenced `_extension_smoke_test.yml`
  and `scripts/extension-upload.sh`, neither of which existed.
- Renamed `portfolio_top_return_optimised` to `_optimized`: American spelling,
  matching the extension's own name and every sibling.

### Notes
- `bin_completion` is a *bounded* completion heuristic (seed + exhaustive
  pairs + greedy top-up), not Korf's algorithm and not optimal. On a
  Falkenauer triplet instance it reaches 11 bins where every greedy member
  stalls at 12 and the optimum is 10.
- Every algorithm is checked for feasibility on an adversarial instance:
  one assignment per item, bin ids in range, no bin over capacity, never
  below the theoretical lower bound.
