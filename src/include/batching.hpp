#pragma once
#include "duckdb.hpp"

namespace duckdb {
// Capacity-bounded batching that minimises the PRIORITY-WEIGHTED mean
// wave — P2's objective.
//
// This is not plain bin packing: packing minimises the NUMBER of bins,
// while P2 pays sum(wave_number * priority)/n, so WHICH wave an order
// lands in matters and a high-priority order belongs early. A packer
// that merely used fewest waves would optimise the wrong quantity, the
// same mismatch that made P6 ignore the setup-minimising family.
//
// (items, priorities, capacity) -> (wave per order, avg weighted wave).
void RegisterBatchingFunctions(ExtensionLoader &loader);
} // namespace duckdb
