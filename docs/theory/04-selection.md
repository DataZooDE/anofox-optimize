# Selection: Assortment and Portfolio

> Choosing a subset when the items interact. What you keep changes what the
> rest are worth.

Both families here are *quadratic* selection problems: the value of a chosen
set is not the sum of its members' individual values. That interaction term
is precisely what a knapsack cannot represent, and why these are separate
families rather than a knapsack with clever inputs.

## Assortment — two opposite models

**Problem.** Choose at most `k` products to stock. Products substitute for one
another, so the choice of what to keep changes what each kept product earns.

There are **two** genuinely different real decisions with this shape, and
they are close to opposites. They take identical arguments and return
identical structs, so nothing in your data reveals which one you meant.

### Cannibalisation (`assortment_*`)

Each listed product earns `margin × (base_demand − demand taken by the other
LISTED products)`.

```
value(S) = Σ_{i∈S} margin_i · max(0, demand_i − Σ_{j∈S, j≠i} sub[j][i] · demand_j)
```

The decision this models: *we have shelf space; stocking near-duplicates
wastes it.* Listing two similar products means each erodes the other, so a
good assortment spreads across distinct demand.

### Recapture (`assortment_recapture_*`)

Each listed product keeps its own `margin × demand` **in full**, and
additionally earns the demand handed to it by **delisted** products, valued at
the *listed* product's margin.

```
value(S) = Σ_{i∈S} margin_i · demand_i
         + Σ_{j∉S} Σ_{i∈S} demand_j · sub[j][i] · margin_i
```

The decision this models: *we must drop products; which survivors soak up the
orphaned demand most profitably.* Here a listed product is worth **more** when
the products it substitutes for are dropped — the exact inverse of
cannibalisation, where listed neighbours are a liability.

A consequence worth internalising: under recapture, a **high-margin,
low-demand** product can be worth a slot purely as a recapture sink. No
`margin × demand` ranking will ever select it.

### Why this distinction is in the library at all

It was found by measurement, not review. A search using the cannibalisation
family against a recapture objective converged to **1433** on a 14-product
instance where a trivial top-margin ranking scored **1741** and the optimum
was **3888**. Optimising one objective while being scored on the other is
worse than not optimising at all.

Both are legitimate, so the fix was to add the missing family rather than
change the old one, and to make every description state its model in full and
name the sibling family as the alternative.

### The members (both families)

**`top_margin`** — rank by standalone `margin × demand`, take the top k.
Ignores interaction entirely. The naive baseline.

**`greedy_marginal`** — add products one at a time, each time taking whichever
raises the *total* objective most given what is already chosen. Stops early
when nothing helps, even below the limit — under cannibalisation, listing a
pure cannibaliser loses money.

**`local_search`** — greedy, then swap a listed product for an unlisted one
while that improves the objective. Escapes the ordering greedy locked itself
into.

**`best_of`** — all of the above.

## Portfolio (`portfolio_*`)

**Problem.** Choose at most `k` assets and their weights to maximise the
Sharpe ratio `(wᵀμ) / √(wᵀΣw)`, with weights summing to 1 and no position
above a cap.

**Hardness.** The continuous mean-variance problem is a convex QP and easy.
Adding the **cardinality constraint** — hold at most k — makes it a mixed
integer QP and NP-hard. That constraint is the whole difficulty.

### The members

**`top_return`** — take the k highest expected returns, weight them
**equally**. Ignores covariance completely, so it will happily buy k assets
that all move together. The naive baseline, and on a correlated instance it
is very bad: 0.74 Sharpe where the optimum is 5.33.

**`top_return_optimized`** — same k assets, but optimise the *weights* on that
fixed support by projected gradient ascent. Isolates the value of weighting
from the value of choosing: on the same instance it moves 0.74 → 0.94, which
shows the support was the problem, not the weights.

**`greedy_sharpe`** — build the holding set incrementally, adding whichever
asset most improves Sharpe *after re-optimising the weights*, then a swap
phase that exchanges a held asset for an unheld one while that helps.

> The swap phase is load-bearing. Without it, greedy growth cannot help when
> `k` equals the minimum support, and the function tied the naive rule
> (0.6156 → 0.9004 when swaps were added). A "greedy" that only grows is
> not enough for this problem.

**`best_of`** — all of the above.

### Why covariance dominates

The trap in cardinality-constrained portfolios is that the highest-return
assets are often the most correlated with each other — they are exposed to
the same factor. Picking by return concentrates that exposure and inflates
`wᵀΣw` far faster than it raises `wᵀμ`.

The test-suite instance is built to make this explicit: the highest-return
assets are a deliberate cluster with pairwise correlation 0.75–0.93. Buying
by return gives Sharpe 0.74; the optimal five-asset set gives 5.33.

## Which to reach for

| Situation | Function |
|---|---|
| Stocking decision, products cannibalise | `assortment_best_of` |
| Delisting decision, demand flows to survivors | `assortment_recapture_best_of` |
| Asset selection with a holding limit | `portfolio_best_of` |
| Showing what ranking-by-return costs | `portfolio_top_return` |
| Separating "wrong assets" from "wrong weights" | compare `top_return` with `top_return_optimized` |
