#pragma once
#include "duckdb.hpp"

namespace duckdb {
// Bin-packing decision algorithms. Every function in the family takes
// (sizes DOUBLE[], capacity DOUBLE) and returns the same STRUCT, so
// swapping one identifier swaps the METHOD and nothing else — which is
// the whole point of exposing them.
void RegisterPackingFunctions(ExtensionLoader &loader);
} // namespace duckdb
