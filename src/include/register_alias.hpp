#pragma once

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

//! Register a scalar function, either as a canonical entry carrying its
//! documentation or as a TRUE ALIAS of one.
//!
//! Every function here is exposed twice: `anofox_optimize_pack_best_of`
//! and the short `opt_pack_best_of`. Both were previously registered as
//! independent functions, each carrying a full copy of the description —
//! which is how anofox-statistics (107 `alias_of` registrations) and
//! anofox-forecast (38) do NOT do it.
//!
//! The duplication was not cosmetic. anofox-evolve builds its prompt
//! vocabulary straight from `duckdb_functions()` without deduplicating,
//! so the copies cost 19,350 characters — roughly 5k tokens — on EVERY
//! prompt of every generation. An alias carries no description of its
//! own, which removes that entirely while leaving both names callable.
//!
//! `alias_of` empty means "this is the canonical entry": attach the
//! description and example. Otherwise attach nothing and point DuckDB at
//! the canonical name, so `duckdb_functions()` reports the relationship
//! instead of two unrelated functions that happen to behave alike.
inline void RegisterScalarOrAlias(ExtensionLoader &loader, ScalarFunction function,
                                  const string &description, const string &example,
                                  const string &alias_of) {
	CreateScalarFunctionInfo info(std::move(function));
	if (alias_of.empty()) {
		FunctionDescription desc;
		desc.description = description;
		desc.examples.push_back(example);
		info.descriptions.push_back(std::move(desc));
	} else {
		info.alias_of = alias_of;
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	}
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
