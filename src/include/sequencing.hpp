#pragma once
#include "duckdb.hpp"

namespace duckdb {
// Sequencing with sequence-dependent setup costs: order n jobs to
// minimise total setup. Every member takes (setup DOUBLE[], n BIGINT)
// where `setup` is the n*n matrix flattened ROW-MAJOR (setup[i*n+j] is
// the cost of running j immediately after i), and returns the same
// STRUCT — so swapping one identifier swaps the METHOD and nothing else.
//
// This is the decision P6 makes.
void RegisterSequencingFunctions(ExtensionLoader &loader);
} // namespace duckdb
