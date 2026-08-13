# Getting Started

> Combinatorial decision-making inside DuckDB — packing, selection, sequencing and allocation, as SQL functions.

**Use this guide to:**
- Install and load the extension
- Run your first optimization and read the result back
- Map results onto your own rows (the part that trips people up)
- Know which function to reach for

## Installation

```sql
INSTALL anofox_optimize FROM community;
LOAD anofox_optimize;

SELECT anofox_optimize_version();
```

Every function has a short `opt_` alias. `anofox_optimize_pack_best_of` and
`opt_pack_best_of` are the same function; the short form is used throughout
these guides.

## Your first optimization

You have cartons to load into containers of fixed capacity, and you want to
use as few containers as possible.

```sql
CREATE TABLE cartons AS
SELECT * FROM (VALUES (1, 448.0), (2, 460.0), (3, 450.0),
                      (4, 337.0), (5, 340.0), (6, 403.0)) AS t(carton_id, size);

SELECT opt_pack_best_of(list(size ORDER BY carton_id), 1000.0) AS result
FROM cartons;
```

```
{'bins_used': 3, 'assignment': [1, 0, 0, 2, 2, 1]}
```

Three containers — and `ceil(sum(size)/1000)` is also 3, so this is provably
optimal. `assignment[i]` is the 0-based container the *i*-th item went into,
in the same order you passed the sizes.

## Mapping the answer back to your rows

The functions take and return **lists**, so the one thing you must control is
the order. Aggregate with an explicit `ORDER BY`, and use the same ordering
when you join the answer back:

```sql
WITH ordered AS (
    SELECT carton_id, size, row_number() OVER (ORDER BY carton_id) AS pos
    FROM cartons
),
packed AS (
    SELECT opt_pack_best_of(list(size ORDER BY pos), 1000.0).assignment AS bins
    FROM ordered
)
SELECT o.carton_id, o.size, p.bins[o.pos] + 1 AS container
FROM ordered o CROSS JOIN packed p
ORDER BY container, o.carton_id;
```

Two things to note, because both are common mistakes:

- **`row_number() OVER ()` with no `ORDER BY` is not stable.** Always give it
  an explicit ordering, or the positions you build the input with may not
  match the ones you read the output with.
- **DuckDB lists are 1-indexed, the returned assignments are 0-based.** Hence
  the `+ 1` above, and `bins[o.pos]` using the 1-based position.

## Which function do I want?

| Your decision | Family | Start with |
|---|---|---|
| Fit items into fewest containers | packing | `opt_pack_best_of` |
| Pick a subset under one capacity | knapsack | `opt_knapsack_best_of` |
| Group orders into capacity-limited waves | wave | `opt_wave_best_of` |
| Order jobs to cut changeover cost | sequence | `opt_sequence_best_of` |
| Order jobs to hit due dates with setups | schedule | `opt_schedule_best_of` |
| Choose which products to stock | assortment | see below |
| Choose assets and weights | portfolio | `opt_portfolio_best_of` |

`opt_*_best_of` runs every algorithm in its family and returns the best
result. Start there; reach for a specific member when you need a particular
speed or behaviour.

**Assortment has two families that are not interchangeable.** Use
`opt_assortment_*` when listed products cannibalise each other, and
`opt_assortment_recapture_*` when you are delisting and want demand to flow to
the survivors. They take identical arguments and answer opposite questions —
see [Choosing a Family](02-choosing-a-family.md).

## Checking the answer

Optimization results are easy to trust and hard to verify. Two habits worth
keeping:

```sql
-- 1. Feasibility: no container over capacity.
WITH packed AS (
    SELECT opt_pack_best_of(list(size ORDER BY carton_id), 1000.0).assignment AS bins,
           list(size ORDER BY carton_id) AS sizes
    FROM cartons
)
SELECT bool_and(load <= 1000.0) AS all_within_capacity
FROM (SELECT sum(s) AS load
      FROM (SELECT unnest(bins) AS b, unnest(sizes) AS s FROM packed)
      GROUP BY b);

-- 2. A lower bound you can compute yourself: total volume / capacity.
SELECT ceil(sum(size) / 1000.0) AS cannot_possibly_beat_this FROM cartons;
```

If a result ever beats a bound you computed independently, the bug is in the
result, not the bound. See [Validating Results](04-validating-results.md).

## Next

- [Choosing a Family](02-choosing-a-family.md) — matching your problem to a function
- [Working With Your Tables](03-working-with-your-tables.md) — lists, matrices, joins
- [Validating Results](04-validating-results.md) — bounding heuristics with exact solvers
- [Theory](../theory/01-how-it-works.md) — what the algorithms actually do
