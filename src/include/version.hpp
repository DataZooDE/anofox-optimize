#pragma once
#include "duckdb.hpp"

//! Fallback only. The real version comes from the BUILD
//! (`EXT_VERSION_ANOFOX_OPTIMIZE`, set by DuckDB's extension tooling from
//! the git describe of this repo), the same way anofox-statistics does
//! it. A hardcoded constant drifts from the artefact it labels, which
//! makes "which build is loaded?" unanswerable — the exact question a
//! version function exists to answer. (Codex review finding, Low.)
#ifndef ANOFOX_OPTIMIZE_FALLBACK_VERSION
#define ANOFOX_OPTIMIZE_FALLBACK_VERSION "0.1.0-dev"
#endif

namespace duckdb {
//! The build-provided version when available, else the fallback.
const char *AnofoxOptimizeVersion();
void RegisterVersionFunction(ExtensionLoader &loader);
} // namespace duckdb
