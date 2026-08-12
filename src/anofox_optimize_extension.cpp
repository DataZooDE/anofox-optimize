#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "packing.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterPackingFunctions(loader);
}

class AnofoxOptimizeExtension : public Extension {
public:
	std::string Name() override {
		return "anofox_optimize";
	}
	std::string Version() const override {
		return "0.1.0";
	}
};

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_optimize, loader) {
	duckdb::LoadInternal(loader);
}
}
