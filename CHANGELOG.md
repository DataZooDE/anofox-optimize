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

### Notes
- `bin_completion` is a *bounded* completion heuristic (seed + exhaustive
  pairs + greedy top-up), not Korf's algorithm and not optimal. On a
  Falkenauer triplet instance it reaches 11 bins where every greedy member
  stalls at 12 and the optimum is 10.
- Every algorithm is checked for feasibility on an adversarial instance:
  one assignment per item, bin ids in range, no bin over capacity, never
  below the theoretical lower bound.
