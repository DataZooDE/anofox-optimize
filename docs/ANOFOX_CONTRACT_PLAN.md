# anofox integration contract: plan

Goal: optimize results plot in anofox-visualization with `anofox_plot(t)` and
no hand-written unnest SQL (DataZooDE/anofox-optimize#6). The contract is
anofox-visualization `docs/plans/integration-contract.md` (v1). It names
anofox-optimize as the producer of two schemas the visualization added for it:

- `schedule`: **resource, task, start, end**, model_id, due, status (`on time` / `late`).
  This is a Gantt chart (`anofox_plot_schedule`, anofox-visualization#176).
- `assignment`: one row per item, **bin | wave, item, size**, capacity, model_id.
  This is bins or waves filled with stacked items (`anofox_plot_assignment`, #177).

anofox-decide and anofox-tabfm joined the contract the same way
(anofox-decide#37, anofox-tabfm#71/#72).

## What the contract asks for

- Long tables in a known schema, detected from their column names.
- Producers tagged in `duckdb_functions()` with
  `{'anofox.output': '<schema>', 'anofox.contract': '1'}`.
- Every function of the extension tagged `anofox.family = 'optimize'`.
- `anofox_contract_version()` returning `'1'`. It is registered only if no
  other anofox extension has registered it yet, so load order does not matter.

## Producers

The solvers stay as they are: scalar functions over lists that return a
STRUCT. One family shares one signature, which is what anofox-evolve's
search relies on. The producers are table macros over the user's rows:

- `data` is a table name or a `'(SELECT …)'` string.
- The column arguments are column names as strings.
- `capacity` is a number.

This is tabfm's convention. The body runs through `query()`, and every
identifier is quoted with `replace(col, '"', '""')`.

- **The rows are sorted by item:** each group's rows are turned into the
  solver's lists in item (job) order. The solver's answer comes back per
  item, so nothing is joined back by list position.
- **`algorithms` picks the methods:** it is a list of the family's members
  (default `['best_of']`). Each member becomes `model_id`, so several
  algorithms compare in one table. An unknown name is an error that lists
  the members.
- **`group_col` splits the rows into instances:** optional, e.g. one packing
  per warehouse. The column comes back under its own name. Without it there is
  no group column.

| Producer | Schema | Columns |
|---|---|---|
| `pack_by(data, item_col, size_col, capacity [, group_col, algorithms])` | `assignment` | <group>, model_id, bin, item, size, capacity |
| `wave_by(data, item_col, size_col, priority_col, capacity [, group_col, algorithms])` | `assignment` | <group>, model_id, wave, item, size, capacity, priority |
| `schedule_by(data, job_col, processing_col, due_col, priority_col [, setup_data, group_col, algorithms])` | `schedule` | <group>, model_id, resource, task, position, start, end, setup, processing, due, priority, tardiness, status |
| `pack_objectives_by(…same arguments…)` | `summary` | <group>, model_id, metric, value, conf_low, conf_high |
| `wave_objectives_by(…)` | `summary` | same |
| `schedule_objectives_by(…)` | `summary` | same |

Every producer exists as `anofox_optimize_*` and as `opt_*`.

### Decisions

- **Bins are 1-based in the long form.** `pack_*` numbers bins from 0 and
  `wave_*` numbers waves from 1 (#7). The scalar solvers keep their bases.
  In the long form both start at 1 (`bin 1` is the first container), so the
  two `assignment` producers agree.
- **`item` / `task` are the user's ids, with their own type.** The input
  column is renamed to the contract's name; nothing is cast to text.
- **The schedule's times follow the solver's model.** A job's setup (from
  the previous job, or from the virtual start for the first job) runs before
  the job. So `start` = the previous job's `end` + `setup`, and
  `end` = `start` + `processing`. The last `end` is the solver's `makespan`.
  The sum of `priority · max(end - due, 0)` is its `total_weighted_tardiness`.
  `status` is `late` when `end > due`.
- **`resource` is the machine:** the group's value as text when `group_col`
  is given (one machine per group), else `'machine'`.
- **Setups come from a table, not a flat matrix.** `setup_data` is a table
  name or `'(SELECT …)'` string with the columns `from_id, to_id, value`.
  `from_id` NULL is the setup before the first job; a missing pair costs 0.
  The macro builds the solver's `(n+1)²` row-major matrix from it, as
  `matrix_from_triples` does for the scalar functions. A duplicate pair is an
  error. A pair whose jobs are not in a group is ignored, so one setup table
  can serve every group. Without `setup_data` every setup is 0.
- **Job ids must be unique per group** in `schedule_by`. A setup needs one
  job per id, and a Gantt needs one bar per task.
- **The objectives are the solver's own numbers.** They are not recomputed:
  - `pack`: `bins_used`;
  - `wave`: `avg_weighted_wave`, and `waves_used` (the last wave);
  - `schedule`: `total_weighted_tardiness`, `makespan`, and `late_jobs`.
- **A separate objectives producer per family.** One table cannot be both
  `assignment` and `summary`. The two macros take the same arguments.
- **Pure SQL over the existing solvers.** No new C++ algorithm, so one macro
  text builds on DuckDB 1.5.6 and 2.0, and the long form reports exactly
  what the solver decided.
- **anofox-evolve's vocabulary grows.** anofox-evolve lists every new
  catalog function in its prompt: 12 entries, and 6 of them are aliases
  shown as bare signatures. The descriptions are kept short for that reason.
  The macros are `table_macro` entries, not scalar functions, so they are not
  a new member of any solver family.

## Not in scope (PR 2)

There is no contract schema yet for the other families' decisions.
Long forms for them are a follow-up, tagged once the visualization has a
schema, or untagged if it never gets one:

- `knapsack_*` (selected items);
- `sequence_*` (an order, a route);
- `assortment_*` / `assortment_recapture_*` (listed products);
- `portfolio_*` (weights).

Their objectives (`summary`) fit today. They need matrix inputs as tables
too (substitution, covariance, setup), so they come with the long forms.

## Definition of done: anofox-visualization picks the producers up

`scripts/visualization_pickup.py`, ported from anofox-tabfm, plus
`make test_visualization` and a CI job **Visualization pickup** that deploy
`needs:`. It runs the built extension next to the released
anofox_visualization in the official DuckDB v1.5.6 CLI and checks:

1. both load orders work, and `anofox_contract_version()` exists once and
   returns `'1'`;
2. the tagged producers are exactly the expected twelve, and the
   visualization has `anofox_plot_assignment`, `anofox_plot_schedule` and
   `anofox_plot_summary`;
3. for every producer, `anofox_plot(t)` is an SVG with no `data-warnings`,
   is the same SVG as the schema's own macro, and draws every model;
4. a comparison of several algorithms plots in one table.

## Tests (red first)

`test/sql/anofox_contract.test`:

- the column names and types of every producer;
- hand-checked placements (first-fit decreasing, next fit), waves and a
  two-job schedule with setups;
- the long form against the scalar solver for every family member:
  - the same assignment and order;
  - `makespan` = the last `end`;
  - `total_weighted_tardiness` = the sum over the Gantt;
- groups, data as a subquery string, quoted identifiers, `setup_data`;
- the errors: an unknown algorithm, an empty list, a duplicate job, a
  duplicate setup pair;
- the tags: exactly the twelve producers, the family tag on every optimize
  function and on nothing of DuckDB's.

`naming.test` covers the new macros through its existing rules: an `opt_`
alias, a description, categories and examples.

## Delivery

1. PR 1 (this plan): the contract plumbing, the three long forms, the three
   objectives producers, the pickup script and the CI job.
2. PR 2: the other five families (see above).
3. A release, then the community-extensions descriptor (#8 is still open).
4. Tell the visualization side (anofox-optimize#6, anofox-visualization#171)
   the producer names, so its conformance harness gets recipes for them.
