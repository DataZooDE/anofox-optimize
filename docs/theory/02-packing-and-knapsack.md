# Packing and Knapsack

> Two problems that look alike and are not: one partitions everything, the
> other selects a subset.

## Bin packing (`pack_*`)

**Problem.** Given item sizes and a bin capacity, assign every item to a bin
so that no bin exceeds capacity, using as few bins as possible.

**What "fits" means.** A bin fits when its sizes, summed in item order (the
order of the input list) in doubles, are at most the capacity, with no
tolerance: `list_sum(list(size ORDER BY item))`, never an unordered `sum()`.
The same rule holds for `wave_*` and `knapsack_*`. The members place items
by a running load summed in their own order, which can differ in the last
bit (largest first, 0.3 + 0.2 + 0.1 is 0.6; in item order 0.1 + 0.2 + 0.3 is
0.6000000000000001), so every member ends by moving an item out of any bin
or wave that is over, into the first other one it fits, else a new one: the
smallest item for `pack_*`, the lowest-priority order for `wave_*`. The
repair may move an item into an EARLIER bin or wave. On integer sizes it
never changes anything, and `next_fit` and `wave_as_given`, which place
items in item order, never need it. In `pack_by` and `wave_by`, item order
is the order of `item_col`.

**Hardness.** NP-hard. Deciding whether a set fits in two bins is already
equivalent to partition.

**The lower bound you always have.** `ceil(sum(sizes) / capacity)`. No packing
can beat it. It costs nothing to compute and is the single most useful check
on any result.

### The members

**`next_fit`** — keep one open bin; start a new one when the item does not
fit. Never revisits a closed bin. Fast, and can use up to twice the optimal
number of bins. Present as an honest baseline.

**`first_fit_decreasing`** — sort descending, place each item in the first bin
it fits. The classic. Guarantees at most `11/9 · OPT + 6/9` bins. Strong when
sizes are spread.

**`best_fit_decreasing`** — same, but choose the *fullest* bin it still fits
in. Same guarantee, different failure mode: it packs tightly and can leave
awkward gaps.

**`worst_fit_decreasing`** — choose the *emptiest* bin. Deliberately keeps
capacity spread across bins, which helps when a large item is still to come.

**`bin_completion`** — build each bin by looking for a *combination* that
fills it well, rather than adding items one at a time. Beats the fit rules
when items combine into exact fills, which is exactly where greedy rules
struggle.

**`local_search`** — start from a greedy packing and move items between bins
while that reduces the count.

> A correctness note worth recording: an early version of `local_search`
> returned a packing with fewer bins than the theoretical lower bound. The
> swap logic moved an item *into* the bin being dissolved. Feasibility
> assertions in the test suite caught it. This is why every member is checked
> for feasibility, not just plausibility.

### The adversarial case: triplets

Falkenauer *triplet* instances are built so every bin holds exactly three
items summing exactly to capacity. The optimum is exactly n/3 by
construction, and greedy fit rules reliably miss it — first-fit-decreasing
needs 23 bins where 20 is optimal on the 60-item instance in
`test/sql/pilot_scenarios.test`.

They are in the test suite because a packing fixture where every algorithm
ties tells you nothing.

## Knapsack (`knapsack_*`)

**Problem.** Given item values and weights and one capacity, choose a subset
maximising total value without exceeding capacity. Unlike packing, items may
be left out entirely.

**Hardness.** NP-hard, but *weakly* so: the DP below is polynomial in the
numeric capacity, which is why an exact solver is practical here and not for
scheduling.

### The members

**`greedy_value`** — take the highest values first. Ignores weight, so a
single heavy item can crowd out several better ones.

**`greedy_ratio`** — take the best value-per-weight first. The standard
heuristic and usually good. Its worst case is well known: with capacity 10
and items (value 10, weight 10) and (value 6, weight 6), ratio ties them and
may take the 6.

"Fits" is the rule of `pack_*` above: the selected weights, summed in item
order in doubles, are at most the capacity, with no tolerance. Every member
returns such an answer, and its `total_weight` is that sum, the same number
a SQL `coalesce(list_sum(...), 0)` over the selected weights in item order
returns (`list_sum` of an empty list is NULL; in
`knapsack_by`, item order is the order of `item_col`). So 0.1 and 0.2 do not
both fit a capacity of 0.3: in doubles they sum to 0.30000000000000004.

**`best_of`** — runs both greedy members and, within the limits below, a
dynamic program over capacity, and keeps the most valuable answer, so it
never loses to a greedy member. `exact` is an **alias** of `best_of`, not a
separate member: it once raised past the limits, but inside them it always
returned `best_of`'s value, and a member that matches another on every
instance gives a search nothing to choose between.

Where the DP runs, the result is the proven optimum when every weight and
the capacity are whole multiples of 0.0001 and their sums are exact in
doubles (integers, halves, quarters). Weights finer than 0.0001 are rounded
**up** to the next 0.0001 and the capacity down for the DP, which optimizes
those rounded weights; the answer, the better of that and the greedy
members, then carries **no** optimality guarantee. With values [10, 6, 6],
weights [0.00008, 0.00006, 0.00006] and capacity 0.00012, every weight is 1
unit against a capacity of 1 unit, so `best_of` returns 10 while {6, 6} fits
and is worth 12. Decimal weights such as 0.1 can fill the capacity exactly in
0.0001 units yet sum over it in doubles; the DP's answer then drops the item
whose removal costs least, or takes the best selection one unit lighter,
which the table already holds, if that is worth more. That can miss a
selection that fills the capacity and does fit: on a sweep of 1,000
instances with weights in steps of 0.1 it fell short of the strict optimum
on 10. Items heavier than the capacity are never taken and play no part in
the scaling.

Cost is O(n · capacity) time and memory, so it scales with the *numeric*
capacity, not just the item count. A capacity of 1,000 with 60 items is
trivial; a capacity of 10^9 is not. The table has items × (capacity + 1)
cells, counting weights in the smallest unit (1, 0.1, … 0.0001) that makes
every item that can fit, and the capacity, whole. It needs 8 bytes per
capacity unit plus one bit per cell. The DP runs only when the table is at
most 50 million cells and within the memory budget set with

```sql
SET anofox_optimize_knapsack_max_memory = '2GB';   -- default '512MB', '64MB' in DuckDB-Wasm
```

Past either limit `best_of` does **not** raise: it returns the best greedy
answer, which is not exact, and nothing in the result says which case
occurred. A caller who needs the guarantee must keep the instance within the
limits, by raising the budget or rounding weights to coarser units. The
table lives outside DuckDB's buffer manager, so in DuckDB-Wasm, where every
thread allocates from one shared 32-bit heap, the default is 64MB rather
than 512MB. The budget bounds each call's table, not the process: several
threads can each hold one.

The DP's time and memory grow with items × capacity, so near the limits
`best_of` is far slower than the greedy members. `knapsack_by` runs
`best_of` by default and pays that cost once per group. It skips the DP only
when greedy is provably optimal for the DP's rounded instance: every
positive-value item fits, nothing fits, or the best-ratio prefix fills the
capacity exactly (the fractional relaxation then has no fractional item).
For weights in multiples of 0.0001 that is the optimum; for finer weights it
is not (the example above returns 10 from `best_of` too). That makes "everything
fits" instances as cheap as greedy, but on a typical instance, where some
item has to be left out, it saves nothing.

### Subset-sum is knapsack with value = weight

A common shape hides here. When you are choosing which shipments to put on a
cheaper capacity-limited carrier, the saving is proportional to weight, so
value equals weight and the problem is exactly subset-sum: fill the capacity
as fully as possible.

This is worth recognising because it is where exact solving pays. On the
60-shipment instance in the test suite, a perfect 1000-unit fill exists,
greedy rules top out at 968, and `knapsack_best_of`'s DP finds the perfect fill.
Two rounding-level percent of total cost — but it is free, and provably the
best available.

## Which to reach for

| Situation | Function |
|---|---|
| Everything must be placed somewhere | `pack_*` |
| Items may be left out | `knapsack_*` |
| Capacity is a modest integer, you want a guarantee | `knapsack_best_of`, within its DP limits |
| Very large input, good enough is fine | `knapsack_greedy_ratio` |
| Items combine into exact fills | `pack_bin_completion` |
