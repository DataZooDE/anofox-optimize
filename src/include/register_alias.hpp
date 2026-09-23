#pragma once

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

//! Register a scalar function, either as a canonical entry or as a TRUE
//! ALIAS of one. BOTH carry the full documentation.
//!
//! Every function here is exposed twice: `anofox_optimize_pack_best_of`
//! and the short `opt_pack_best_of`. `alias_of` empty means "this is the
//! canonical entry"; otherwise DuckDB is pointed at the canonical name, so
//! `duckdb_functions()` reports the relationship instead of two unrelated
//! functions that happen to behave alike.
//!
//! WHY THE ALIAS IS DOCUMENTED TOO
//!
//! It previously was not. The reason was real: anofox-evolve builds its
//! prompt vocabulary straight from `duckdb_functions()`, and repeating the
//! description on every short name cost 19,350 characters — roughly 5k
//! tokens — on EVERY prompt of every generation, forfeiting the implicit
//! cache discount on top of the tokens.
//!
//! But that is a defect in a consumer that reads the whole catalog at
//! once, and paying for it here made the catalog worse for every OTHER
//! consumer. The primary audience for this metadata is an agent that looks
//! up ONE function by name; handed `opt_pack_best_of`, it got nothing and
//! had to know to chase `alias_of` to find out what the function does.
//!
//! So the deduplication moved to where the duplication actually hurts:
//! `render_vocabulary` in anofox-evolve now renders an alias as a bare
//! signature tagged `[alias of ...]` and charges the description once.
//! That fix must be in place BEFORE this one, or evolve's prompts regress
//! by ~19k characters; `vocabulary_alias_cost_tests` is the gate.
//! `parameter_names` and `categories` are per-FAMILY, not per-function: every
//! algorithm in a family shares one signature, so each caller passes them once
//! rather than 39 times. Names are taken from the ReadList()/exception strings in
//! the implementations, which is what the errors already call these arguments --
//! so a caller who hits "processing_times must be >= 0" can find the argument the
//! message is talking about.
//!
//! No parameter_types: all overloads within a family are identical, so a single
//! description matches every one of them, and supplying types here would only
//! create a way for the match to fail silently.
inline void RegisterScalarOrAlias(ExtensionLoader &loader, ScalarFunction function,
                                  const string &description, const string &example,
                                  const string &alias_of, vector<string> parameter_names = {},
                                  vector<string> categories = {}) {
	CreateScalarFunctionInfo info(std::move(function));
	FunctionDescription desc;
	desc.description = description;
	desc.examples.push_back(example);
	desc.parameter_names = std::move(parameter_names);
	desc.categories = std::move(categories);
	info.descriptions.push_back(std::move(desc));
	if (!alias_of.empty()) {
		info.alias_of = alias_of;
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	}
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
