#pragma once
#include "duckdb.hpp"

namespace duckdb {
// Build the flattened ROW-MAJOR matrix the sequencing, scheduling,
// assortment and portfolio families expect, from the (from, to, value)
// TRIPLES the pilots actually store.
//
// Every matrix-shaped family takes DOUBLE[] of length n*n, while the
// pilot tables hold triples — so a policy must pivot them, in the right
// order, with the right zero fill. That is fiddly enough to be a reason
// not to call the function at all, which is the leading explanation for
// P7 declaring the vocabulary and using it 0 times while P5, whose
// matrix is 5x5 and sparse, used it 4 times in 5.
void RegisterMatrixFunctions(ExtensionLoader &loader);
} // namespace duckdb
