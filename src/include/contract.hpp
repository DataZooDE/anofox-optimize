#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! The names of the functions already in the system catalog, taken before this
//! extension registers anything, so the tagging pass leaves them alone.
case_insensitive_set_t SnapshotSystemFunctionNames(ExtensionLoader &loader);

//! The anofox integration contract's producers (pack_by, wave_by, schedule_by
//! and their *_objectives_by) and anofox_contract_version().
void RegisterContractFunctions(ExtensionLoader &loader);

//! Tags every function this extension registered anofox.family = 'optimize',
//! and the producers anofox.contract / anofox.output. Call last.
void ApplyContractTags(ExtensionLoader &loader, const case_insensitive_set_t &preexisting);

} // namespace duckdb
