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
  optimization result is feasible or any good, which algorithm did best on
  the user's data, how far a heuristic is from optimal, what optimizing saves
  over today's policy, or how to write a test that could actually fail.
version: 2026.11.13
user-invocable: false
---

# Anofox Optimize — Evaluate and Compare

**Extension:** `anofox_optimize` v2026.11.13 | **DuckDB:** v1.5.6 / v2.0.0 | **Dual naming:** `anofox_optimize_*` and `opt_*` (true aliases, same description)

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
   another heuristic is not a bound. Scheduling's `best_of` is one too: it
   does not run `exact` and can lose to it (55 against 47 on a 5-job instance).
6. **Compare assortment members within one family only.** `captured_margin`
   from `assortment_*` and from `assortment_recapture_*` measure different
   economics; recapture's numbers run higher on the same data, so a "which
   scored best" across the two always picks recapture. Which family applies
   is a business question (`anofox-optimize-formulate`), not a metric.
7. **`as_given` in a `*_by` long form is item-id order**, because the long
   forms take items in id order. It is today's order only when the ids sort
   that way; otherwise the "savings" over it are invented. Baseline today's
   order with the scalar form (below).

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
| `opt_assortment_objectives_by` | `captured_margin` (cannibalisation model), `items_listed` |
| `opt_assortment_recapture_objectives_by` | `captured_margin` (recapture model, not comparable with the row above), `items_listed` |
| `opt_portfolio_objectives_by` | `expected_return`, `holdings`, `sharpe`, `volatility` |

```sql
-- what the textbook rule (EDD) costs against the best heuristic and the proven optimum
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

Every check below returns booleans that must all be true. A check that only
looks at the length or the sum passes broken answers: an assignment of
`[1, 0]` has the right length, and bin 0 is never summed.

```sql
-- long form: per warehouse and algorithm, every item placed exactly once,
-- every bin within capacity summed in item order (the solver's own rule)
WITH plan AS (
  SELECT * FROM opt_pack_by('items', 'item_id', 'size', 1000.0, group_col := 'warehouse_id',
                            algorithms := ['next_fit', 'bin_completion', 'best_of'])
),
bins AS (
  SELECT warehouse_id, model_id, bin,
         list_sum(list(size ORDER BY item)) AS load, any_value(capacity) AS capacity
  FROM plan GROUP BY ALL
)
SELECT bool_and(load <= capacity)                                     AS every_bin_fits,
       (SELECT count(*) FROM plan) = 3 * (SELECT count(*) FROM items) AS every_item_placed,
       (SELECT count(DISTINCT (warehouse_id, model_id, item)) FROM plan)
         = (SELECT count(*) FROM plan)                                AS no_item_twice
FROM bins;
```

```sql
-- scalar: one valid bin per item, no empty bin, no bin over capacity in item order
WITH r AS (
  SELECT opt_pack_best_of(list(size ORDER BY pos), 1000.0) AS p,
         list(size ORDER BY pos) AS sizes
  FROM ordered_items
)
SELECT len(p.assignment) = len(sizes)                                AS one_bin_per_item,
       list_bool_and(list_transform(p.assignment,
           lambda a: a IS NOT NULL AND a BETWEEN 1 AND p.bins_used))  AS bin_ids_valid,
       len(list_distinct(p.assignment)) = p.bins_used               AS no_empty_bin,
       list_bool_and(list_transform(range(1, p.bins_used + 1),
           lambda b: list_sum(list_filter(sizes, lambda s, i: p.assignment[i] = b)) <= 1000.0))
                                                                     AS within_capacity
FROM r;
```

```sql
-- scheduling / sequencing: the order is a genuine permutation of 1..n
WITH r AS (SELECT opt_schedule_best_of(p, d, w, s, n)."order" AS ord, n FROM jobs_input)
SELECT list_sort(ord) = range(1, n + 1)::BIGINT[] AS is_a_permutation FROM r;
```

```sql
-- portfolio: one finite, non-negative weight per asset, at most k holdings,
-- no position over the cap, weights sum to 1
WITH r AS (SELECT opt_portfolio_best_of(mu, cov, 5, 0.35) AS p, mu FROM inputs)
SELECT len(p.weights) = len(mu)                                       AS one_weight_per_asset,
       list_bool_and(list_transform(p.weights,
           lambda w: w IS NOT NULL AND isfinite(w) AND w >= -1e-12))  AS weights_valid,
       len(list_filter(p.weights, lambda w: w > 1e-9)) <= 5           AS cardinality_ok,
       list_max(p.weights) <= 0.35 + 1e-9                             AS cap_ok,
       abs(list_sum(p.weights) - 1.0) < 1e-6                          AS weights_sum_to_one
FROM r;
```

## Recompute the objective yourself

The solver's objective is its own number (gotcha 2). Recomputing it from the
decision and the input catches a wrong matrix, a transposed pairs table, and
a result from the wrong model.

```sql
-- schedule: times chain (start = previous end + setup), setups come from the
-- pairs table, and the weighted tardiness adds up to what the solver reports
WITH s AS (
  SELECT *, lag("end", 1, 0) OVER (PARTITION BY model_id ORDER BY position) AS prev_end,
            lag(task)        OVER (PARTITION BY model_id ORDER BY position) AS prev_task
  FROM opt_schedule_by('jobs', 'job', 'minutes', 'due', 'priority',
                       setup_data := 'changeovers', algorithms := ['atcs', 'best_of'])
),
recomputed AS (
  SELECT model_id,
         bool_and(start = prev_end + s.setup AND "end" = start + processing) AS times_chain,
         bool_and(s.setup = coalesce(c.value, 0))                          AS setups_match_input,
         sum(priority * greatest("end" - due, 0))                           AS twt
  FROM s LEFT JOIN changeovers c
         ON c.from_id IS NOT DISTINCT FROM s.prev_task AND c.to_id = s.task
  GROUP BY model_id
)
SELECT bool_and(times_chain) AS times_chain, bool_and(setups_match_input) AS setups_match_input,
       bool_and(abs(r.twt - o.value) < 1e-9) AS tardiness_matches_solver
FROM recomputed r
JOIN opt_schedule_objectives_by('jobs', 'job', 'minutes', 'due', 'priority',
                                setup_data := 'changeovers', algorithms := ['atcs', 'best_of']) o
  ON o.model_id = r.model_id AND o.metric = 'total_weighted_tardiness';
```

```sql
-- assortment: score the chosen shelf under BOTH models; it must match its own
-- (1025 here), and the other model's number (1280) says nothing about it
WITH shelf AS (
  SELECT item, margin, demand, listed
  FROM opt_assortment_by('products', 'sku', 'margin', 'demand', 'substitution', 3)
),
taken AS (             -- demand a listed product loses to listed look-alikes
  SELECT s.to_id AS item, sum(s.value * j.demand) AS taken
  FROM substitution s JOIN shelf j ON j.item = s.from_id AND j.listed
  GROUP BY s.to_id
),
gained AS (            -- demand a listed product wins from delisted ones, at its own margin
  SELECT sum(j.demand * s.value * i.margin) AS gained
  FROM substitution s
  JOIN shelf j ON j.item = s.from_id AND NOT j.listed
  JOIN shelf i ON i.item = s.to_id AND i.listed
),
scores AS (
  SELECT sum(i.margin * greatest(i.demand - coalesce(t.taken, 0), 0))      AS as_cannibalisation,
         sum(i.margin * i.demand) + (SELECT coalesce(gained, 0) FROM gained) AS as_recapture
  FROM shelf i LEFT JOIN taken t USING (item)
  WHERE i.listed
),
reported AS (
  SELECT value AS reported
  FROM opt_assortment_objectives_by('products', 'sku', 'margin', 'demand', 'substitution', 3)
  WHERE metric = 'captured_margin'
)
SELECT abs(reported - as_cannibalisation) < 1e-9 AS matches_its_own_model
FROM reported, scores;
```

## Bound the gap with something independent

```sql
-- packing: no packing beats ceil(total size / capacity), per warehouse.
-- The tolerance keeps a total like 1.2000000000000002 from raising the bound.
SELECT o.warehouse_id, lb.lower_bound, o.value AS best_of_bins,
       o.value >= lb.lower_bound AS bound_holds
FROM opt_pack_objectives_by('items', 'item_id', 'size', 1000.0, group_col := 'warehouse_id') o
JOIN (SELECT warehouse_id, ceil(sum(size) / 1000.0 - 1e-9) AS lower_bound
      FROM items GROUP BY warehouse_id) lb USING (warehouse_id)
WHERE o.metric = 'bins_used'
ORDER BY o.warehouse_id;
```

When `best_of_bins` equals `lower_bound`, that packing is proven optimal.

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

To baseline **today's** run order, put the matrix in today's order and use
the scalar `as_given` (gotcha 7). Here today's position of each job is
`pos`; it costs 38, id order 34, and the improved order 14:

```sql
WITH today AS (SELECT job, (job * 7) % 10 + 1 AS pos FROM jobs),  -- your incumbent positions
m AS (
  SELECT opt_matrix_from_triples(list(a.pos), list(b.pos), list(c.cost), 10, 1) AS setup
  FROM setup_costs c
  JOIN today a ON a.job = c.from_job
  JOIN today b ON b.job = c.to_job
)
SELECT opt_sequence_as_given(setup, 10).total_setup AS today,
       opt_sequence_best_of(setup, 10).total_setup  AS improved
FROM m;
```

The improved `"order"` is then in positions: `today.job` where `pos = order[k]`
is the k-th job to run.

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
| A guarantee is required | `schedule_exact` (≤ 14 jobs) or `knapsack_best_of` within its DP limits (gotcha 1); beyond them, a MIP solver |

The quadratic families (`assortment_*`, `portfolio_*`) are comfortable in the
hundreds of items, not the millions.

See also: `anofox-optimize-solve` (the `*_by` twins of these objectives),
`anofox-optimize-visualize` (plot an objectives table as a model comparison),
`anofox-optimize-formulate` (when every member scores the same, check the
family before the algorithm).

Reference docs: `docs/guides/04-validating-results.md`, `docs/theory/05-complexity-and-limits.md`,
`test/sql/pilot_scenarios.test`.
