cmake_minimum_required(VERSION 3.28)
project(abistudy VERSION 3.0.0 LANGUAGES CXX
        DESCRIPTION "Measures which kinds of ABI change occur between consecutive library releases")

# ---------------------------------------------------------------------------
# Language level. C++26 is a hard requirement: the code relies on
# `= delete("reason")`, structured bindings in conditions, std::expected,
# std::print and std::ranges::views::enumerate. No extensions.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++ modules are used; scanning would pass GCC-only flags to clang-tidy.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# Static analysis. -DABISTUDY_TIDY=ON runs clang-tidy (rules in .clang-tidy)
# on every translation unit as part of the build; CI builds with it on.
# `cmake --build build --target format-check` verifies clang-format compliance.
# ---------------------------------------------------------------------------
option(ABISTUDY_TIDY "Run clang-tidy during the build" OFF)
if(ABISTUDY_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-21 REQUIRED)
  # clang parses `#embed` in C++26 mode but still labels it a C23 extension.
  set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--quiet;--extra-arg=-Wno-c23-extensions")
endif()
find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-21)
file(GLOB_RECURSE _abistudy_sources CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_SOURCE_DIR}/tests/*.cpp" "${CMAKE_SOURCE_DIR}/tests/*.hpp")
if(CLANG_FORMAT_EXE)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${_abistudy_sources}
    COMMENT "clang-format: rewriting sources in place")
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${_abistudy_sources}
    COMMENT "clang-format: verifying sources")
endif()

add_library(abistudy_warnings INTERFACE)
target_compile_options(abistudy_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
  -Wnull-dereference -Wimplicit-fallthrough
  $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
target_link_options(abistudy_warnings INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# ---------------------------------------------------------------------------
# Third-party libraries. Everything is linked as a library and called
# in-process; the pipeline never parses another tool's text output.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json 3.11 REQUIRED)
# nlohmann's header is third-party code; report its diagnostics as system-header noise, not ours.
get_target_property(_nlohmann_inc nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nlohmann_inc}")
pkg_check_modules(ABIGAIL REQUIRED IMPORTED_TARGET libabigail)
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)

# libclang ships no pkg-config file on Debian; locate the C API by hand.
file(GLOB _llvm_roots LIST_DIRECTORIES true "/usr/lib/llvm-*")
list(SORT _llvm_roots COMPARE NATURAL ORDER DESCENDING)
find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h
          HINTS ${_llvm_roots} PATH_SUFFIXES include REQUIRED)
find_library(CLANG_C_LIBRARY NAMES clang libclang
             HINTS ${_llvm_roots} PATH_SUFFIXES lib REQUIRED)
add_library(clang_c INTERFACE)
target_include_directories(clang_c INTERFACE ${CLANG_C_INCLUDE_DIR})
target_link_libraries(clang_c INTERFACE ${CLANG_C_LIBRARY})
message(STATUS "libclang C API: ${CLANG_C_LIBRARY} (${CLANG_C_INCLUDE_DIR})")

# ---------------------------------------------------------------------------
# Library: everything except main(), so the tests link the real code.
# ---------------------------------------------------------------------------
add_library(abistudy_lib STATIC
  src/core/version.cpp
  src/core/fs.cpp
  src/core/process.cpp
  src/core/hash.cpp
  src/core/artifact.cpp
  src/snapshot/client.cpp
  src/snapshot/api.cpp
  src/snapshot/archive_index.cpp
  src/deb/extract.cpp
  src/abi/model.cpp
  src/abi/compare.cpp
  src/hdr/index.cpp
  src/pipeline/artifacts.cpp
  src/pipeline/materialize.cpp
  src/pipeline/select.cpp
  src/pipeline/resolve.cpp
  src/pipeline/diff.cpp
  src/pipeline/headers.cpp
  src/pipeline/analyze.cpp
  src/pipeline/report.cpp
)
target_include_directories(abistudy_lib PUBLIC src)
# `#embed "template.html"` in report.cpp resolves through the embed search path.
set_source_files_properties(src/pipeline/report.cpp PROPERTIES
  COMPILE_OPTIONS "--embed-dir=${CMAKE_SOURCE_DIR}/report"
  OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/report/template.html")
target_link_libraries(abistudy_lib
  PUBLIC  nlohmann_json::nlohmann_json PkgConfig::ABIGAIL clang_c
  PRIVATE CURL::libcurl PkgConfig::ARCHIVE OpenSSL::Crypto abistudy_warnings)

add_executable(abistudy src/cli/main.cpp)
target_link_libraries(abistudy PRIVATE abistudy_lib abistudy_warnings)

# ---------------------------------------------------------------------------
# Tests. The calibration suite is the correctness gate: thirty synthetic
# libraries with exactly one known change each, compiled with the host g++
# and pushed through the real diff and header stages.
# ---------------------------------------------------------------------------
enable_testing()
foreach(t version strong classify extract)
  add_executable(${t}_test tests/unit/${t}_test.cpp)
  target_link_libraries(${t}_test PRIVATE abistudy_lib abistudy_warnings)
  add_test(NAME unit.${t} COMMAND ${t}_test)
endforeach()
target_link_libraries(extract_test PRIVATE PkgConfig::ARCHIVE)

add_executable(abistudy_calibration tests/calibration/calibration_test.cpp)
target_link_libraries(abistudy_calibration PRIVATE abistudy_lib abistudy_warnings)
add_test(NAME calibration
         COMMAND abistudy_calibration ${CMAKE_SOURCE_DIR}/tests/calibration/cases)
set_tests_properties(calibration PROPERTIES TIMEOUT 900)

install(TARGETS abistudy RUNTIME DESTINATION bin)
