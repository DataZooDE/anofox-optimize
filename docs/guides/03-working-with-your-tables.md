# Working With Your Tables

> Getting data in and answers out, without silently scrambling the order.

**Use this guide to:**
- Aggregate rows into the list arguments these functions take
- Build matrices from `(from, to, value)` triples
- Join results back onto your rows
- Run an optimization per group

These functions are scalar functions over lists. Everything here is about the
boundary between your rows and those lists — which is where the mistakes are.

## The ordering rule

**Aggregate with an explicit `ORDER BY`, and read results back with the same
order.** The functions return positional results; they have no idea what your
primary key is.

```sql
-- GOOD: one explicit ordering used on the way in and on the way out
WITH ordered AS (
    SELECT item_id, size, row_number() OVER (ORDER BY item_id) AS pos
    FROM items
),
packed AS (
    SELECT opt_pack_best_of(list(size ORDER BY pos), 1000.0).assignment AS bins
    FROM ordered
)
SELECT o.item_id, p.bins[o.pos] + 1 AS bin_number
FROM ordered o CROSS JOIN packed p;
```

```sql
-- BAD: row_number() with no ORDER BY is not stable, and list() with no
-- ORDER BY need not match it. This can silently assign the wrong bins.
SELECT opt_pack_best_of(list(size), 1000.0) FROM items;
```

Remember: **DuckDB lists are 1-indexed; returned assignments and orders are
0-based.** Add 1 when presenting them, and index with the 1-based position.

## Building a matrix from triples

Four families take a matrix flattened row-major: `sequence_*`, `schedule_*`,
`assortment_*`, `portfolio_*`. Your tables almost certainly store it as
triples instead.

```sql
-- setup_costs(from_job, to_job, cost), job ids 1..n
SELECT opt_matrix_from_triples(
         list(from_job), list(to_job), list(cost),
         (SELECT count(*) FROM jobs),   -- n
         1                              -- ids are 1-based
       ) AS setup_matrix
FROM setup_costs;
```

- Cells you do not supply are **0**.
- Supplying the same cell twice **raises** rather than keeping whichever
  arrived last — silent last-write-wins is how a matrix quietly stops
  matching the table it came from.
- An out-of-range id raises an error naming `n` and `index_base`, so you can
  see which of the two is wrong.

### The scheduling matrix is (n+1) × (n+1)

`schedule_*` needs a **virtual start** at index 0: `setup[i*(n+1)+j]` is the
cost of running job `j-1` after job `i-1`, and row 0 is the cost of running a
job first. Build it with `n+1` and shift your ids up by one:

```sql
SELECT opt_matrix_from_triples(
         list(from_job),      -- 0 means "from the virtual start"
         list(to_job),
         list(cost),
         (SELECT count(*) FROM jobs) + 1,
         0
       ) AS setup_matrix
FROM setup_costs;
```

## Optimizing per group

Because these are scalar functions over aggregates, "one optimization per
warehouse" is a plain `GROUP BY`:

```sql
WITH ordered AS (
    SELECT warehouse_id, item_id, size,
           row_number() OVER (PARTITION BY warehouse_id ORDER BY item_id) AS pos
    FROM items
),
packed AS (
    SELECT warehouse_id,
           opt_pack_best_of(list(size ORDER BY pos), 1000.0) AS r
    FROM ordered
    GROUP BY warehouse_id
)
SELECT o.warehouse_id, o.item_id, p.r.assignment[o.pos] + 1 AS bin_number
FROM ordered o
JOIN packed p USING (warehouse_id);
```

Each group is solved independently, and DuckDB parallelises the groups.

## Unpacking a struct result

Every function returns a struct. Pull fields with dot notation, and unnest
list fields when you want rows:

```sql
WITH r AS (SELECT opt_knapsack_best_of(list(value), list(weight), 1000.0) AS k FROM shipments)
SELECT k.total_value, k.total_weight FROM r;

-- as rows, aligned with the input order
WITH ordered AS (
    SELECT shipment_id, value, weight,
           row_number() OVER (ORDER BY shipment_id) AS pos
    FROM shipments
),
r AS (
    SELECT opt_knapsack_best_of(list(value ORDER BY pos), list(weight ORDER BY pos), 1000.0).selected AS sel
    FROM ordered
)
SELECT o.shipment_id, r.sel[o.pos] AS take_it
FROM ordered o CROSS JOIN r;
```

Field names that collide with SQL keywords are quoted: `schedule_*` and
`sequence_*` return `"order"`.

```sql
SELECT opt_schedule_best_of(p, d, w, s, n)."order" FROM jobs_input;
```

## Types

Inputs are `DOUBLE[]`; cast integer columns on the way in:

```sql
list(CAST(size AS DOUBLE) ORDER BY pos)
```

Counts and cardinality limits are `BIGINT`. Passing an integer column
directly is fine.
