# Included by DuckDB's CMake build system to load this extension.

# DuckDB v1.5.6 stamps the short commit SHA instead of the release tag: its duckdb_extension_generate_version
# passes --match '${VERSIONING_TAG_MATCH}' with literal single quotes, so no tag ever matches. Pass the version
# explicitly when HEAD is on a vX.Y.Z tag (2.0.x detects it correctly too; this keeps both lines identical).
set(ANOFOX_OPTIMIZE_EXT_VERSION "")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --match "v*.*.*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE ANOFOX_OPTIMIZE_GIT_RESULT
        OUTPUT_VARIABLE ANOFOX_OPTIMIZE_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ANOFOX_OPTIMIZE_GIT_RESULT EQUAL 0 AND ANOFOX_OPTIMIZE_GIT_DESCRIBE MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[a-zA-Z0-9\\.]+)?$")
        set(ANOFOX_OPTIMIZE_EXT_VERSION "${ANOFOX_OPTIMIZE_GIT_DESCRIBE}")
    endif()
endif()
if(ANOFOX_OPTIMIZE_EXT_VERSION STREQUAL "")
    duckdb_extension_load(anofox_optimize
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    )
else()
    message(STATUS "anofox_optimize extension version ${ANOFOX_OPTIMIZE_EXT_VERSION}")
    duckdb_extension_load(anofox_optimize
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        EXTENSION_VERSION ${ANOFOX_OPTIMIZE_EXT_VERSION}
    )
endif()
