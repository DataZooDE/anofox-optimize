cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(AnofoxOptimizeExtension)
include_directories(src/include)

set(EXTENSION_SOURCES
    src/anofox_optimize_extension.cpp
    src/packing.cpp
    src/version.cpp
)

# Feedback banner (header-only). Telemetry stays OFF: like anofox-inventory,
# this extension does not link posthog-telemetry, so there is no telemetry.hpp
# to include and no instance to report a banner_shown event through.
set(DATAZOO_BANNER_TELEMETRY OFF CACHE BOOL "" FORCE)
add_subdirectory(datazoo-banner)
# On the global include path: the extension targets come from DuckDB's macros
# and do not reliably pick up an INTERFACE include from target_link_libraries.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/datazoo-banner/include)

build_static_extension(anofox_optimize ${EXTENSION_SOURCES})
build_loadable_extension(anofox_optimize " " ${EXTENSION_SOURCES})

target_link_libraries(anofox_optimize_extension duckdb_static)
target_link_libraries(anofox_optimize_loadable_extension duckdb_static)

install(
  TARGETS anofox_optimize_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
