cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(AnofoxOptimizeExtension)
include_directories(src/include)

set(EXTENSION_SOURCES
    src/anofox_optimize_extension.cpp
    src/packing.cpp
)

build_static_extension(anofox_optimize ${EXTENSION_SOURCES})
build_loadable_extension(anofox_optimize " " ${EXTENSION_SOURCES})

target_link_libraries(anofox_optimize_extension duckdb_static)
target_link_libraries(anofox_optimize_loadable_extension duckdb_static)

install(
  TARGETS anofox_optimize_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
