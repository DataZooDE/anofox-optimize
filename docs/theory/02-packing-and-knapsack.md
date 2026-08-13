# Packing and Knapsack

> Two problems that look alike and are not: one partitions everything, the
> other selects a subset.

## Bin packing (`pack_*`)

**Problem.** Given item sizes and a bin capacity, assign every item to a bin
so that no bin exceeds capacity, using as few bins as possible.

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

**`exact`** — dynamic programming over capacity. Returns the proven optimum.
Cost is O(n · capacity) time and memory, so it scales with the *numeric*
capacity, not just the item count. A capacity of 1,000 with 60 items is
trivial; a capacity of 10^9 is not.

**`best_of`** — runs the greedy members. It is explicitly **not** exact, and
its description says so, because a caller who wants a guarantee must ask for
`exact` and accept its cost.

### Subset-sum is knapsack with value = weight

A common shape hides here. When you are choosing which shipments to put on a
cheaper capacity-limited carrier, the saving is proportional to weight, so
value equals weight and the problem is exactly subset-sum: fill the capacity
as fully as possible.

This is worth recognising because it is where exact solving pays. On the
60-shipment instance in the test suite, a perfect 1000-unit fill exists,
greedy rules top out at 968, and `knapsack_exact` finds the perfect fill.
Two rounding-level percent of total cost — but it is free, and provably the
best available.

## Which to reach for

| Situation | Function |
|---|---|
| Everything must be placed somewhere | `pack_*` |
| Items may be left out | `knapsack_*` |
| Capacity is a modest integer, you want a guarantee | `knapsack_exact` |
| Very large input, good enough is fine | `knapsack_greedy_ratio` |
| Items combine into exact fills | `pack_bin_completion` |
