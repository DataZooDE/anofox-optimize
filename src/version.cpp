#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "version.hpp"

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
	ScalarFunction version("anofox_optimize_version", {}, LogicalType::VARCHAR, VersionFunction);
	FunctionDescription desc;
	desc.description =
	    "Returns the version of the loaded anofox_optimize extension, as stamped by "
	    "the build.";
	desc.examples.push_back("anofox_optimize_version()");
	CreateScalarFunctionInfo info(version);
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
