#pragma once
#include "duckdb.hpp"

namespace duckdb {
// Selection where the items INTERACT — the value of a set is not the sum
// of its parts.
//
// * assortment (P5): listing a product cannibalises demand from the
//   other listed products, so a top-N-by-margin rule overstates its own
//   result. Plain knapsack cannot express this: its objective is
//   separable and this one is quadratic.
// * portfolio (P7): choosing at most K assets and their weights to
//   maximise Sharpe, where risk depends on the covariance BETWEEN the
//   chosen assets.
void RegisterSelectionFunctions(ExtensionLoader &loader);
} // namespace duckdb
