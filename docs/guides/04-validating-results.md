# Validating Results

> An optimizer's output looks equally plausible whether it is optimal or
> badly wrong. Here is how to tell.

**Use this guide to:**
- Check a result is feasible before you act on it
- Bound how far a heuristic is from optimal
- Build a regression test that would actually fail

This guide exists because of a real defect in this library: the scheduling
local search returned **168** on an instance whose proven optimum was **60**,
and nothing about the answer looked wrong. It was caught only by comparing
against a value computed independently.

## 1. Feasibility first

Every family has constraints that a buggy solver could violate. Check them
directly — they are cheap.

```sql
-- packing: every item placed exactly once, no bin over capacity
WITH r AS (
  SELECT opt_pack_best_of(list(size ORDER BY pos), 1000.0).assignment AS bins,
         list(size ORDER BY pos) AS sizes
  FROM ordered_items
)
SELECT
  (SELECT count(*) FROM ordered_items) = len(bins) AS every_item_placed,
  (SELECT bool_and(load <= 1000.0)
     FROM (SELECT sum(s) AS load
             FROM (SELECT unnest(bins) AS b, unnest(sizes) AS s FROM r)
            GROUP BY b))            AS within_capacity
FROM r;
```

```sql
-- portfolio: at most k holdings, no position over the cap, weights sum to 1
WITH r AS (SELECT opt_portfolio_best_of(mu, cov, 5, 0.35) AS p FROM inputs)
SELECT
  len(list_filter(p.weights, lambda w: w > 1e-9)) <= 5              AS cardinality_ok,
  (SELECT bool_and(w <= 0.35 + 1e-9) FROM (SELECT unnest(p.weights) AS w)) AS cap_ok,
  abs(list_sum(p.weights) - 1.0) < 1e-6                        AS weights_sum_to_one
FROM r;
```

```sql
-- scheduling / sequencing: the order is a genuine permutation
WITH r AS (SELECT opt_schedule_best_of(p, d, w, s, n)."order" AS ord FROM inputs)
SELECT len(ord) = (SELECT count(*) FROM jobs)
   AND len(list_distinct(ord)) = len(ord) AS is_a_permutation
FROM r;
```

## 2. Bound the gap with something independent

A heuristic tells you what it found, never how good it is. Get a second
opinion that does not come from the same code path.

### Use the exact solver on a small case

```sql
-- how far is the heuristic on an instance small enough to solve exactly?
SELECT
  opt_schedule_local_search(p, d, w, s, n).total_weighted_tardiness AS heuristic,
  opt_schedule_exact(p, d, w, s, n).total_weighted_tardiness        AS optimal
FROM inputs;
```

`schedule_exact` refuses beyond 14 jobs and `knapsack_exact` is
pseudo-polynomial, so this works on a scaled-down version of your problem.
A heuristic that is near-optimal on small instances is not guaranteed to be
near-optimal on large ones — but one that is *far off* on small instances
will not improve with size.

### Use a bound you can compute in SQL

Some bounds need no solver at all, and they are the most trustworthy check
you have because they share nothing with the implementation.

```sql
-- packing: you can never use fewer bins than total volume / capacity
SELECT ceil(sum(size) / 1000.0) AS lower_bound FROM items;

-- knapsack: you can never capture more value than everything, nor exceed capacity
SELECT sum(value) AS upper_bound, 1000.0 AS capacity FROM candidates;
```

If a result violates one of these, the result is wrong. This is worth
asserting in production, not just in tests.

### Compare against the weak member

Every family ships a deliberately naive algorithm. If `best_of` does not beat
it on your data, either your instance is easy (fine — stop optimizing) or
something is broken (not fine).

```sql
SELECT opt_pack_best_of(sizes, cap).bins_used        AS best,
       opt_pack_next_fit(sizes, cap).bins_used       AS naive
FROM inputs;
```

## 3. Write tests that could fail

A test asserting a function returns what it currently returns passes on a
broken implementation. Two rules make tests real:

**Assert against externally computed values.** Construct an instance whose
answer you know independently — by construction, by brute force, or from a
textbook — and assert *that* number.

```sql
-- 60 comes from an independent Held-Karp computation, not from this library
SELECT opt_schedule_exact(p, d, w, s, n).total_weighted_tardiness = 60 FROM fixture;
```

**Assert the naive rule is beaten.** If your fixture is so easy that the
greedy member ties the exact solver, the test proves nothing about the
algorithm — it only proves the instance is trivial.

```sql
SELECT opt_knapsack_greedy_ratio(v, w, cap).total_value
     < opt_knapsack_exact(v, w, cap).total_value AS fixture_discriminates
FROM fixture;
```

`test/sql/pilot_scenarios.test` in this repository is written to that
standard: every expected value comes from outside the extension — Falkenauer
triplets whose optimum is fixed by construction, subset-sum solved by DP,
Held-Karp scheduling, and portfolio enumeration over all 15,504 subsets.

## 4. When results disagree with a bound

If a solver returns something that beats a bound you computed independently,
suspect the solver — but check the bound too. Both have been wrong here:

- An "exact" scheduler returned 91 against a heuristic's 60. The scheduler
  was wrong (it kept one label per state when the clock depends on the path).
- A portfolio search returned 5.3289 against a "best known" 5.2958. The
  *reference* was wrong — only the top 150 of 15,504 subsets had been
  refined.

The rule that resolves it: a bound proven by construction or exhaustive
enumeration outranks anything produced by a heuristic. A reference produced
by *another* heuristic outranks nothing.
