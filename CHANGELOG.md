# Changelog

## Unreleased

The wasm build loads in DuckDB-Wasm (#9). Every published wasm artifact had failed `LOAD`. The other families' decisions plot too: anofox-visualization v2026.11.5 has a schema for each.

### Added

- **`selection`, `sequence` and `weights` producers** (#19, #20): the long forms are tagged in
  `duckdb_functions()` with `anofox.contract = '1'` and the schema they already returned, so `anofox_plot(t)`
  draws them. No column changes.
  - `knapsack_by`, `assortment_by` and `assortment_recapture_by` return `selection`: a bar per item, picked or not;
  - `sequence_by` returns `sequence`: a step line of the cumulative setup, one step per position and item;
  - `portfolio_by` returns `weights`: the weight per asset.
- The visualization pickup check renders these five and checks their values: every item a bar of its measure
  and its picked flag, every step at its cumulative setup, every asset a segment of its weight.
- **DuckDB-Wasm runtime test** (`test/wasm`, from anofox-forecast): the whole `test/sql` suite runs in
  DuckDB-Wasm under Node against the v1.5.6 `wasm_eh` artifact, and the v1.5.6 deploy waits for it.
  The harness compares the way DuckDB's own runner does: integers exactly (BIGINT keeps its precision),
  text exactly, doubles within 1e-9 relative, by each column's real type. It checks column and row
  counts first, and fails on any record keyword it does not support or any record that runs over 60 s.
  `extension-upload.sh` LOADs the exact bytes and runs `test/wasm/post_upload_smoke.test` on them
  before the first upload, so an artifact that fails never reaches the versioned or latest path.
  After the upload the deploy job checks what it published again (`scripts/extension-verify-upload.sh`).
  `make audit` runs the self-tests of the section walker and of the comparator, and `make test_wasm`
  runs the suite against `build/wasm_eh`.
- **`wasm_eh` in the community registry**: the descriptor no longer excludes it. `wasm_mvp` stays excluded,
  because there an extension's errors reach the caller as `_setThrew is not defined` instead of their
  message. `wasm_threads` stays excluded because nothing tests it (no Node bundle).
- **`anofox_optimize_knapsack_max_memory`** (default `'512MB'`, DuckDB size strings such as `'2GB'`): the
  largest table `knapsack_exact` and `knapsack_best_of` build for their dynamic program. The table needs
  8 bytes per capacity unit plus one bit per cell, so one item at a large capacity could pass the
  50-million-cell limit and still need hundreds of MB. Over the budget `knapsack_exact` raises, naming
  the setting and its value; `knapsack_best_of` returns its best greedy answer. NULL, negative and
  unlimited (`'none'`) values are rejected.

### Changed

- `knapsack_best_of` also runs the exact DP whenever `knapsack_exact` would accept the instance (at most
  50 million cells and within `anofox_optimize_knapsack_max_memory`), so it is the proven optimum there
  for weights in multiples of 0.0001; over either limit it still returns the best greedy answer without
  raising. `knapsack_by`'s default `['best_of']` is therefore exact on small instances — on a five-crate
  fixture it went from 31 to the optimum of 36 — and pays the DP's cost once per group. Both functions
  share one limit check. The DP is skipped when greedy provably is the optimum (every positive-value item
  fits, nothing fits, or the best-ratio prefix fills the capacity exactly), which helps only on such
  instances.
- Items heavier than the capacity no longer influence `knapsack_exact`'s weight scaling, so one
  irrelevant `123.4567` no longer multiplies the table by 10^4.

### Fixed

- **wasm artifacts LOAD in DuckDB-Wasm** (#9). Two defects, each enough on its own to break `LOAD`:
  - the PostHog telemetry client was compiled into the Emscripten build, which left 25 `SSL_*`/`TLS_*`
    functions as unresolved imports (`t is not a function`). Telemetry is now off under Emscripten, as
    on MinGW, and vcpkg skips `openssl` for `wasm32`. Native builds keep telemetry;
  - `scripts/extension-upload.sh` appended a second `duckdb_signature` section to an artifact that
    already ended with one (`Unknown ABI type of value '208'`). It now replaces the 256-byte placeholder
    in place, as for native binaries, and `scripts/wasm_signature_sections.py check` requires exactly one
    section before and after signing. Ported from anofox-forecast#272.
- **`SET anofox_telemetry_enabled = false` works in every build.** The wasm and MinGW builds, which have no
  telemetry, did not register the `anofox_telemetry_*` options, so the documented opt-out failed with
  "unrecognized configuration parameter". The options now exist everywhere; there they change nothing, and
  `anofox_telemetry_enabled` defaults to `false`.
- **`knapsack_exact` could return an over-capacity answer.**
  - Weights not whole at the 10^4 scale were rounded to NEAREST inside the DP table, so an item could
    weigh less there than it really does: `opt_knapsack_exact([1.0, 1.0], [0.00004, 0.00004], 0.00007)`
    took both, 8e-05 against a capacity of 7e-05. Such weights are now rounded UP and the capacity down.
  - An absolute 1e-9 tolerance treated near-whole weights as whole, rounding them DOWN:
    `[3.0000000005, 3.0000000005]` at capacity 6 took both (6.000000001), and `[5e-10]` at capacity 0 was
    taken. The tolerance is now a few ulps relative to the value, and a positive weight never becomes 0
    table units.

  Every answer now fits. It is the proven optimum when weights and capacity are multiples of 0.0001, and
  optimal for the rounded-up weights otherwise. `total_weight` is still the sum of the original weights.
- `knapsack_exact` on an instance where no item fits, the empty list included, returns the empty
  selection without building a table: `opt_knapsack_exact([], [], 1e12)` tried to allocate 8 TB.

## v2026.10.26

**Breaking:** every index the solvers return is a 1-based BIGINT, the SQL convention (#7). Producers gain `row_order`.

### Breaking

- **Every index is a 1-based BIGINT, the SQL convention** (#7):
  - `pack_*` bins: `assignment` now runs 1..`bins_used`; it was 0-based;
  - `sequence_*` and `schedule_*`: the `order` lists are 1-based positions, so `items[order[k]]` is the
    *k*-th item; they were 0-based;
  - `wave_*` was already 1-based, and `avg_weighted_wave` is unchanged;
  - the index lists and `bins_used` are `BIGINT` instead of `UBIGINT`, so they can index a list directly.
    DuckDB has no implicit `UBIGINT` → `BIGINT` cast for that.

  Callers that added 1 to present a bin, or looked up `items[order[k] + 1]`, must drop the `+ 1`.
  `matrix_from_triples(…, index_base)` and the matrix layouts are unchanged, including the schedule
  matrix's row 0 for the virtual start.

### Added

- **`row_order`**, the last column of every `*_by` / `*_objectives_by` producer: a BIGINT numbering the
  table 1..n in the producer's own order (anofox-statistics#242). `anofox_plot_*` sorts rows, and this
  column lets a consumer keep the producer's order.

## v2026.10.25

optimize joins the anofox integration contract: decisions plot in anofox-visualization with `anofox_plot(t)`.
The community build also returns for DuckDB v1.5.6 (#8).

### Added

- **anofox integration contract** (#6): decisions plot in
  [anofox-visualization](https://github.com/DataZooDE/anofox-visualization) with no hand-written unnest SQL.
  - Table macros run the solvers over a table's rows and return one row per item:
    - `pack_by` and `wave_by` return the contract's `assignment` schema (bin or wave, item, size, capacity);
    - `schedule_by` returns the `schedule` schema, a Gantt chart (resource, task, start, end, setup, due,
      tardiness, status).
  - `pack_objectives_by`, `wave_objectives_by` and `schedule_objectives_by` return the solvers' objectives as
    the `summary` schema.
  - `algorithms :=` names the family members to run, and each becomes a `model_id`, so algorithms compare
    in one plot. `group_col :=` splits the rows into instances.
  - `knapsack_by`, `sequence_by`, `assortment_by`, `assortment_recapture_by` and `portfolio_by` return the other
    families' decisions one row per item. The contract has no schema for these yet, so they are not tagged.
    Their `*_objectives_by` are tagged `summary`.
  - Matrix inputs are pairs tables with the columns `(from_id, to_id, value)`: `setup_data` (schedule,
    sequence), `substitution_data` and `covariance_data`. A missing pair is 0, a duplicate is an error, and a
    covariance pair given once counts both ways.
  - Each macro exists as `anofox_optimize_*` and `opt_*`. The producers are tagged in `duckdb_functions()`
    (`anofox.contract = '1'`, `anofox.output`), and every optimize function carries
    `anofox.family = 'optimize'`.
  - `anofox_contract_version()` returns `'1'`. It is registered only if no other anofox extension has
    registered it.
- **Visualization pickup check**: the built extension next to a released anofox_visualization in the
  official DuckDB CLI. It runs as `scripts/visualization_pickup.py`, `make test_visualization`, and a CI job
  that deploy waits for. It checks both load orders and the tags, and runs every producer's output through
  `anofox_plot`, which must detect its schema; items and jobs must keep their sizes and times.
- `scripts/generate_api_reference.sh` lists table macros with their parameter names.

## v2026.10.22

Builds on **DuckDB 2.0.0** and v1.5.6 (1.4.x is no longer supported).

- Support DuckDB v2.0.0 alongside v1.5.6 using shared compatibility helpers.
- Return NULL from matrix-from-triples when any outer triple list is NULL.
- Cover NULL rows, reused chunks, and constant list/struct results in SQL tests.

## v2026.09.26

First release. Versions use CalVer (`vYYYY.MM.DD`, the date the binary set was cut),
matching the rest of the anofox and erpl extensions.

### Documentation

- All 80 catalog entries document themselves in `duckdb_functions()`: description,
  runnable example, real parameter names and categories, up from 40 of 80. An agent
  connected to a DuckDB database can only learn what an extension does by querying that
  view.
- Aliases (`opt_*`) now carry the full description alongside `alias_of`, rather than
  being left bare. An agent handed the short name should find documentation at the short
  name, not have to follow a pointer. The prompt-size concern that motivated the old
  behaviour moved to anofox-evolve, which renders an alias as a bare signature tagged
  `[alias of ...]`.
- Parameter names come from the `ReadList()`/exception strings in the implementations, so
  a caller who hits `processing_times must be >= 0` can find the argument being
  complained about.


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
