cmake_minimum_required(VERSION 3.20)
project(pseudo-parser C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET LibEdit::LibEdit)
  add_library(LibEdit::LibEdit INTERFACE IMPORTED)
endif()

if(NOT TARGET clang-resource-headers)
  add_custom_target(clang-resource-headers)
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

  include_directories(SYSTEM
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
  )
  link_directories(
    ${LLVM_LIBRARY_DIRS}
  )
  add_definitions(${LLVM_DEFINITIONS})
endif()

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
endif()

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

option(PSEUDO_ENABLE_TESTS "Build pseudo-parser tests" ON)
if(PSEUDO_ENABLE_TESTS)
  add_subdirectory(unittests)
endif()
