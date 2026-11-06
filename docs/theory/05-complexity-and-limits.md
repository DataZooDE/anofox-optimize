# Complexity and Limits

> What is exact, what is heuristic, what will refuse to run, and where the
> real ceilings are.

## Exact vs heuristic

Two functions can return a proven optimum: `schedule_exact`, which refuses
what it cannot solve, and `knapsack_best_of` (alias `knapsack_exact`), only
when its dynamic program ran.

| Function | Guarantee | Cost | Limit |
|---|---|---|---|
| `knapsack_best_of` (alias `knapsack_exact`) | where the DP runs: proven optimum for weights in multiples of 0.0001 whose sums are exact in doubles; finer weights are rounded up for the DP and then carry no optimality guarantee, and decimal ones such as 0.1 can fall short (fits means summed in doubles, so 0.1 + 0.2 > 0.3) | O(n · capacity) | the DP runs up to **50 million cells** and within `anofox_optimize_knapsack_max_memory` (default `'512MB'`, `'64MB'` in DuckDB-Wasm); past them, the best greedy answer, silently |
| `schedule_exact` | proven optimum | exponential | refuses above **14 jobs** |

Past the limits `knapsack_best_of` returns its best greedy answer without
raising, that answer is not exact, and nothing in the result says which case
occurred. `knapsack_exact` used to raise there instead; it is now an alias of
`knapsack_best_of`, because inside the limits the two always returned the same
value.

Everything else is heuristic and carries no guarantee. Some have known
worst-case bounds — `first_fit_decreasing` never exceeds `11/9 · OPT + 6/9`
bins — but a bound is not an optimum, and most members have none at all.

The practical consequence: **you cannot tell how good a heuristic result is
by looking at it.** Use the exact solver on a scaled-down instance, or a
bound you can compute yourself. See
[Validating Results](../guides/04-validating-results.md).

## Two different meanings of "too big"

**Knapsack's cost scales with capacity, not item count.** `knapsack_best_of` on
60 items with capacity 1,000 is instant. The same 60 items with capacity
1,000,000,000 is not, because the DP table is indexed by capacity. If your
capacity is a large continuous quantity, scale your units down — capacity in
pallets rather than grams. Past the limits `knapsack_best_of` returns its best
greedy answer, which is not exact.
`knapsack_by`'s default (`best_of`) pays the DP's cost once per group.

**Scheduling's cost scales with job count.** `schedule_exact` explores states
over subsets of jobs, so cost roughly doubles per additional job regardless of
the numbers involved.

This is the difference between *weakly* and *strongly* NP-hard, and it is the
reason one exact solver is practical at 60 items while the other stops at 14.

## Why `schedule_exact` stops at 14 and not 20

A textbook Held-Karp keeps one state per (set, last job): `2ⁿ · n`, which is
tolerable to about 20. This implementation keeps a **Pareto frontier** of
`(time, cost)` labels per state instead, because with sequence-dependent
setups the elapsed clock depends on the path taken and one label per state is
simply wrong — see
[Sequencing and Scheduling](03-sequencing-and-scheduling.md).

Correctness costs memory. Each state holds every non-dominated trade-off, so
the frontier width multiplies the state count. 14 is where that stays
comfortable; the function refuses beyond rather than degrading into a long
silence.

## Complexity by family

| Family | Member | Time | Notes |
|---|---|---|---|
| `pack_*` | `next_fit` | O(n) | single pass |
| | `*_fit_decreasing` | O(n log n + n·b) | b = bins opened |
| | `bin_completion` | higher | searches combinations per bin |
| | `local_search` | O(n²) per improving pass | |
| `knapsack_*` | `greedy_*` | O(n log n) | |
| | `exact` | O(n · capacity) | pseudo-polynomial |
| `wave_*` | all | O(n log n) | ranking rules |
| `sequence_*` | `nearest_neighbour` | O(n²) | |
| | `two_opt` | O(n²) per improving pass | |
| `schedule_*` | `edd`, `wspt` | O(n log n) | ignore setups |
| | `atcs` | O(n²) | evaluates all candidates per step |
| | `local_search` | O(n²) evaluations per pass, each O(n) | swaps + relocation |
| | `exact` | exponential | ≤ 14 jobs |
| `assortment_*` | `top_margin` | O(n log n) | |
| | `greedy_marginal` | O(k · n · objective) | objective itself is O(n²) |
| | `local_search` | O(n²) objective evaluations per pass | |
| `portfolio_*` | `top_return` | O(n log n) | |
| | `*_optimized`, `greedy_sharpe` | O(k² · iterations) per candidate set | projected gradient |

The quadratic objectives (`assortment_*`, `portfolio_*`) are the ones to
watch: each *evaluation* is already O(n²) or O(k²), and local search performs
many of them. These families are comfortable in the hundreds of items, not
the millions.

## Numeric behaviour

**Ties.** Improvement loops require a strict improvement of more than `1e-12`
before accepting a move, so they terminate on plateaus rather than cycling
between equal-scoring solutions.

**Degenerate inputs.** Zero-variance portfolios return a Sharpe of 0 rather
than dividing by zero. Empty inputs return empty results rather than
erroring — an empty shelf recaptures nothing, an empty packing uses no bins.

**Rejected inputs.** Negative or non-finite sizes, substitution rates outside
[0,1], and duplicate matrix cells all raise. Each of these produced a
confidently wrong answer before it was rejected, which is why they raise
rather than being clamped: a clamped input silently answers a different
question than the one you asked.

## Choosing under a time budget

| Budget | Approach |
|---|---|
| Interactive, large input | the single greedy member |
| Interactive, moderate input | `best_of` |
| Batch, moderate input | `best_of`, then `*_exact` on a scaled-down instance to measure the gap |
| You need a guarantee | `schedule_exact`, or `knapsack_best_of` within its DP limits |

If you need a guarantee outside those limits, this library cannot give it to
you, and the honest answer is a dedicated MIP solver.
