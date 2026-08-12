#define DUCKDB_EXTENSION_MAIN

#include "anofox_optimize_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "packing.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterPackingFunctions(loader);
}

void AnofoxOptimizeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AnofoxOptimizeExtension::Name() {
	return "anofox_optimize";
}

std::string AnofoxOptimizeExtension::Version() const {
	return "0.1.0";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_optimize, loader) {
	duckdb::LoadInternal(loader);
}
}
