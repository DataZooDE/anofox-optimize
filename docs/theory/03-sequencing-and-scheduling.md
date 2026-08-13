# Sequencing and Scheduling

> Both order jobs. One minimises changeover, the other hits deadlines. Mixing
> them up produces an efficient schedule that misses every due date.

## Sequencing (`sequence_*`)

**Problem.** Given a matrix of changeover costs between jobs, find the order
minimising total changeover. This is the travelling salesman problem in
disguise — jobs are cities, changeovers are distances.

**Hardness.** NP-hard.

### The members

**`as_given`** — the input order, unchanged. Your incumbent policy, and the
number every improvement should be measured against.

**`nearest_neighbour`** — repeatedly jump to the cheapest unvisited job.
Fast, and characteristically leaves one expensive edge at the end when the
only remaining job is far from everything.

**`two_opt`** — start from nearest neighbour, then repeatedly reverse a
segment when doing so shortens the tour. The standard local improvement;
removes the crossing edges that nearest neighbour produces.

**`best_of`** — all of the above.

## Scheduling (`schedule_*`)

**Problem.** One machine, jobs with processing times, due dates and
priorities, plus a **sequence-dependent setup** between consecutive jobs.
Minimise total priority-weighted tardiness, `Σ wⱼ · max(0, Cⱼ − dⱼ)`.

In the standard notation: `1 | s_ij | Σ wⱼTⱼ`.

**Hardness.** NP-hard even without setups. With them, the completion time of
every job depends on the whole preceding order.

### The setup matrix and the virtual start

Setups are `(n+1) × (n+1)`, row-major, with index 0 as a **virtual start**:
`setup[i*(n+1)+j]` is the cost of running job `j-1` after job `i-1`, and row 0
is the cost of running a job first. The virtual start matters — the first
job's setup is real machine time, and ignoring it changes every completion
time downstream.

### The members

**`edd`** — earliest due date. Optimal for *maximum lateness* with no setups,
and the textbook default. It ignores priorities and setups entirely, which
makes it arbitrarily bad here: on the test-suite instance it pays **617**
against an optimum of **60**.

**`wspt`** — weighted shortest processing time, ordering by `wⱼ/pⱼ`. Optimal
for weighted *completion* time with no due dates. Ignores due dates and
setups.

**`atcs`** — Apparent Tardiness Cost with Setups. At each step, pick the job
maximising

```
(wⱼ/pⱼ) · exp(−slackⱼ / k₁·p̄) · exp(−setupⱼ / k₂·s̄)
```

Three pressures at once: value density, urgency, and cheapness of the
changeover. The exponential terms decay the contribution of jobs that are not
yet urgent or expensive to switch to. This is the strongest constructive rule
in the family.

**`local_search`** — ATCS, then improvement moves: swap any pair, and
**relocate** a job to any other position.

> Relocation is not optional, and this was learned the hard way. An earlier
> version tried only *adjacent* swaps and returned **168** against a proven
> optimum of **60** — worse than plain hand-written SQL for the same problem.
> Under sequence-dependent setups, moving a single job next to a cheap
> predecessor can save more than any number of neighbour exchanges. With
> relocation added it reaches 60.

**`exact`** — the proven optimum, for at most 14 jobs.

### Why the exact solver needs a Pareto frontier

The natural approach is Held-Karp: a state per (set of scheduled jobs, last
job), keeping the cheapest way to reach it. **That is wrong here**, and
subtly so.

With sequence-dependent setups, the elapsed clock at a state depends on the
*path* taken, not just on which jobs are done. So two ways of reaching the
same (set, last) can differ in both cost and time — and a costlier prefix
that finishes *earlier* may beat a cheaper one on everything that follows,
because every later job becomes less tardy.

Keeping only the cheapest label discards that branch. The first
implementation did exactly this and returned **91** on an instance where the
local search returned **60** — an "exact" solver losing to a heuristic, which
is how the bug was found.

The fix is to keep a **Pareto frontier** of `(time, cost)` labels per state.
A label dominates another when it is *neither later nor dearer*; anything
dominated can be discarded because nothing reachable from it is better. What
survives is every trade-off that could still win. With the frontier, the
solver returns 60, matching an independent computation.

The cost is that the state space is no longer bounded by `2ⁿ · n` but by the
frontier sizes, which is why the limit is 14 jobs rather than 20. The
function refuses beyond that with an error naming the heuristic to use
instead.

## Which to reach for

| Situation | Function |
|---|---|
| Changeovers dominate, no meaningful due dates | `sequence_best_of` |
| Due dates matter | `schedule_best_of` |
| ≤ 14 jobs and you want the proven optimum | `schedule_exact` |
| You need to show what the current policy costs | `sequence_as_given`, `schedule_edd` |
| Large instance, tardiness matters | `schedule_local_search` |
