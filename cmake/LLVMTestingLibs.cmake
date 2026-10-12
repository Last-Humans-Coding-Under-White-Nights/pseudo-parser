# Provide the gtest and LLVM testing libraries that unit tests link against.
#
# LLVM only exports llvm_gtest, llvm_gtest_main, LLVMTestingSupport and
# LLVMTestingAnnotations from a build tree configured with
# LLVM_INCLUDE_TESTS=ON. Installed LLVM packages (distro, Homebrew) and the
# CI build of clangd do not ship them. When they are missing, build them from
# an llvm-project source tree instead, using LLVM's own CMake files so the
# result matches what LLVM would have produced.
#
# Requires PSEUDO_LLVM_SOURCE_DIR to point at an llvm-project checkout that
# matches the LLVM version found by find_package(LLVM).

function(pseudo_require_llvm_testing_libs)
  set(missing "")
  foreach(target llvm_gtest llvm_gtest_main LLVMTestingSupport LLVMTestingAnnotations)
    if(NOT TARGET ${target})
      list(APPEND missing ${target})
    endif()
  endforeach()
  if(NOT missing)
    return()
  endif()

  string(REPLACE ";" ", " missing_list "${missing}")
  if(NOT PSEUDO_LLVM_SOURCE_DIR OR NOT EXISTS "${PSEUDO_LLVM_SOURCE_DIR}/third-party/unittest/CMakeLists.txt")
    message(FATAL_ERROR
      "Unit tests need ${missing_list}, which this LLVM package does not provide, "
      "and no llvm-project source tree was found to build them from.\n"
      "Set -DPSEUDO_LLVM_SOURCE_DIR=/path/to/llvm-project (must match LLVM "
      "${LLVM_PACKAGE_VERSION}), or pass -DPSEUDO_ENABLE_TESTS=OFF.")
  endif()

  message(STATUS "Building ${missing_list} from ${PSEUDO_LLVM_SOURCE_DIR}")

  # LLVM's gtest CMake file refers to the LLVM source and binary trees to
  # find headers; point them at the same tree the rest of the build uses.
  set(LLVM_SOURCE_DIR "${PSEUDO_LLVM_SOURCE_DIR}/llvm")
  set(LLVM_MAIN_INCLUDE_DIR "${PSEUDO_LLVM_SOURCE_DIR}/llvm/include")
  if(NOT LLVM_BINARY_DIR)
    set(LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}")
  endif()

  if(NOT TARGET llvm_gtest)
    add_subdirectory("${PSEUDO_LLVM_SOURCE_DIR}/third-party/unittest"
                     "${CMAKE_BINARY_DIR}/third-party/unittest")
  endif()
  if(NOT TARGET LLVMTestingAnnotations)
    add_subdirectory("${PSEUDO_LLVM_SOURCE_DIR}/llvm/lib/Testing/Annotations"
                     "${CMAKE_BINARY_DIR}/llvm/lib/Testing/Annotations")
  endif()
  if(NOT TARGET LLVMTestingSupport)
    add_subdirectory("${PSEUDO_LLVM_SOURCE_DIR}/llvm/lib/Testing/Support"
                     "${CMAKE_BINARY_DIR}/llvm/lib/Testing/Support")
  endif()

  # The llvm/Testing headers live in the source tree; installed packages may
  # or may not ship them, so make the source copy visible to consumers. Only
  # the targets built here can take usage requirements; an imported one from
  # the LLVM package already carries its include directories.
  foreach(target LLVMTestingSupport LLVMTestingAnnotations)
    if(target IN_LIST missing)
      target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${PSEUDO_LLVM_SOURCE_DIR}/llvm/include>)
    endif()
  endforeach()
endfunction()
