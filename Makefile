PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXT_NAME=anofox_optimize
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# anofox-visualization picks up the contract producers (docs/ANOFOX_CONTRACT_PLAN.md):
# the release build next to a released anofox_visualization in the official DuckDB CLI.
test_visualization:
	python3 scripts/visualization_pickup.py build/release/extension/anofox_optimize/anofox_optimize.duckdb_extension

.PHONY: audit test_wasm

# Build-free checks (#9): the wasm duckdb_signature section walker's self-test,
# which scripts/extension-upload.sh relies on to guard every wasm upload, and
# the wasm harness comparator's (node builtins only, no npm install needed).
audit:
	python3 scripts/wasm_signature_sections.py --self-test
	node test/wasm/sqllogic.mjs --self-test

# The sqllogictest suite in DuckDB-Wasm under Node (test/wasm/README.md), against
# a wasm_eh build (`make wasm_eh` with emsdk 3.1.71, the extension-ci-tools pin).
test_wasm:
	npm --prefix test/wasm ci --ignore-scripts
	node test/wasm/run.mjs --ext build/wasm_eh/extension/anofox_optimize/anofox_optimize.duckdb_extension.wasm
