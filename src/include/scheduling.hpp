#pragma once
#include "duckdb.hpp"

namespace duckdb {
// Sequencing that minimises TOTAL WEIGHTED TARDINESS with
// sequence-dependent setups — P6's actual objective.
//
// The `opt_sequence_*` family minimises total SETUP, which is a
// different quantity: a minimal-setup order can be badly tardy, because
// tardiness depends on due dates and priorities those functions never
// see. Measured: with only the setup family available, 0 of 7 P6
// candidates called it. The model was right to ignore a function that
// optimises the wrong thing.
//
// Every member takes (processing, due, priority, setup, n) and returns
// the same STRUCT.
void RegisterSchedulingFunctions(ExtensionLoader &loader);
} // namespace duckdb
