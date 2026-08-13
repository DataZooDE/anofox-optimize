# anofox-optimize

[![Main Extension Distribution Pipeline](https://github.com/DataZooDE/anofox-optimize/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/DataZooDE/anofox-optimize/actions/workflows/MainDistributionPipeline.yml)
[![DuckDB](https://img.shields.io/badge/DuckDB-v1.4.5%20LTS%20%7C%20v1.5.5-blue)](https://duckdb.org)
[![License](https://img.shields.io/badge/license-BSL%201.1-green)](LICENSE)

**Combinatorial decision algorithms as DuckDB functions.** Pack containers,
choose an assortment, sequence a machine, allocate a portfolio — in SQL,
against the tables the data already lives in. No solver process, no external
service, no modelling language.

```sql
INSTALL anofox_optimize FROM community;
LOAD anofox_optimize;

-- Fit cartons into the fewest 1000-unit containers
SELECT opt_pack_best_of(list(size ORDER BY carton_id), 1000.0).bins_used
FROM cartons;
```

## What it does

Eight families, each solving one decision shape. Every member of a family
shares an identical signature, so swapping one identifier swaps the algorithm
and nothing else.

| Family | Decision | Example function |
|---|---|---|
| `pack_*` | fit items into fewest containers | `opt_pack_best_of` |
| `knapsack_*` | pick a subset under one capacity | `opt_knapsack_exact` |
| `wave_*` | group orders into capacity-limited waves | `opt_wave_best_of` |
| `sequence_*` | order jobs to cut changeover cost | `opt_sequence_two_opt` |
| `schedule_*` | order jobs against due dates with setups | `opt_schedule_best_of` |
| `assortment_*` | choose what to stock when products interact | `opt_assortment_best_of` |
| `portfolio_*` | choose assets and weights under a holding limit | `opt_portfolio_best_of` |
| `matrix_from_triples` | build a matrix from `(from, to, value)` rows | — |

Every function has a short `opt_` alias: `anofox_optimize_pack_best_of` and
`opt_pack_best_of` are the same function.

`opt_*_best_of` runs every algorithm in its family and returns the winner.
Start there.

## A real example

Choosing which shipments go on a cheaper but capacity-limited carrier is
exactly subset-sum. On a 60-shipment instance where a perfect 1000-unit fill
exists, greedy rules top out at 968 — the exact solver finds the perfect fill:

```sql
SELECT opt_knapsack_exact(
         list(weight ORDER BY shipment_id),   -- value == weight: savings scale with weight
         list(weight ORDER BY shipment_id),
         1000.0
       ).total_weight AS loaded_on_cheap_carrier
FROM shipments;
-- 1000.0
```

## Honesty about guarantees

Two functions return a **proven optimum** and say so in their names:
`knapsack_exact` and `schedule_exact`. Everything else is heuristic and comes
with no guarantee attached.

Each family also ships a deliberately **weak** member (`pack_next_fit`,
`portfolio_top_return`, `assortment_top_margin`, `sequence_as_given`). You
need an honest baseline to know whether optimizing bought you anything, and
"what we do today" is usually a greedy rule.

Functions that would blow up refuse rather than hang: `schedule_exact` rejects
more than 14 jobs with an error naming the heuristic to use instead.

## Two ways to pick the wrong function

Both are silent, so they are worth reading before you start:

- **`assortment_*` vs `assortment_recapture_*`** take identical arguments and
  model opposite economics — products cannibalising each other, versus demand
  flowing to survivors when you delist. Using the wrong one is worse than not
  optimizing at all.
- **`sequence_*` vs `schedule_*`** both order jobs, but only `schedule_*`
  knows about due dates.

See [Choosing a Family](docs/guides/02-choosing-a-family.md).

## Documentation

**Guides** — how to use it:
[Getting Started](docs/guides/01-getting-started.md) ·
[Choosing a Family](docs/guides/02-choosing-a-family.md) ·
[Working With Your Tables](docs/guides/03-working-with-your-tables.md) ·
[Validating Results](docs/guides/04-validating-results.md)

**Theory** — how it works behind the curtain:
[How It Works](docs/theory/01-how-it-works.md) ·
[Packing and Knapsack](docs/theory/02-packing-and-knapsack.md) ·
[Sequencing and Scheduling](docs/theory/03-sequencing-and-scheduling.md) ·
[Selection](docs/theory/04-selection.md) ·
[Complexity and Limits](docs/theory/05-complexity-and-limits.md)

**Reference** — [API_REFERENCE.md](docs/API_REFERENCE.md), generated from
`duckdb_functions()` of the built extension so it cannot drift from the code.

## Building from source

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-optimize
cd anofox-optimize
make release        # or: make debug
make test
```

Pure C++ — no Rust toolchain required. Built against DuckDB v1.4.5 LTS and
v1.5.5.

## Testing

The suite asserts against optima computed **outside** this extension —
Falkenauer triplets whose optimum is fixed by construction, subset-sum solved
by DP, Held-Karp scheduling, and portfolio enumeration over all 15,504
subsets. A test that only checks self-consistency passes on a broken solver.

That standard has already paid for itself: it caught a local search returning
168 against a proven optimum of 60, and an "exact" scheduler that was not
exact.

```bash
make test    # 111 assertions across 10 test cases
```

## Telemetry

Anonymous usage telemetry, opt-out. See [TELEMETRY.md](TELEMETRY.md).

## License

BSL 1.1 — see [LICENSE](LICENSE).

## Related

- [anofox-inventory](https://github.com/DataZooDE/anofox-inventory) —
  replenishment and inventory policy (EOQ, safety stock, lot-sizing). Composes
  with this extension; no name collisions.
- [anofox-forecast](https://github.com/DataZooDE/anofox-forecast) — time
  series forecasting.
- [anofox-statistics](https://github.com/DataZooDE/anofox-statistics) —
  statistical tests and models.
