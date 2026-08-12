#pragma once
#include "duckdb.hpp"

namespace duckdb {
// 0/1 knapsack: choose a subset of items maximising value subject to a
// weight capacity. Every member takes (values DOUBLE[], weights DOUBLE[],
// capacity DOUBLE) and returns the same STRUCT, so swapping one
// identifier swaps the METHOD and nothing else.
//
// This is the decision P3 makes (which shipments go on the capped cheap
// carrier) and the separable half of P5 (which products to list).
void RegisterKnapsackFunctions(ExtensionLoader &loader);
} // namespace duckdb
