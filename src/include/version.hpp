#pragma once
#include "duckdb.hpp"

#define ANOFOX_OPTIMIZE_VERSION "0.1.0"

namespace duckdb {
void RegisterVersionFunction(ExtensionLoader &loader);
} // namespace duckdb
