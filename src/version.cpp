#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "version.hpp"

#include "register_alias.hpp"

namespace duckdb {

//! House convention (cf. `anofox_bayes_version`): every anofox extension
//! reports its own version, so a session can tell which build it loaded
//! without inspecting the binary.
const char *AnofoxOptimizeVersion() {
#ifdef EXT_VERSION_ANOFOX_OPTIMIZE
	return EXT_VERSION_ANOFOX_OPTIMIZE;
#else
	return ANOFOX_OPTIMIZE_FALLBACK_VERSION;
#endif
}

static void VersionFunction(DataChunk &, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddString(result, AnofoxOptimizeVersion());
}

void RegisterVersionFunction(ExtensionLoader &loader) {
	const string description =
	    "Returns the version of the loaded anofox_optimize extension, as stamped by "
	    "the build.";
	// Every other function in this extension is reachable by both its
	// canonical name and a short `opt_` one; `version` was the single
	// exception, which made "every function has a short form" untrue.
	RegisterScalarOrAlias(
	    loader, ScalarFunction("anofox_optimize_version", {}, LogicalType::VARCHAR, VersionFunction),
	    description, "anofox_optimize_version()", "");
	RegisterScalarOrAlias(loader,
	                      ScalarFunction("opt_version", {}, LogicalType::VARCHAR, VersionFunction),
	                      description, "", "anofox_optimize_version");
}

} // namespace duckdb
