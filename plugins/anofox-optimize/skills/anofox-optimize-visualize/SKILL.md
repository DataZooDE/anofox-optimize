---
name: anofox-optimize-visualize
description: >
  Showing anofox_optimize decisions as charts and tables: plotting the `*_by`
  and `*_objectives_by` long forms with `anofox_plot` from the
  anofox_visualization DuckDB extension (container fill per bin, pick waves,
  a Gantt chart of a schedule with late jobs, selected items, run order with
  cumulative setup, portfolio weights, model comparisons), the anofox
  integration contract schemas (assignment, schedule, selection, sequence,
  weights, summary), the `row_order` column, and discovering producers
  through `duckdb_functions()` tags and `anofox_contract_version()`. Use when
  the user wants to see, chart, present or report an optimization result, or
  to wire optimize's output into another anofox extension.
version: 2026.11.13
user-invocable: false
---

# Anofox Optimize — Visualize the Decision

**Extension:** `anofox_optimize` v2026.11.13 | **DuckDB:** v1.5.6 / v2.0.0 | **Dual naming:** `anofox_optimize_*` and `opt_*` (true aliases, same description)

Every `*_by` and `*_objectives_by` long form returns a table in one of the
anofox integration contract's schemas, which
[anofox-visualization](https://github.com/DataZooDE/anofox-visualization)'s
`anofox_plot(t)` recognizes and plots with no reshaping. Pass several
algorithms and each becomes a `model_id` to compare.

## Critical gotchas

1. **`anofox_plot` lives in another extension.** `INSTALL anofox_visualization
   FROM community; LOAD anofox_visualization;` — optimize only produces the
   tables.
2. **Plot the long forms, not the scalar structs.** A scalar result
   (`opt_pack_best_of(...)`) has no contract schema; run the matching `*_by`.
3. **The assignment plot does not separate groups.** For packings and waves
   with `group_col`, plot one group at a time (`WHERE warehouse_id = 1`). A
   schedule draws each group as its own machine lane.
4. **Keep `row_order`.** It is the last column of every producer and numbers
   the rows in the producer's own order; aggregate with
   `list(t ORDER BY t.row_order)` when passing rows on.

## Schemas

| Producer | Schema | What the plot shows |
|---|---|---|
| `opt_pack_by`, `opt_wave_by` | `assignment` | items stacked per bin / wave, per algorithm |
| `opt_schedule_by` | `schedule` | a Gantt chart per algorithm: setup then processing per job, late jobs marked |
| `opt_knapsack_by`, `opt_assortment_by`, `opt_assortment_recapture_by` | `selection` | which items were picked |
| `opt_sequence_by` | `sequence` | run order with cumulative setup |
| `opt_portfolio_by` | `weights` | the weight per asset |
| every `*_objectives_by` | `summary` | the metrics side by side per `model_id` |

Schedule times: a job's setup runs right before it, so `start` is the
previous `end` plus `setup`, and the last `end` is the makespan.

## Plotting

```sql
INSTALL anofox_visualization FROM community;
LOAD anofox_optimize; LOAD anofox_visualization;

-- containers filled carton by carton: next fit against bin completion
SELECT anofox_plot(t) FROM opt_pack_by('cartons', 'carton_id', 'size', 1000.0,
    algorithms := ['next_fit', 'bin_completion']) t;

-- a Gantt chart per algorithm, late jobs in red
SELECT anofox_plot(t) FROM opt_schedule_by('jobs', 'job', 'minutes', 'due', 'priority',
    setup_data := 'changeovers', algorithms := ['edd', 'best_of']) t;

-- the objectives as a model comparison
SELECT anofox_plot(t) FROM opt_schedule_objectives_by('jobs', 'job', 'minutes', 'due', 'priority',
    setup_data := 'changeovers', algorithms := ['edd', 'wspt', 'best_of']) t;
```

## Without the visualization extension

The long forms are ordinary tables, so a text report is plain SQL:

```sql
-- container fill per bin; load summed in item order, as the solver checks it
SELECT bin, count(*) AS cartons, list_sum(list(size ORDER BY item)) AS load,
       any_value(capacity) AS capacity,
       round(100 * list_sum(list(size ORDER BY item)) / any_value(capacity), 1) AS pct_full
FROM opt_pack_by('cartons', 'carton_id', 'size', 1000.0)
GROUP BY bin ORDER BY bin;
```

```sql
-- the schedule as a timeline with late jobs flagged
SELECT position, task, start, "end", due, status
FROM opt_schedule_by('jobs', 'job', 'minutes', 'due', 'priority', setup_data := 'changeovers')
ORDER BY row_order;
```

## Discovery

Every function optimize registers is tagged `anofox.family = 'optimize'`; the
producers also carry `anofox.contract` and `anofox.output = <schema>`.
`anofox_contract_version()` returns the contract version ('1'), registered by
whichever anofox extension loads first.

```sql
SELECT function_name, tags['anofox.output'] AS output_schema
FROM duckdb_functions()
WHERE tags['anofox.contract'] IS NOT NULL AND function_name LIKE 'opt\_%' ESCAPE '\'
ORDER BY function_name;
```

```sql
SELECT anofox_contract_version();
```

See also: `anofox-optimize-solve` (the producers and their arguments),
`anofox-optimize-evaluate` (what the objectives mean and how to check them).

Reference docs: `docs/ANOFOX_CONTRACT_PLAN.md`, README section "Plot the decisions".
