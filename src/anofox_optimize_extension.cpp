#define DUCKDB_EXTENSION_MAIN

#include "anofox_optimize_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "packing.hpp"
#include "version.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterPackingFunctions(loader);
	RegisterVersionFunction(loader);
}

void AnofoxOptimizeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AnofoxOptimizeExtension::Name() {
	return "anofox_optimize";
}

std::string AnofoxOptimizeExtension::Version() const {
	return ANOFOX_OPTIMIZE_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_optimize, loader) {
	duckdb::LoadInternal(loader);
}
}
