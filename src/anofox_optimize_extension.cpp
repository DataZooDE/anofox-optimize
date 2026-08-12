#define DUCKDB_EXTENSION_MAIN

#include "anofox_optimize_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "packing.hpp"
#include "anofox_optimize_banner.hpp"
#include "version.hpp"

// Deliberately outside namespace duckdb: the banner library is
// DuckDB-agnostic and the guard macro refers to this object from every
// guarded source file. Same shape as anofox-inventory.
const datazoo::BannerInfo ANOFOX_OPTIMIZE_BANNER {
    "anofox_optimize", "0.1.0", "https://github.com/DataZooDE/anofox-optimize"};

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterPackingFunctions(loader);
	RegisterVersionFunction(loader);

	datazoo::RegisterBannerOption(loader);
	// Last, so a load that fails earlier never advertises itself. Silent
	// unless stderr is a terminal and the ~/.duckdb stamp is over a day
	// old. Matches anofox-inventory.
	datazoo::ShowBanner(ANOFOX_OPTIMIZE_BANNER);
}

void AnofoxOptimizeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AnofoxOptimizeExtension::Name() {
	return "anofox_optimize";
}

std::string AnofoxOptimizeExtension::Version() const {
	return AnofoxOptimizeVersion();
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_optimize, loader) {
	duckdb::LoadInternal(loader);
}
}
