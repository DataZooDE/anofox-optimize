#define DUCKDB_EXTENSION_MAIN

#include "anofox_optimize_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "batching.hpp"
#include "knapsack.hpp"
#include "selection.hpp"
#include "packing.hpp"
#include "scheduling.hpp"
#include "sequencing.hpp"
#include "anofox_optimize_banner.hpp"

#ifdef ANOFOX_TELEMETRY_ENABLED
#include "telemetry.hpp"
#endif
#include "version.hpp"

// Deliberately outside namespace duckdb: the banner library is
// DuckDB-agnostic and the guard macro refers to this object from every
// guarded source file. Same shape as anofox-inventory.
const datazoo::BannerInfo ANOFOX_OPTIMIZE_BANNER {
    "anofox_optimize", "0.1.0", "https://github.com/DataZooDE/anofox-optimize"};

namespace duckdb {

#ifdef ANOFOX_TELEMETRY_ENABLED

namespace {

void OnTelemetryEnabled(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_telemetry_enabled cannot be NULL");
	}
	PostHogTelemetry::Instance().SetEnabled(BooleanValue::Get(parameter));
}

void OnTelemetryKey(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_telemetry_key cannot be NULL");
	}
	PostHogTelemetry::Instance().SetAPIKey(StringValue::Get(parameter));
}

} // anonymous namespace

//! Same two options every anofox extension exposes, so one SET turns
//! telemetry off across all of them in a session.
static void RegisterTelemetryOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("anofox_telemetry_enabled",
	                          "Enable or disable anonymous usage telemetry",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true), OnTelemetryEnabled);
	config.AddExtensionOption("anofox_telemetry_key", "PostHog API key for telemetry",
	                          LogicalType::VARCHAR,
	                          Value("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t"),
	                          OnTelemetryKey);
}

#endif // ANOFOX_TELEMETRY_ENABLED

static void LoadInternal(ExtensionLoader &loader) {
#ifdef ANOFOX_TELEMETRY_ENABLED
	RegisterTelemetryOptions(loader);
#endif
	RegisterPackingFunctions(loader);
	RegisterKnapsackFunctions(loader);
	RegisterSequencingFunctions(loader);
	RegisterSchedulingFunctions(loader);
	RegisterBatchingFunctions(loader);
	RegisterSelectionFunctions(loader);
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
