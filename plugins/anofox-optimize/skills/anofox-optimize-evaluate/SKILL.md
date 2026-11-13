---
name: anofox-optimize-evaluate
description: >
  Judging and comparing anofox_optimize results in DuckDB: the
  `*_objectives_by` table macros (`opt_pack_objectives_by`,
  `opt_schedule_objectives_by`, …) that score every algorithm per group as a
  model comparison, feasibility checks (every item placed once, no bin over
  capacity in item order, permutations, holding and position limits), bounds
  computed in plain SQL, the exact members as ground truth
  (`opt_schedule_exact` up to 14 jobs, `opt_knapsack_best_of` where its DP
  runs) and the deliberately weak baselines (`opt_pack_next_fit`,
  `opt_schedule_edd`, `opt_sequence_as_given`, …). Use when asked whether an
  optimization result is any good, which algorithm to use, how far a
  heuristic is from optimal, what optimizing saves over today's policy, or
  how to write a test that could actually fail.
version: 2026.11.7
user-invocable: false
---

# Anofox Optimize — Evaluate and Compare

**Extension:** `anofox_optimize` v2026.11.7 | **DuckDB:** v1.5.6 / v2.0.0 | **Dual naming:** `anofox_optimize_*` and `opt_*` (true aliases, same description)

An optimizer's output looks equally plausible whether it is optimal or badly
wrong. This library's own scheduling local search once returned 168 on an
instance whose proven optimum was 60, and nothing about the answer looked
wrong. Check feasibility, bound the gap with something independent, and
compare against the weak member.

## Critical gotchas

1. **Only two members can prove optimality, within limits.** `schedule_exact`
   refuses above 14 jobs (exponential). `knapsack_best_of` (and its alias
   `knapsack_exact`) is exact only where its dynamic program ran: up to 50
   million cells and `anofox_optimize_knapsack_max_memory` (default 512MB,
   64MB in DuckDB-Wasm), with weights in multiples of 0.0001 whose sums are
   exact in doubles. Past the limits it **silently** returns its best greedy
   answer and nothing in the result says so. Everything else is heuristic.
2. **Objectives are the solver's own numbers**, not recomputed. For an
   independent check, recompute from the decision (examples below).
3. **"Fits" is the item-order sum in doubles.** Check capacity with
   `list_sum` over the sizes in item order, not an unordered `sum()`, which
   has no defined order and can differ in the last bit.
4. **A fixture where every member agrees proves nothing.** If `best_of` ties
   the weak member, the instance is easy or something is broken; build the
   instance so the obvious heuristic is measurably far from optimal.
5. **A heuristic reference outranks nothing.** A bound proven by construction
   or exhaustive enumeration outranks any heuristic; a "best known" value from
   another heuristic is not a bound.

## Compare algorithms — `*_objectives_by`

Same arguments as the matching `*_by`; one row per group, algorithm and metric
(`model_id, metric, value, conf_low, conf_high, row_order`; the confidence
columns are NULL — these are deterministic decisions).

| Objectives | Metrics |
|---|---|
| `opt_pack_objectives_by` | `bins_used` |
| `opt_wave_objectives_by` | `avg_weighted_wave`, `waves_used` |
| `opt_knapsack_objectives_by` | `items_selected`, `total_value`, `total_weight` |
| `opt_sequence_objectives_by` | `total_setup` |
| `opt_schedule_objectives_by` | `late_jobs`, `makespan`, `total_weighted_tardiness` |
| `opt_assortment_objectives_by`, `opt_assortment_recapture_objectives_by` | `captured_margin`, `items_listed` |
| `opt_portfolio_objectives_by` | `expected_return`, `holdings`, `sharpe`, `volatility` |

```sql
-- what today's rule (EDD) costs against the best the family finds and the proven optimum
SELECT model_id, value AS total_weighted_tardiness
FROM opt_schedule_objectives_by('jobs', 'job', 'minutes', 'due', 'priority',
                                setup_data := 'changeovers',
                                algorithms := ['edd', 'atcs', 'best_of', 'exact'])
WHERE metric = 'total_weighted_tardiness'
ORDER BY value;
```

```sql
-- bins per warehouse and algorithm, as one table
PIVOT (SELECT warehouse_id, model_id, value
       FROM opt_pack_objectives_by('items', 'item_id', 'size', 1000.0,
                                   group_col := 'warehouse_id',
                                   algorithms := ['next_fit', 'first_fit_decreasing', 'best_of']))
ON model_id USING any_value(value);
```

## Feasibility first

Cheap, and what a buggy member would violate. (An early `pack_local_search`
returned fewer bins than the lower bound; feasibility checks caught it.)

```sql
-- packing: every item placed once, no bin over capacity in item order
WITH r AS (
  SELECT opt_pack_best_of(list(size ORDER BY pos), 1000.0) AS p,
         list(size ORDER BY pos) AS sizes
  FROM ordered_items
)
SELECT len(p.assignment) = len(sizes) AS every_item_placed,
       bool_and(list_sum(list_filter(sizes, lambda s, i: p.assignment[i] = b)) <= 1000.0) AS within_capacity
FROM r, range(1, r.p.bins_used + 1) t(b)
GROUP BY ALL;
```

```sql
-- scheduling / sequencing: the order is a genuine permutation of 1..n
WITH r AS (SELECT opt_schedule_best_of(p, d, w, s, n)."order" AS ord, n FROM jobs_input)
SELECT list_sort(ord) = range(1, n + 1)::BIGINT[] AS is_a_permutation FROM r;
```

```sql
-- portfolio: at most k holdings, no position over the cap, weights sum to 1
WITH r AS (SELECT opt_portfolio_best_of(mu, cov, 5, 0.35) AS p FROM inputs)
SELECT len(list_filter(p.weights, lambda w: w > 1e-9)) <= 5          AS cardinality_ok,
       list_max(p.weights) <= 0.35 + 1e-9                             AS cap_ok,
       abs(list_sum(p.weights) - 1.0) < 1e-6                          AS weights_sum_to_one
FROM r;
```

## Bound the gap with something independent

```sql
-- packing: no packing beats ceil(total size / capacity)
SELECT ceil(sum(size) / 1000.0) AS lower_bound,
       (SELECT opt_pack_best_of(list(size ORDER BY item_id), 1000.0).bins_used FROM items) AS best
FROM items;
```

```sql
-- scheduling: how far is the heuristic on an instance small enough to solve exactly (<= 14 jobs)
SELECT opt_schedule_local_search(p, d, w, s, n).total_weighted_tardiness AS heuristic,
       opt_schedule_exact(p, d, w, s, n).total_weighted_tardiness        AS optimal
FROM jobs_input;
```

A heuristic that is near-optimal on small instances is not guaranteed to be on
large ones, but one that is far off on small ones will not improve with size.
For knapsack, keep the instance inside the DP limits (integers, or units scaled
down: pallets rather than grams) if you need the proven optimum.

## What optimizing buys — the weak member

Every family ships a poor-but-valid member on purpose: `pack_next_fit`,
`wave_as_given`, `knapsack_greedy_value`, `sequence_as_given`,
`schedule_edd`, `assortment_top_margin`, `assortment_recapture_top_margin`,
`portfolio_top_return`.

```sql
SELECT opt_pack_next_fit(sizes, cap).bins_used AS today,
       opt_pack_best_of(sizes, cap).bins_used  AS optimized
FROM inputs;
```

```sql
-- the incumbent run order against the improved one
SELECT model_id, value AS total_setup
FROM opt_sequence_objectives_by('jobs', 'job', 'setup_costs_pairs',
                                algorithms := ['as_given', 'nearest_neighbour', 'two_opt'])
ORDER BY value;
```

## Tests that could fail

Assert against a value computed outside the library, and assert that the
naive rule is beaten:

```sql
-- 60 is an independent Held-Karp optimum for this instance (pilot P6), EDD pays 617
SELECT opt_schedule_exact(p, d, w, s, n).total_weighted_tardiness = 60 AS exact_ok,
       opt_schedule_edd(p, d, w, s, n).total_weighted_tardiness > 60   AS fixture_discriminates
FROM fixture;
```

```sql
SELECT opt_knapsack_greedy_ratio(v, w, cap).total_value
     < opt_knapsack_best_of(v, w, cap).total_value AS fixture_discriminates
FROM fixture;
```

## Choosing under a time budget

| Budget | Approach |
|---|---|
| Interactive, large input | a single greedy member |
| Interactive, moderate input | `best_of` |
| Batch, moderate input | `best_of`, then the exact member on a scaled-down instance to measure the gap |
| A guarantee is required | `schedule_exact` or `knapsack_best_of` within their limits; beyond them, a MIP solver |

The quadratic families (`assortment_*`, `portfolio_*`) are comfortable in the
hundreds of items, not the millions.

See also: `anofox-optimize-solve` (the `*_by` twins of these objectives),
`anofox-optimize-visualize` (plot an objectives table as a model comparison),
`anofox-optimize-formulate` (when every member scores the same, check the
family before the algorithm).

Reference docs: `docs/guides/04-validating-results.md`, `docs/theory/05-complexity-and-limits.md`,
`test/sql/pilot_scenarios.test`.
