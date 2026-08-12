# anofox-optimize

Combinatorial **decision** algorithms as DuckDB functions.

Written because anofox-evolve's search could not cross algorithm families
on its combinatorial pilots: with only plain SQL available, a search on a
Falkenauer triplet bin-packing instance produced 15 textually distinct
candidates that all scored the same first-fit-decreasing result (12 bins
against an optimum of 10). The evolutionary premise — *a text edit is an
algorithm change* — needs the algorithms to exist as callable functions.

`anofox-inventory` already does this for replenishment policies, and with
it declared, 5 of 6 candidates called the extension and switched families
across generations. This extension supplies the same for packing,
assignment and selection.

Every function in a family shares an identical signature, so swapping one
identifier swaps the method and nothing else.
