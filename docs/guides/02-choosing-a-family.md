# Choosing a Family

> Match the shape of your decision to the right function — including the two
> cases where picking wrong is silent and expensive.

**Use this guide to:**
- Identify your decision shape
- Pick between members of a family
- Avoid the two mistakes that produce confidently wrong answers

## Identify the shape, not the domain

The families are named after decision shapes, not industries. A hospital
allocating theatre slots and a factory sequencing a press line are the same
problem.

| Shape | You are choosing | Family |
|---|---|---|
| Partition into groups under a capacity | which group each item goes in | `pack_*` |
| Subset under one capacity | in or out | `knapsack_*` |
| Partition into ordered groups, earlier is better | which wave each item goes in | `wave_*` |
| Permutation, cost between neighbours | what follows what | `sequence_*` |
| Permutation, due dates and setups | what follows what, against deadlines | `schedule_*` |
| Subset of size ≤ k, items interact | which to keep | `assortment_*` |
| Subset of size ≤ k plus continuous weights | which, and how much of each | `portfolio_*` |

## Choosing within a family

Every family has the same structure: several named algorithms plus a
`best_of` that runs them all and returns the winner.

**Default to `best_of`.** It is the only member that cannot lose to another
member, and for the sizes these functions handle the cost of running all of
them is rarely the bottleneck.

Reach for a specific member when you need something `best_of` cannot express:

| You need | Use |
|---|---|
| The proven optimum, small instance | `knapsack_exact`, `schedule_exact` |
| A deliberately weak baseline to beat | `pack_next_fit`, `portfolio_top_return`, `assortment_top_margin` |
| Speed on very large inputs | the greedy members (`*_greedy_*`, `*_first_fit_*`) |
| To reproduce an incumbent policy | `sequence_as_given`, `wave_as_given` |

The weak members are in the library on purpose. If you are evaluating whether
optimization is worth it at all, you need something honest to compare
against, and "what we do today" is usually a greedy rule.

## Two ways to pick wrong silently

### 1. Assortment: cannibalisation vs recapture

These two families take **identical arguments** and return an **identical
struct**, but score a shelf under opposite economics. Nothing in your data
says which one you mean.

| | `assortment_*` | `assortment_recapture_*` |
|---|---|---|
| Question | which products to stock when they steal from each other | which survivors soak up demand when we drop products |
| A listed product earns | margin × (demand − what other **listed** products take) | margin × demand, **in full** |
| Plus | — | demand handed over by **delisted** products, at the listed product's margin |
| Listing a near-duplicate | destroys value | is neutral |
| A high-margin, low-demand item | rarely worth a slot | can be worth a slot purely as a recapture sink |

Getting this backwards is worse than not optimizing. On a real 14-product
instance, running the cannibalisation family against a recapture objective
scored **1433** where a trivial top-margin ranking scored **1741** and the
optimum was **3888** — it actively steered away from the answer.

Ask yourself: *when I drop a product, does its demand disappear, or move?*
If it moves to the survivors, you want `recapture`.

### 2. Sequencing vs scheduling

Both order jobs. They optimize different things.

| | `sequence_*` | `schedule_*` |
|---|---|---|
| Minimises | total changeover cost | priority-weighted tardiness |
| Knows about due dates | no | yes |
| Use when | changeovers dominate and everything is due "whenever" | deadlines matter |

If you have due dates and use `sequence_*`, you will get a beautifully
efficient order that misses them.

## Matrix-shaped inputs

`sequence_*`, `schedule_*`, `assortment_*` and `portfolio_*` take a matrix
flattened **row-major**. Most tables store these as `(from, to, value)`
triples, so build the matrix with:

```sql
opt_matrix_from_triples(list(from_idx), list(to_idx), list(value), n, index_base)
```

`index_base` is subtracted from your ids: pass `1` for 1-based ids, `0` for
0-based. Missing cells become 0; duplicates raise rather than silently
keeping the last one. See [Working With Your Tables](03-working-with-your-tables.md).

## Sizes and limits

| Family | Practical size | Notes |
|---|---|---|
| `pack_*`, `wave_*`, `knapsack_greedy_*` | large | linear or n log n |
| `knapsack_exact` | moderate | pseudo-polynomial in capacity |
| `sequence_*`, `assortment_*`, `portfolio_*` | moderate | quadratic per improving pass |
| `schedule_exact` | **≤ 14 jobs** | exponential; refuses beyond, naming the fallback |

Functions that would blow up refuse with an error naming the alternative,
rather than running until you kill the query.
