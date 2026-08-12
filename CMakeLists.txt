cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(AnofoxOptimizeExtension)

# --- Telemetry (disabled on MinGW, where vcpkg cannot build OpenSSL) ---
# Same wiring as anofox-statistics/bayes/forecast. Schema-2 posthog lib.
if(NOT MINGW)
    find_package(OpenSSL REQUIRED)

    add_library(posthog_telemetry_obj OBJECT
        ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/src/telemetry.cpp
    )

    target_include_directories(posthog_telemetry_obj PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/include
        ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib
        ${OPENSSL_INCLUDE_DIR}
    )

    target_compile_features(posthog_telemetry_obj PUBLIC cxx_std_17)
    target_compile_definitions(posthog_telemetry_obj PRIVATE ANOFOX_TELEMETRY_ENABLED)
    set(ANOFOX_TELEMETRY_ENABLED ON)
else()
    message(STATUS "Telemetry disabled: OpenSSL not available on MinGW")
    set(ANOFOX_TELEMETRY_ENABLED OFF)
endif()

include_directories(src/include)
if(ANOFOX_TELEMETRY_ENABLED)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/include)
endif()

set(EXTENSION_SOURCES
    src/anofox_optimize_extension.cpp
    src/packing.cpp
    src/knapsack.cpp
    src/sequencing.cpp
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

if(ANOFOX_TELEMETRY_ENABLED)
    foreach(TGT anofox_optimize_extension anofox_optimize_loadable_extension)
        target_sources(${TGT} PRIVATE $<TARGET_OBJECTS:posthog_telemetry_obj>)
        target_compile_definitions(${TGT} PRIVATE ANOFOX_TELEMETRY_ENABLED)
        target_link_libraries(${TGT} OpenSSL::SSL OpenSSL::Crypto)
    endforeach()
endif()

install(
  TARGETS anofox_optimize_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
