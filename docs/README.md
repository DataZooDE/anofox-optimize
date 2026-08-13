# anofox-optimize documentation

Two tracks, deliberately separate.

## Guides — how to use it

Task-oriented, copy-pasteable SQL.

1. [Getting Started](guides/01-getting-started.md) — install, first optimization, reading results back
2. [Choosing a Family](guides/02-choosing-a-family.md) — matching your decision shape to a function
3. [Working With Your Tables](guides/03-working-with-your-tables.md) — lists, matrices, joins, per-group optimization
4. [Validating Results](guides/04-validating-results.md) — feasibility, bounds, tests that can actually fail

## Theory — how it works behind the curtain

What the algorithms do, what they guarantee, and where they fail.

1. [How It Works](theory/01-how-it-works.md) — the family pattern and the design rules
2. [Packing and Knapsack](theory/02-packing-and-knapsack.md)
3. [Sequencing and Scheduling](theory/03-sequencing-and-scheduling.md)
4. [Selection: Assortment and Portfolio](theory/04-selection.md)
5. [Complexity and Limits](theory/05-complexity-and-limits.md)

## Reference

[API_REFERENCE.md](API_REFERENCE.md) — every function, generated from
`duckdb_functions()` of the built extension. Never hand-edited: the built
binary is the source of truth, so the reference cannot drift from the code.
