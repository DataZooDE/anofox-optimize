# Contributing to anofox-optimize

Read `AGENTS.md` first — it records the rules that shape this library.

## Adding an algorithm

1. **Match the family's signature exactly.** The point of this extension
   is that swapping one identifier swaps the method and nothing else. A
   member with a different signature cannot be reached by a text edit.
2. **Show it adds spread.** If it returns the same answer as an existing
   member on the instances that matter, it is decorative. Add a test on an
   instance where it differs, and say in the description when it does not
   help (e.g. `local_search` does not beat greedy on triplets).
3. **Add it to the feasibility table** in `test/sql/packing.test`. Every
   algorithm must satisfy every invariant: one assignment per item, bin
   ids in range, no bin over capacity, never below the lower bound. A
   member that quietly returns an infeasible answer does not merely fail —
   an optimising search will select it.
4. **Describe what the code does, not what you wish it did.**
   `duckdb_functions()` is the single source of truth, and anofox-evolve
   derives its prompt vocabulary from it. An overstated description
   poisons the search this library exists to help.
5. Regenerate docs: `./scripts/generate_api_reference.sh`.

## Building

    make release -j$(nproc)
    ./build/release/test/unittest --test-dir . "test/sql/*.test"
