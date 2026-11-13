---
name: anofox-optimize-data-prep
description: >
  Getting tables into the shape the anofox_optimize DuckDB solvers take:
  aggregating rows into ordered DOUBLE[] lists, mapping 1-based results back
  to rows, building row-major matrices from (from, to, value) triples with
  `opt_matrix_from_triples`, the (n+1)×(n+1) scheduling setup matrix with its
  virtual start, substitution and covariance matrices, pairs tables for the
  `*_by` long forms, per-group (per warehouse / per machine) instances, types
  and the inputs that are rejected. Use when preparing changeover, setup,
  substitution or covariance data, when results come back attached to the
  wrong rows, or when a solver raises about matrix size, ids or values.
version: 2026.11.13
user-invocable: false
---

# Anofox Optimize — Data Prep

**Extension:** `anofox_optimize` v2026.11.13 | **DuckDB:** v1.5.6 / v2.0.0 | **Dual naming:** `anofox_optimize_*` and `opt_*` (true aliases, same description)

The solvers are scalar functions over lists: they know positions, not your
keys. Every mistake worth worrying about happens at the boundary between rows
and lists. The `*_by` long forms (`anofox-optimize-solve`) handle this
boundary for you; use this skill when calling the scalar solvers directly or
when preparing the pairs tables the long forms take.

## Critical gotchas

1. **Aggregate with an explicit `ORDER BY`, and read back with the same
   order.** `list(size)` without `ORDER BY` has no defined order, and a
   `row_number()` without `ORDER BY` need not match it: results land on the
   wrong rows, silently.
2. **Everything is 1-based BIGINT.** Bins, waves, orders and positions start
   at 1, like SQL lists: `items[r."order"[k]]` is the k-th item to run, and
   `r.assignment[pos]` is the bin of the item at `pos`. (Before v2026.10.26
   they were 0-based; old snippets may subtract 1.)
3. **The scheduling matrix is (n+1)×(n+1), the others n×n.** `schedule_*`
   has a virtual start at index 0: `setup[i*(n+1)+j]` (0-based i, j) is the
   setup of job j after job i, both shifted up by one, and row 0 is the setup
   before the first job. An n×n matrix raises ("were required").
4. **Missing matrix cells are 0; duplicates raise.** `opt_matrix_from_triples`
   never keeps "the last one". An out-of-range id raises naming `n` and
   `index_base`.
5. **Rejected inputs raise rather than being clamped:** negative or
   non-finite sizes, substitution rates outside [0,1], `max_position_size`
   outside (0,1], duplicate matrix cells. Each once produced a confidently
   wrong answer.
6. **`"order"` is a keyword.** Quote it: `opt_schedule_best_of(...)."order"`.

## The ordering rule

```sql
-- one explicit ordering, used on the way in and on the way out
WITH ordered AS (
    SELECT item_id, size, row_number() OVER (ORDER BY item_id) AS pos
    FROM items
),
packed AS (
    SELECT opt_pack_best_of(list(size ORDER BY pos), 1000.0).assignment AS bins
    FROM ordered
)
SELECT o.item_id, o.size, p.bins[o.pos] AS bin
FROM ordered o CROSS JOIN packed p
ORDER BY bin, o.item_id;
```

## Per-group instances

One optimization per warehouse, machine or store is a `GROUP BY`; partition
the position the same way:

```sql
WITH ordered AS (
    SELECT warehouse_id, item_id, size,
           row_number() OVER (PARTITION BY warehouse_id ORDER BY item_id) AS pos
    FROM items
),
packed AS (
    SELECT warehouse_id, opt_pack_best_of(list(size ORDER BY pos), 1000.0) AS r
    FROM ordered
    GROUP BY warehouse_id
)
SELECT o.warehouse_id, o.item_id, p.r.assignment[o.pos] AS bin
FROM ordered o JOIN packed p USING (warehouse_id)
ORDER BY o.warehouse_id, bin, o.item_id;
```

## Matrices from triples — `opt_matrix_from_triples`

```text
opt_matrix_from_triples(from BIGINT[], to BIGINT[], value DOUBLE[], n BIGINT, index_base BIGINT)
  -> DOUBLE[]   -- n*n cells, row-major; index_base is subtracted from the ids
```

Changeovers between jobs with ids 1..n (for `sequence_*`):

```sql
-- setup_costs(from_job, to_job, cost), job ids 1..10
SELECT opt_sequence_best_of(
         opt_matrix_from_triples(list(from_job), list(to_job), list(cost),
                                 (SELECT count(*) FROM jobs), 1),
         (SELECT count(*) FROM jobs)) AS r
FROM setup_costs;
```

### The scheduling matrix with its virtual start

Size `n+1` and `index_base` 0, so job ids 1..n land at indices 1..n and
id 0 is "before the first job":

```sql
-- schedule_setups(from_job, to_job, cost): from_job 0 = before the first job
WITH m AS (
    SELECT opt_matrix_from_triples(list(from_job), list(to_job), list(cost),
                                   (SELECT count(*) FROM jobs) + 1, 0) AS setup
    FROM schedule_setups
),
j AS (
    SELECT list(minutes ORDER BY job) AS p, list(due ORDER BY job) AS d,
           list(priority ORDER BY job) AS w, count(*) AS n
    FROM jobs
)
SELECT opt_schedule_best_of(j.p, j.d, j.w, m.setup, j.n) AS r FROM j, m;
```

The job ids must be exactly 1..n here: the matrix is indexed by id, the
lists by `ORDER BY job` position. With other ids, number them first with
`row_number() OVER (ORDER BY job)` and join the triples through that number.

### Substitution (assortment) and covariance (portfolio)

- **Substitution:** `substitution_matrix[j*n+i]` (0-based) is the share of
  product j's demand that moves to product i, in [0,1]. Build it with
  `from` = the product losing demand, `to` = the product gaining it.
- **Covariance:** n×n, symmetric. `opt_matrix_from_triples` does not mirror
  cells: supply both (i, j) and (j, i), and the diagonal (the variances).

## Pairs tables for the `*_by` long forms

The long forms take a matrix as a **pairs table** (a table name or a
`'(SELECT …)'` string) with the columns `from_id, to_id, value`, keyed by your
own item ids, not positions:

| Argument | Used by | Notes |
|---|---|---|
| `setup_data` | `schedule_by`, `sequence_by` | `from_id NULL` in `schedule_by`: the setup before the first job |
| `substitution_data` | `assortment_by`, `assortment_recapture_by` | from = loses demand, to = gains it |
| `covariance_data` | `portfolio_by` | a pair given once counts both ways; given both ways the two values must agree |

A missing pair is 0 and a duplicate pair raises. Item ids must be unique within
a group for `schedule_by`, `sequence_by`, `knapsack_by`, `assortment_by`,
`assortment_recapture_by` and `portfolio_by`.

```sql
-- a setup pairs table from a changeover log, with the first-job setups as from_id NULL
CREATE TABLE my_changeovers AS
SELECT CASE WHEN from_job = 0 THEN NULL ELSE from_job END AS from_id,
       to_job AS to_id, cost AS value
FROM schedule_setups;
SELECT count(*) FILTER (WHERE from_id IS NULL) AS first_job_setups FROM my_changeovers;
```

## Types

Solver inputs are `DOUBLE[]`; counts and limits are `BIGINT`. Cast integer
columns on the way in: `list(CAST(size AS DOUBLE) ORDER BY pos)`. An integer
literal for a count is fine.

"Fits" means the sizes of a bin, wave or knapsack selection, **summed in item
order** in doubles, are within capacity, so `0.1 + 0.2` does not fit `0.3`.
Decimal sizes are fine; know that the last digit counts.

See also: `anofox-optimize-formulate` (which family), `anofox-optimize-solve`
(the long forms do this mapping for you), `anofox-optimize-evaluate`
(feasibility checks need the same ordering).

Reference docs: `docs/guides/03-working-with-your-tables.md`, `docs/theory/05-complexity-and-limits.md`.
