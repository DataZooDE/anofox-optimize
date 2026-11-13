---
name: anofox-optimize-solve
description: >
  Running anofox_optimize solvers over tables in DuckDB: the `*_by` table
  macros (`opt_pack_by`, `opt_wave_by`, `opt_knapsack_by`, `opt_sequence_by`,
  `opt_schedule_by`, `opt_assortment_by`, `opt_assortment_recapture_by`,
  `opt_portfolio_by`) that take a table and column names and return one row
  per item or job, per group (warehouse, machine, store) and per algorithm;
  the scalar solvers for inline SQL; choosing `best_of` or named members via
  `algorithms :=`; and joining the decision back to the source rows. Use when
  the user wants a packing, wave plan, selection, run order, schedule,
  assortment or portfolio computed from a table, for many groups at once, or
  for several algorithms side by side.
version: 2026.11.7
user-invocable: false
---

# Anofox Optimize — Solve Over Your Tables

**Extension:** `anofox_optimize` v2026.11.7 | **DuckDB:** v1.5.6 / v2.0.0 | **Dual naming:** `anofox_optimize_*` and `opt_*` (true aliases, same description)

Two ways to run a family:

- **Long form, `opt_<family>_by(...)`** — a table macro over your rows. It
  builds the ordered lists and matrices itself, solves every group with every
  algorithm you name, and returns one row per item. Prefer it.
- **Scalar, `opt_<family>_<member>(lists...)`** — for inline SQL over lists
  you built yourself (`anofox-optimize-data-prep`), and the form
  anofox-evolve searches over: every member of a family shares one signature,
  so swapping the name swaps the algorithm and nothing else.

## Critical gotchas

1. **`data` is a string**: a table name (`'cartons'`) or a parenthesized
   query (`'(SELECT * FROM cartons WHERE site = ''w1'')'`). Column arguments
   are column *names* as strings, not identifiers.
2. **`algorithms` takes member names without the family prefix**:
   `algorithms := ['next_fit', 'best_of']`, default `['best_of']`. A name not in
   the family raises. Each algorithm becomes a `model_id` in the output.
3. **`group_col := 'site'` splits the rows into independent instances** (one
   packing per warehouse, one machine per group in a schedule). Without it the
   whole table is one instance.
4. **Ids must be unique per group** for `schedule_by`, `sequence_by`,
   `knapsack_by`, `assortment_by`, `assortment_recapture_by`, `portfolio_by`;
   pack and wave allow repeats. Items are taken in id order.
5. **`knapsack_exact` is an alias of `knapsack_best_of`** (since v2026.11.7).
   `algorithms := ['exact']` still works and returns what `'best_of'` does.
6. **The last column is always `row_order`** (1..n in the producer's own
   order). Sort by it to keep the producer's order: `ORDER BY row_order`.
7. **Do not mix up the assortment families** — see `anofox-optimize-formulate`.

## The long forms

| Long form | Required arguments (then `group_col :=`, `algorithms :=`) | One row per… with columns |
|---|---|---|
| `opt_pack_by` | `data, item_col, size_col, capacity` | item: `model_id, bin, item, size, capacity` |
| `opt_wave_by` | `data, item_col, size_col, priority_col, capacity` | order: `model_id, wave, item, size, capacity, priority` |
| `opt_knapsack_by` | `data, item_col, value_col, weight_col, capacity` | item: `model_id, item, value, weight, selected, capacity` |
| `opt_sequence_by` | `data, item_col, setup_data` | item in run order: `model_id, position, item, setup, cumulative_setup` |
| `opt_schedule_by` | `data, job_col, processing_col, due_col, priority_col [, setup_data :=]` | job: `model_id, resource, task, position, start, end, setup, processing, due, priority, tardiness, status` |
| `opt_assortment_by`, `opt_assortment_recapture_by` | `data, item_col, margin_col, demand_col, substitution_data, shelf` | product: `model_id, item, margin, demand, listed` |
| `opt_portfolio_by` | `data, item_col, return_col, covariance_data, max_holdings, weight_cap` | asset: `model_id, item, expected_return, weight` |

With `group_col`, the group column comes first. `item` / `task` keep your id
type. Every long form has an `*_objectives_by` twin with the same arguments
(see `anofox-optimize-evaluate`).

### Packing per warehouse, two algorithms

```sql
SELECT warehouse_id, model_id, bin, item, size
FROM opt_pack_by('items', 'item_id', 'size', 1000.0,
                 group_col := 'warehouse_id',
                 algorithms := ['next_fit', 'best_of'])
ORDER BY row_order;
```

### Pick waves

```sql
SELECT wave, item, size, priority
FROM opt_wave_by('orders', 'order_id', 'lines', 'priority', 40.0)
ORDER BY row_order;
```

### Which shipments go on the capacity-limited carrier

```sql
SELECT item, weight, selected
FROM opt_knapsack_by('shipments', 'shipment_id', 'value', 'weight', 1000.0)
ORDER BY row_order;
```

### Run order on one line (changeovers only)

```sql
SELECT position, item, setup, cumulative_setup
FROM opt_sequence_by('jobs', 'job', 'setup_costs_pairs')
ORDER BY row_order;
```

### Schedule against due dates, with setups

`setup_data` pairs with `from_id NULL` are the setup before the first job.
Each group is one machine (`resource`).

```sql
SELECT model_id, position, task, start, "end", tardiness, status
FROM opt_schedule_by('jobs', 'job', 'minutes', 'due', 'priority',
                     setup_data := 'changeovers', algorithms := ['edd', 'best_of'])
ORDER BY row_order;
```

### Shelf assortment (recapture model)

```sql
SELECT item, margin, demand, listed
FROM opt_assortment_recapture_by('products', 'sku', 'margin', 'demand', 'substitution', 3)
ORDER BY row_order;
```

### Portfolio, at most 3 holdings, no position above 50%

```sql
SELECT item, expected_return, round(weight, 4) AS weight
FROM opt_portfolio_by('assets', 'ticker', 'expected_return', 'covariance', 3, 0.5)
ORDER BY row_order;
```

### Filter the input with a query string

```sql
SELECT count(DISTINCT bin) AS bins
FROM opt_pack_by('(SELECT * FROM items WHERE warehouse_id = 1)', 'item_id', 'size', 1000.0);
```

## Joining the decision back to your rows

The long forms return your ids, so join on them:

```sql
SELECT s.*, k.selected
FROM shipments s
JOIN opt_knapsack_by('shipments', 'shipment_id', 'value', 'weight', 1000.0) k
  ON k.item = s.shipment_id
ORDER BY s.shipment_id;
```

## Scalar solvers inline

Every scalar returns a STRUCT; take fields with dot notation. `best_of` is the
default; any member name of the family fits the same call.

```sql
SELECT r.bins_used, r.assignment
FROM (SELECT opt_pack_best_of(list(size ORDER BY carton_id), 1000.0) AS r FROM cartons);
```

```sql
-- the same instance, every packing member side by side
SELECT opt_pack_next_fit(s, 1000.0).bins_used             AS next_fit,
       opt_pack_first_fit_decreasing(s, 1000.0).bins_used AS ffd,
       opt_pack_bin_completion(s, 1000.0).bins_used       AS completion,
       opt_pack_best_of(s, 1000.0).bins_used              AS best
FROM (SELECT list(size ORDER BY item_id) AS s FROM items);
```

| Family | Scalar members | Returns |
|---|---|---|
| pack | `first_fit_decreasing`, `best_fit_decreasing`, `worst_fit_decreasing`, `next_fit`, `bin_completion`, `local_search`, `best_of` | `bins_used, assignment` |
| wave | `as_given`, `priority_first`, `priority_density`, `fewest_waves`, `best_of` | `wave, avg_weighted_wave` |
| knapsack | `greedy_ratio`, `greedy_value`, `best_of`, `exact` (alias of `best_of`) | `selected, total_value, total_weight` |
| sequence | `as_given`, `nearest_neighbour`, `two_opt`, `best_of` | `"order", total_setup` |
| schedule | `edd`, `wspt`, `atcs`, `local_search`, `exact`, `best_of` | `"order", total_weighted_tardiness, makespan` |
| assortment, assortment_recapture | `top_margin`, `greedy_marginal`, `local_search`, `best_of` | `listed, captured_margin` |
| portfolio | `top_return`, `top_return_optimized`, `greedy_sharpe`, `best_of` | `weights, expected_return, volatility, sharpe` |

Full names, for lookup: `opt_pack_first_fit_decreasing`, `opt_pack_best_fit_decreasing`,
`opt_pack_worst_fit_decreasing`, `opt_pack_next_fit`, `opt_pack_bin_completion`,
`opt_pack_local_search`, `opt_pack_best_of`; `opt_wave_as_given`, `opt_wave_priority_first`,
`opt_wave_priority_density`, `opt_wave_fewest_waves`, `opt_wave_best_of`;
`opt_knapsack_greedy_ratio`, `opt_knapsack_greedy_value`, `opt_knapsack_best_of`,
`opt_knapsack_exact`; `opt_sequence_as_given`, `opt_sequence_nearest_neighbour`,
`opt_sequence_two_opt`, `opt_sequence_best_of`; `opt_schedule_edd`, `opt_schedule_wspt`,
`opt_schedule_atcs`, `opt_schedule_local_search`, `opt_schedule_exact`, `opt_schedule_best_of`;
`opt_assortment_top_margin`, `opt_assortment_greedy_marginal`, `opt_assortment_local_search`,
`opt_assortment_best_of`; `opt_assortment_recapture_top_margin`,
`opt_assortment_recapture_greedy_marginal`, `opt_assortment_recapture_local_search`,
`opt_assortment_recapture_best_of`; `opt_portfolio_top_return`,
`opt_portfolio_top_return_optimized`, `opt_portfolio_greedy_sharpe`, `opt_portfolio_best_of`;
`opt_matrix_from_triples`; `opt_version`.

## Canonical pipeline

```sql
-- 1. one packing per warehouse, by every algorithm worth comparing
CREATE OR REPLACE TABLE plan AS
SELECT * FROM opt_pack_by('items', 'item_id', 'size', 1000.0,
                          group_col := 'warehouse_id',
                          algorithms := ['first_fit_decreasing', 'bin_completion', 'best_of']);

-- 2. how many bins each algorithm needs per warehouse
SELECT warehouse_id, model_id, max(bin) AS bins
FROM plan GROUP BY ALL ORDER BY warehouse_id, bins;

-- 3. keep best_of's answer and attach it to the source rows
SELECT i.*, p.bin
FROM items i
JOIN plan p ON p.item = i.item_id AND p.warehouse_id = i.warehouse_id
WHERE p.model_id = 'best_of'
ORDER BY i.warehouse_id, p.bin;
```

See also: `anofox-optimize-formulate` (which family), `anofox-optimize-data-prep`
(lists and matrices for the scalar form), `anofox-optimize-evaluate` (objectives,
feasibility, gap), `anofox-optimize-visualize` (plot these tables).

Reference docs: `docs/guides/01-getting-started.md`, `docs/ANOFOX_CONTRACT_PLAN.md`, `docs/API_REFERENCE.md`.
