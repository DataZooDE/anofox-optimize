# How It Works

> What this extension is, what it is not, and the design rules every family
> follows.

## What this is

A set of classical combinatorial optimization algorithms, compiled into
DuckDB and callable as scalar functions over lists. No solver process, no
external service, no modelling language. You aggregate columns into lists,
call a function, and get a struct back.

## What this is not

**Not a general-purpose solver.** There is no way to express an arbitrary
objective or add your own constraints. Each family solves one fixed problem
shape. If your problem is not one of those shapes, this library cannot bend
to fit it, and you want a MIP solver.

**Not always optimal.** Most families are heuristic. Two functions are exact
and say so in their names (`knapsack_exact`, `schedule_exact`); everything
else returns a good answer with no guarantee attached. The
[validation guide](../guides/04-validating-results.md) covers how to find out
how good.

**Not a black box.** Every function's `duckdb_functions()` description states
what its algorithm does, including where it is weak. That text is the
contract.

## The family pattern

Every problem shape gets a *family*: several algorithms sharing one signature
and one return struct, plus a `best_of` member.

```
opt_pack_next_fit            weak baseline
opt_pack_first_fit_decreasing
opt_pack_best_fit_decreasing
opt_pack_worst_fit_decreasing
opt_pack_bin_completion
opt_pack_local_search
opt_pack_best_of             runs them all, returns the winner
```

Three rules govern this, and they are enforced by tests:

**One signature per family.** Members are interchangeable, so you can swap
algorithms without rewriting the query. This is also what makes `best_of`
possible.

**A family must spread.** If every member returns the same answer on your
data, the family is not giving you a choice — and a test fixture where they
all tie proves nothing about any of them.

**Include a weak member.** `next_fit`, `top_return`, `top_margin`,
`as_given` are deliberately naive. You need an honest baseline to know
whether optimizing is worth anything, and "what we do today" is usually a
greedy rule. A library that only ships its best algorithm cannot tell you it
made no difference.

## Why `best_of` is usually right

Combinatorial heuristics have complementary failure modes. First-fit-
decreasing is strong when item sizes are spread out and weak when they
cluster near half the capacity; bin completion is the reverse. Running all of
them and keeping the winner costs a small multiple of running one, and cannot
be worse than any single member.

`best_of` deliberately does **not** call the exact solvers. Silently going
exponential past some hidden size threshold is exactly the surprise a library
should not spring on you; if you want the optimum, you ask for it by name and
accept the size limit that comes with it.

## Feasibility is not optional

Every algorithm returns a solution that satisfies its constraints, or raises.
This sounds obvious, and it matters more than it sounds: an optimizer scored
on an objective has an incentive to cheat, and a packing that quietly
overfills a bin will beat every honest packing.

The test suite checks feasibility for *every* member of every family, not
just `best_of` — one assignment per item, indices in range, nothing over
capacity, permutations that are genuinely permutations, weights that sum to
one and respect their cap.

## Errors, not garbage

Nonsense inputs raise with a message naming the problem and the fix, rather
than returning a plausible number:

- negative or non-finite sizes are rejected
- substitution rates outside [0,1] are rejected — they would create or
  destroy demand rather than move it
- duplicate matrix cells raise rather than silently keeping the last one
- oversized inputs to exponential functions raise, naming the heuristic to
  use instead

## Where the algorithms come from

| Family | Problem | Approach |
|---|---|---|
| `pack_*` | 1-D bin packing | greedy fits, bin completion, local search |
| `knapsack_*` | 0/1 knapsack | greedy ratio/value, exact DP |
| `wave_*` | capacitated grouping, weighted completion | priority and density rules |
| `sequence_*` | sequence-dependent changeover, TSP-like | nearest neighbour, 2-opt |
| `schedule_*` | 1‖Σw·T with setups | EDD, WSPT, ATCS, local search, exact DP |
| `assortment_*` | quadratic selection | greedy marginal, swap local search |
| `portfolio_*` | cardinality-constrained mean-variance | greedy Sharpe, projected gradient |

Details, guarantees and failure modes are in the family chapters:

- [Packing and Knapsack](02-packing-and-knapsack.md)
- [Sequencing and Scheduling](03-sequencing-and-scheduling.md)
- [Selection: Assortment and Portfolio](04-selection.md)
- [Complexity and Limits](05-complexity-and-limits.md)
