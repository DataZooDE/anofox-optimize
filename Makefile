PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXT_NAME=anofox_optimize
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# anofox-visualization picks up the contract producers (docs/ANOFOX_CONTRACT_PLAN.md):
# the release build next to a released anofox_visualization in the official DuckDB CLI.
test_visualization:
	python3 scripts/visualization_pickup.py build/release/extension/anofox_optimize/anofox_optimize.duckdb_extension
