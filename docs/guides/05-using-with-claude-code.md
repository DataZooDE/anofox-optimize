# Using With Claude Code

> An installable Claude Code plugin teaches an AI assistant to pick the right
> family, shape your tables, run the solvers and check the answer.

**Use this guide to:**
- Install the plugin
- Know what each skill covers and when it loads
- Understand where the skills get their facts, and report one that is wrong

## Install

In Claude Code:

```text
/plugin marketplace add DataZooDE/anofox-optimize
/plugin install anofox-optimize@anofox-optimize
```

Working inside a clone of this repository, load the skills straight from the
tree instead:

```bash
claude --plugin-dir ./plugins/anofox-optimize
```

The skills load on their own when a conversation needs them. They are not
slash commands, so you describe the problem and do not name a skill.

## The skills follow the work, not the function list

The function list is in `duckdb_functions()` already; an assistant connected to
DuckDB can read it. What the catalog cannot hold is the workflow: which family
a business problem is, how a table becomes the solver's lists, how to tell a
good answer from a plausible one. The skills are cut along that workflow:

| Skill | Loads when you… | Covers |
|---|---|---|
| `anofox-optimize-formulate` | describe a decision in business terms ("which products stay on a 5-slot shelf") | decision shape → family and member; cannibalisation vs recapture; sequencing vs scheduling; what is out of scope |
| `anofox-optimize-data-prep` | prepare changeover, substitution or covariance data, or results land on the wrong rows | ordered lists, 1-based indices, `opt_matrix_from_triples`, the (n+1)² schedule matrix, pairs tables, rejected inputs |
| `anofox-optimize-solve` | want a decision computed over a table, per group, by several algorithms | the `*_by` long forms, `algorithms :=`, `group_col :=`, scalar solvers, joining back |
| `anofox-optimize-evaluate` | ask whether a result is any good or which algorithm to use | `*_objectives_by`, feasibility checks, bounds, the exact and the weak members, tests that can fail |
| `anofox-optimize-visualize` | want to see or report the decision | `anofox_plot` from anofox-visualization, the contract schemas, `row_order`, discovery tags |

Try a prompt that names no function, for example *"we have to cut the shelf
to 5 products and most of a dropped product's buyers switch to a similar one;
which 5?"* The formulate skill should steer to `assortment_recapture_*`, the
family whose model matches that sentence, rather than `assortment_*`.

## Where the skills get their facts

The skills repeat what this repository already says, in workflow order: the
function descriptions in `duckdb_functions()` (the same text anofox-evolve
builds its prompt vocabulary from), these guides and the theory pages. They
must not claim more than the code does; a skill that overstates an algorithm
misleads an assistant exactly as an overclaiming description misleads a search.

`make check-docs` (and the CI job of the same name) keeps them honest:

- every ```sql example in the skills, this documentation and the README runs
  against the built extension;
- every function name they mention is registered;
- every function is covered by at least one skill;
- the plugin and every skill carry the version of the newest release.

## Report a wrong skill

If a skill steers to the wrong family, uses a stale argument or overstates
a guarantee, open an issue at
[DataZooDE/anofox-optimize](https://github.com/DataZooDE/anofox-optimize/issues)
with the prompt you used and what the assistant did. The fix lands in the
skill and, where the skill only repeated it, in the description or guide it
came from.
