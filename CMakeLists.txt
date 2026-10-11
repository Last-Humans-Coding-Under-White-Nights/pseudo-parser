cmake_minimum_required(VERSION 3.20)
project(pseudo-parser C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT TARGET LibEdit::LibEdit)
  add_library(LibEdit::LibEdit INTERFACE IMPORTED)
endif()

# Find LLVM and Clang if not built in-tree
if(NOT DEFINED LLVM_MAIN_SRC_DIR)
  find_package(LLVM REQUIRED CONFIG)
  find_package(Clang REQUIRED CONFIG)

  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}" "${CLANG_CMAKE_DIR}")
  include(AddLLVM)
  include(AddClang)
  include(TableGen)
  include(HandleLLVMOptions)

  if(NOT TARGET clang-resource-headers)
    add_custom_target(clang-resource-headers)
  endif()

  include_directories(SYSTEM
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
  )
  link_directories(
    ${LLVM_LIBRARY_DIRS}
  )
  add_definitions(${LLVM_DEFINITIONS})

  # Standalone builds have no LLVM build tree to inherit output locations
  # from. Collect executables under bin/ and libraries under lib/ so the lit
  # configuration (CURRENT_TOOLS_DIR, SHLIBDIR) can find the tools it runs.
  set(LLVM_RUNTIME_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin")
  set(LLVM_LIBRARY_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib")
  set(LLVM_SHLIB_OUTPUT_INTDIR "${LLVM_LIBRARY_OUTPUT_INTDIR}")
endif()

if(MSVC)
  add_compile_options(/GR-)
else()
  add_compile_options(-fno-rtti)
endif()

add_definitions(-DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1)

# Find Clangd headers
if(NOT CLANGD_INCLUDE_DIRS)
  if(DEFINED ENV{CLANGD_INCLUDE_DIRS})
    set(CLANGD_INCLUDE_DIRS "$ENV{CLANGD_INCLUDE_DIRS}")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/clang-tools-extra/clangd")
    set(CLANGD_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/clang-tools-extra/clangd")
  elseif(EXISTS "${LLVM_MAIN_SRC_DIR}/../clang-tools-extra/clangd")
    set(CLANGD_INCLUDE_DIRS "${LLVM_MAIN_SRC_DIR}/../clang-tools-extra/clangd")
  elseif(EXISTS "${LLVM_EXTERNAL_CLANG_TOOLS_EXTRA_SOURCE_DIR}/clangd")
    set(CLANGD_INCLUDE_DIRS "${LLVM_EXTERNAL_CLANG_TOOLS_EXTRA_SOURCE_DIR}/clangd")
  endif()
endif()

if(NOT CLANGD_INCLUDE_DIRS)
  message(WARNING "CLANGD_INCLUDE_DIRS was not found. Please specify -DCLANGD_INCLUDE_DIRS=/path/to/clang-tools-extra/clangd if building clangd_pseudo.")
else()
  # Normalize to a CMake-style path. A native Windows path with backslashes
  # breaks LLVM's macro-based helpers (add_unittest, add_llvm_executable),
  # which re-parse their arguments and treat "\a" as an escape sequence.
  get_filename_component(CLANGD_INCLUDE_DIRS "${CLANGD_INCLUDE_DIRS}" ABSOLUTE)
endif()

# Locate an llvm-project source tree. Tests need it for gtest, the LLVM
# testing libraries, lit, and clangd's test support sources.
set(PSEUDO_LLVM_SOURCE_DIR "" CACHE PATH
  "Path to an llvm-project checkout matching the LLVM package in use (used to build test dependencies)")
if(NOT PSEUDO_LLVM_SOURCE_DIR)
  if(DEFINED ENV{PSEUDO_LLVM_SOURCE_DIR})
    set(PSEUDO_LLVM_SOURCE_DIR "$ENV{PSEUDO_LLVM_SOURCE_DIR}")
  elseif(CLANGD_INCLUDE_DIRS AND EXISTS "${CLANGD_INCLUDE_DIRS}/../../llvm/utils/lit/lit.py")
    get_filename_component(PSEUDO_LLVM_SOURCE_DIR "${CLANGD_INCLUDE_DIRS}/../.." ABSOLUTE)
  elseif(LLVM_BUILD_MAIN_SRC_DIR AND EXISTS "${LLVM_BUILD_MAIN_SRC_DIR}/../third-party/unittest")
    get_filename_component(PSEUDO_LLVM_SOURCE_DIR "${LLVM_BUILD_MAIN_SRC_DIR}/.." ABSOLUTE)
  endif()
endif()
if(PSEUDO_LLVM_SOURCE_DIR)
  # Normalize user-supplied values too: the path ends up in a C++ string
  # literal, where native Windows backslashes would become escape sequences.
  get_filename_component(PSEUDO_LLVM_SOURCE_DIR "${PSEUDO_LLVM_SOURCE_DIR}" ABSOLUTE)
endif()

# Python drives the resource bundler in tool/ and lit in test/. Find it once
# here: find_package() results are directory-scoped, and add_lit_target builds
# its command from Python3_EXECUTABLE, so a lookup inside tool/ would leave
# test/ invoking lit.py bare. Unix runs that through the shebang; Windows
# hands it to the .py file association, which runs detached and reports
# success without executing anything visible.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

include_directories(BEFORE
  include
  ${CMAKE_CURRENT_BINARY_DIR}/include
)

add_subdirectory(lib/grammar)
add_subdirectory(gen)
add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory(tool)

if(CLANGD_INCLUDE_DIRS)
  add_subdirectory(clangd)
endif()

# Tests default to ON when their dependencies can be satisfied, so a plain
# configure against an LLVM package without testing libraries and without an
# llvm-project tree still builds the plugin. Once tests are ON, a missing
# dependency is a configure error (see cmake/LLVMTestingLibs.cmake).
# PseudoModuleTests has extra prerequisites (clangDaemon, clangd's test
# scaffolding) and is skipped with a status message when they are absent, so
# ClangPseudoTests and the lit suite still build; CI builds that target
# explicitly so a skip cannot pass unnoticed there.
if(TARGET LLVMTestingSupport
   OR (PSEUDO_LLVM_SOURCE_DIR AND EXISTS "${PSEUDO_LLVM_SOURCE_DIR}/third-party/unittest/CMakeLists.txt"))
  set(PSEUDO_ENABLE_TESTS_DEFAULT ON)
else()
  set(PSEUDO_ENABLE_TESTS_DEFAULT OFF)
  message(STATUS "Test dependencies not found; PSEUDO_ENABLE_TESTS defaults to OFF. "
    "Set PSEUDO_LLVM_SOURCE_DIR to an llvm-project checkout to enable them.")
endif()
option(PSEUDO_ENABLE_TESTS "Build pseudo-parser tests" ${PSEUDO_ENABLE_TESTS_DEFAULT})
option(PSEUDO_ENABLE_FUZZER "Build clang-pseudo-fuzzer even when tests are off (the lit suite always builds it)" OFF)
option(PSEUDO_ENABLE_BENCHMARKS "Build pseudo-parser benchmarks (needs LLVM's google benchmark target)" OFF)

if(PSEUDO_ENABLE_TESTS OR PSEUDO_ENABLE_FUZZER)
  add_subdirectory(fuzzer)
endif()

if(PSEUDO_ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(PSEUDO_ENABLE_TESTS)
  add_subdirectory(unittests)
  add_subdirectory(test)
endif()
