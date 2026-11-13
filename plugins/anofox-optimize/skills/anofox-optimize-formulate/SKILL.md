---
name: anofox-optimize-formulate
description: >
  Turning a business decision into the right anofox_optimize DuckDB function
  family and member: loading pallets, cartons or orders onto the fewest
  trucks / containers / bins; releasing pick waves; choosing which projects,
  shipments or items fit a budget or capacity (capital allocation); the
  production sequence on a line to cut changeover or cleaning time;
  scheduling jobs against due dates with setup times (late orders,
  tardiness); shelf assortment, range review, SKU rationalisation or
  delisting (products that cannibalise each other versus delisted demand
  that moves to the products that stay); picking assets with a holding
  limit. Use when a user describes such a decision in business terms, asks
  which approach or family fits, or must choose between assortment and
  assortment_recapture or between sequence and schedule.
version: 2026.11.13
user-invocable: false
---

# Anofox Optimize — Formulate the Decision

**Extension:** `anofox_optimize` v2026.11.13 | **DuckDB:** v1.5.6 / v2.0.0 | **Dual naming:** `anofox_optimize_*` and `opt_*` (true aliases, same description)

From "here is my problem" to "this family, this member". Families are named
after the *shape* of the decision, not the industry: a hospital allocating
theatre slots and a factory sequencing a press line are the same problem.

## Critical gotchas

1. **Assortment has two families with identical signatures and opposite
   economics.** `assortment_*` scores cannibalisation among the *listed*
   products; `assortment_recapture_*` scores demand handed over by the
   *delisted* ones. Nothing in the data says which one is meant. Optimizing
   one while being judged on the other is worse than not optimizing: a live
   search did exactly that in 6 of 6 runs and scored 1433 where a trivial
   top-margin ranking scored 1741 and the optimum was 3888. **Ask the user:
   when a product is dropped, does its demand disappear, or move to the
   products that stay?** Moves → `assortment_recapture_*`.
2. **Sequencing is not scheduling.** `sequence_*` minimizes total changeover
   and knows nothing about due dates. `schedule_*` minimizes
   priority-weighted tardiness with sequence-dependent setups. With due dates,
   `sequence_*` returns an efficient order that misses them.
3. **`wave_fewest_waves` optimizes the wrong objective on purpose** (number of
   waves, not the priority-weighted mean wave). It is there to show what that
   costs; do not recommend it as the answer.
4. **Everything is placed in packing, not in knapsack.** `pack_*` must put
   every item in some bin; `knapsack_*` may leave items out.
5. **Default to `best_of` for a heuristic answer.** It runs the family's
   members and returns the winner, so it cannot lose to any member it runs.
   **Scheduling's `best_of` does not run `schedule_exact`** (so it stays usable
   at any size) and can lose to it: on a 5-job instance `best_of` pays 55
   where the optimum is 47. With ≤ 14 jobs, run `schedule_exact` as well.
   Reach for another named member only for a reason from "Choosing the member".
6. **Only two answers are proven optima, and only within limits.**
   `schedule_exact` (≤ 14 jobs, refuses beyond) and `knapsack_best_of` /
   `knapsack_exact` (an alias), the knapsack one only when its dynamic
   program ran: weights in multiples of 0.0001, at most 50 million cells and
   within `anofox_optimize_knapsack_max_memory`. Past those limits it
   **silently** returns its best greedy answer. Everything else is a
   heuristic with no guarantee. Say so when presenting a result; the limits
   in full are in `anofox-optimize-evaluate`.

## Decision shape → family

| The user says… | Shape | Family |
|---|---|---|
| "fit these cartons into as few containers / trucks / pallets as possible" | partition into groups under a capacity | `pack_*` |
| "release orders in pick waves; urgent ones early; each wave has a capacity" | partition into *ordered* groups, earlier is better | `wave_*` |
| "which shipments go on the cheap carrier", "which projects fit the budget" | subset under one capacity | `knapsack_*` |
| "in what order do we run the jobs to cut changeovers / cleaning" | permutation, cost between neighbours | `sequence_*` |
| "order the jobs so late orders are minimized, setups depend on what ran before" | permutation, due dates and setups | `schedule_*` |
| "which products to keep on a k-slot shelf; similar products steal from each other" | subset of size ≤ k, items interact (cannibalisation) | `assortment_*` |
| "we must delist down to k products; where does the delisted demand go" | subset of size ≤ k, items interact (recapture) | `assortment_recapture_*` |
| "pick at most k assets and how much of each, best risk-adjusted return" | subset of size ≤ k plus continuous weights | `portfolio_*` |

Not covered: multiple machines with routing, vehicle routing with time windows,
general MIP. If the honest answer needs a guarantee outside the limits above,
say that a dedicated MIP solver is the right tool.

## Recipes

Each recipe shows the scalar call on literal arrays so the shape is visible;
`anofox-optimize-solve` runs the same decision over a table.

### Container / truck / bin loading — `pack_*`

```text
opt_pack_<member>(items DOUBLE[], capacity DOUBLE)
  -> STRUCT(bins_used BIGINT, assignment BIGINT[])   -- assignment[i] = 1-based bin of item i
```

Members: `first_fit_decreasing`, `best_fit_decreasing`, `worst_fit_decreasing`,
`next_fit` (deliberately weak), `bin_completion`, `local_search`, `best_of`.

```sql
SELECT opt_pack_best_of([448.0, 460.0, 450.0, 337.0, 340.0, 403.0], 1000.0) AS r;
```

Start with `best_of`. When items combine into exact fills (sizes that sum to
the capacity in threes), the fit rules do badly and `pack_bin_completion` can
beat them (11 bins against their 12 on the repository's triplet instance), but
it is not optimal either: the optimum there is 10. It searches pairs of
companions per bin, then tops up greedily.

### Pick-wave release — `wave_*`

```text
opt_wave_<member>(items DOUBLE[], priorities DOUBLE[], capacity DOUBLE)
  -> STRUCT(wave BIGINT[], avg_weighted_wave DOUBLE)  -- wave[i] = 1-based wave of order i
```

`items` is each order's work (lines, minutes), `priorities` its urgency.
Objective: the priority-weighted mean wave (urgent orders early). Members:
`as_given` (the order the list is given in), `priority_first`, `priority_density`, `fewest_waves`
(wrong objective, on purpose), `best_of`.

```sql
SELECT opt_wave_best_of([12.0, 30.0, 8.0, 25.0, 5.0], [5.0, 1.0, 3.0, 2.0, 4.0], 40.0) AS r;
```

### Budget / capacity selection — `knapsack_*`

```text
opt_knapsack_<member>(values DOUBLE[], weights DOUBLE[], capacity DOUBLE)
  -> STRUCT(selected BOOLEAN[], total_value DOUBLE, total_weight DOUBLE)
```

Members: `greedy_ratio`, `greedy_value` (deliberately weak), `best_of`, and
`exact`, which is an **alias of `best_of`**. When value equals weight (savings
proportional to load) the problem is subset-sum: pass the weights twice.

```sql
SELECT opt_knapsack_best_of([600.0, 520.0, 310.0, 300.0], [550.0, 450.0, 300.0, 250.0], 1000.0) AS r;
```

### Changeover order on one line — `sequence_*`

```text
opt_sequence_<member>(setup_matrix DOUBLE[], n_jobs BIGINT)
  -> STRUCT("order" BIGINT[], total_setup DOUBLE)     -- n×n row-major, setup[i*n+j] = i then j
```

Members: `as_given` (the order the list is given in), `nearest_neighbour`,
`two_opt`, `best_of`. There is no exact sequencing member.

```sql
SELECT opt_sequence_best_of([0.0, 4.0, 1.0,  2.0, 0.0, 5.0,  3.0, 1.0, 0.0], 3) AS r;
```

### Due-date scheduling with setups — `schedule_*`

```text
opt_schedule_<member>(processing_times DOUBLE[], due_dates DOUBLE[], priorities DOUBLE[],
                      setup_matrix DOUBLE[], n_jobs BIGINT)
  -> STRUCT("order" BIGINT[], total_weighted_tardiness DOUBLE, makespan DOUBLE)
```

The setup matrix is **(n+1)×(n+1)**: index 0 is a virtual start, so row 0 is
the setup before the first job (see `anofox-optimize-data-prep`). Members:
`edd` (textbook, ignores setups and priorities), `wspt`, `atcs`,
`local_search`, `exact` (≤ 14 jobs), `best_of`.

```sql
SELECT opt_schedule_best_of([4.0, 6.0], [5.0, 8.0], [1.0, 2.0],
                            [0.0, 1.0, 2.0,  0.0, 0.0, 3.0,  0.0, 1.0, 0.0], 2) AS r;
```

### Shelf assortment — `assortment_*` or `assortment_recapture_*`

```text
opt_assortment[_recapture]_<member>(margins DOUBLE[], base_demands DOUBLE[],
                                    substitution_matrix DOUBLE[], max_listed BIGINT)
  -> STRUCT(listed BOOLEAN[], captured_margin DOUBLE)
```

`substitution_matrix[j*n+i]` (0-based j, i) in [0,1] is the share of j's demand
that moves to i. Members (both families): `top_margin` (naive),
`greedy_marginal`, `local_search`, `best_of`.

| | `assortment_*` | `assortment_recapture_*` |
|---|---|---|
| Question | what to stock when products steal from each other | which survivors soak up demand when we drop products |
| A listed product earns | margin × (demand − what other *listed* products take) | margin × demand in full, plus demand from *delisted* products at its own margin |
| Listing a near-duplicate | destroys value | is neutral |
| High-margin, low-demand item | rarely worth a slot | can be worth a slot as a recapture sink |

The same shelf under the two models picks different products. On the
`products` table below, product C (margin 8, demand 20) is listed only under
recapture, as the sink for the demand of the products that are dropped:

```sql
SELECT 'cannibalisation' AS model, list(item ORDER BY item) FILTER (listed) AS shelf
FROM opt_assortment_by('products', 'sku', 'margin', 'demand', 'substitution', 3)
UNION ALL
SELECT 'recapture', list(item ORDER BY item) FILTER (listed)
FROM opt_assortment_recapture_by('products', 'sku', 'margin', 'demand', 'substitution', 3);
-- cannibalisation [A, D, E]; recapture [A, C, F]
```

```sql
SELECT opt_assortment_recapture_best_of([5.0, 3.0, 8.0], [100.0, 120.0, 20.0],
                                        [0.0, 0.5, 0.3,  0.4, 0.0, 0.2,  0.1, 0.1, 0.0], 2) AS r;
```

### Asset selection with a holding limit — `portfolio_*`

```text
opt_portfolio_<member>(expected_returns DOUBLE[], covariance_matrix DOUBLE[],
                       max_holdings BIGINT, max_position_size DOUBLE)   -- cap in (0, 1]
  -> STRUCT(weights DOUBLE[], expected_return DOUBLE, volatility DOUBLE, sharpe DOUBLE)
```

Maximizes Sharpe with weights summing to 1 and none above the cap. Members:
`top_return` (top-k returns, equal weights, ignores covariance — naive),
`top_return_optimized` (same assets, optimized weights), `greedy_sharpe`,
`best_of`.

```sql
SELECT opt_portfolio_best_of([0.10, 0.08, 0.12], [0.04, 0.006, 0.01,  0.006, 0.03, 0.004,  0.01, 0.004, 0.06], 2, 0.7) AS r;
```

## Choosing the member

| You need | Use |
|---|---|
| The best heuristic answer | `*_best_of` (scheduling with ≤ 14 jobs: also `schedule_exact`) |
| A proven optimum on a small instance | `schedule_exact` (≤ 14 jobs); `knapsack_best_of` where its DP runs |
| An honest baseline to show what optimizing buys | `pack_next_fit`, `knapsack_greedy_value`, `wave_as_given`, `sequence_as_given`, `schedule_edd`, `assortment_top_margin` or `assortment_recapture_top_margin` (the same family as the solve), `portfolio_top_return` |
| To reproduce today's policy | `sequence_as_given`, `wave_as_given` on lists in **today's** order. In a `*_by` long form, `as_given` means item-id order, which is today's order only if the ids sort that way (see `anofox-optimize-evaluate`) |
| Speed on a very large input | a single greedy member (`pack_first_fit_decreasing`, `knapsack_greedy_ratio`) |
| To separate "wrong assets" from "wrong weights" | `portfolio_top_return` vs `portfolio_top_return_optimized` |

## Questions to ask before solving

- Must every item be placed (pack, wave), or may some be left out (knapsack)?
- Is there one capacity per bin/wave, the same for all? (Yes is the only case supported.)
- Do due dates matter (schedule), or only changeover cost (sequence)?
- When a product is delisted, does its demand vanish or move (assortment vs recapture)?
- How large is the instance? `schedule_exact` refuses above 14 jobs; the
  assortment and portfolio families are comfortable in the hundreds, not millions.

See also: `anofox-optimize-data-prep` (turn tables into these arrays and
matrices), `anofox-optimize-solve` (run a family over a table, per group),
`anofox-optimize-evaluate` (is the answer feasible, and how good is it),
`anofox-optimize-visualize` (plot the decision).

Reference docs: `docs/guides/02-choosing-a-family.md`, `docs/theory/02-packing-and-knapsack.md`,
`docs/theory/03-sequencing-and-scheduling.md`, `docs/theory/04-selection.md`, `docs/API_REFERENCE.md`.
