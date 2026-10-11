set(LLVM_LINK_COMPONENTS
  Support
)

# PseudoModuleTests drives real ClangdServer / ClangdLSPServer instances and
# relies on the same test scaffolding clangd's own unit tests use. Those
# helpers are not part of any library, so compile them from clangd's
# unittests directory alongside the test.
set(CLANGD_UNITTEST_DIR "${CLANGD_INCLUDE_DIRS}/unittests")
set(CLANGD_TEST_SUPPORT_SOURCES
  "${CLANGD_UNITTEST_DIR}/Annotations.cpp"
  "${CLANGD_UNITTEST_DIR}/LSPClient.cpp"
  "${CLANGD_UNITTEST_DIR}/SyncAPI.cpp"
  "${CLANGD_UNITTEST_DIR}/TestFS.cpp"
  "${CLANGD_UNITTEST_DIR}/TestIndex.cpp"
  "${CLANGD_UNITTEST_DIR}/TestTU.cpp"
)

# Feature.h includes Features.inc, which clangd's build generates from
# Features.inc.in. Reuse the one from the LLVM build tree when present;
# otherwise configure it here with clangd's defaults. The values only feed
# feature strings in compiled clangd sources, so they do not affect the ABI
# of the headers this test includes.
set(CLANGD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/clangd-generated")
set(CLANGD_FEATURES_INC "")
foreach(candidate
    "${LLVM_BINARY_DIR}/tools/clang/tools/extra/clangd/Features.inc"
    "${LLVM_BINARY_DIR}/tools/extra/clangd/Features.inc")
  if(LLVM_BINARY_DIR AND EXISTS "${candidate}")
    set(CLANGD_FEATURES_INC "${candidate}")
    break()
  endif()
endforeach()

# Prerequisites beyond those of the other tests. When one is missing, skip
# this target only: ClangPseudoTests and the lit suite do not need clangd.
set(PSEUDO_MODULE_TESTS_SKIP_REASON "")
if(NOT TARGET clangDaemon)
  set(PSEUDO_MODULE_TESTS_SKIP_REASON
    "the LLVM package does not export the clangDaemon target (an LLVM build tree that built clangd does)")
elseif(NOT CLANGD_FEATURES_INC AND NOT EXISTS "${CLANGD_INCLUDE_DIRS}/Features.inc.in")
  set(PSEUDO_MODULE_TESTS_SKIP_REASON
    "no Features.inc in the LLVM build tree and no Features.inc.in under CLANGD_INCLUDE_DIRS")
else()
  foreach(src ${CLANGD_TEST_SUPPORT_SOURCES})
    if(NOT EXISTS "${src}")
      set(PSEUDO_MODULE_TESTS_SKIP_REASON
        "clangd test support source not found: ${src} (CLANGD_INCLUDE_DIRS must be a full clang-tools-extra/clangd checkout)")
      break()
    endif()
  endforeach()
endif()
if(PSEUDO_MODULE_TESTS_SKIP_REASON)
  message(STATUS "Skipping PseudoModuleTests: ${PSEUDO_MODULE_TESTS_SKIP_REASON}")
  return()
endif()

if(CLANGD_FEATURES_INC)
  get_filename_component(CLANGD_GENERATED_DIR "${CLANGD_FEATURES_INC}" DIRECTORY)
else()
  if(APPLE)
    set(CLANGD_BUILD_XPC 1)
  else()
    set(CLANGD_BUILD_XPC 0)
  endif()
  set(CLANGD_ENABLE_REMOTE 0)
  set(ENABLE_GRPC_REFLECTION 0)
  set(CLANGD_MALLOC_TRIM 1)
  set(CLANGD_TIDY_CHECKS 1)
  set(CLANGD_DECISION_FOREST 1)
  configure_file("${CLANGD_INCLUDE_DIRS}/Features.inc.in"
                 "${CLANGD_GENERATED_DIR}/Features.inc")
endif()

add_unittest(ClangPseudoUnitTests PseudoModuleTests
  PseudoModuleTests.cpp
  ${CLANGD_TEST_SUPPORT_SOURCES}
)

target_include_directories(PseudoModuleTests PRIVATE
  "${CLANGD_UNITTEST_DIR}"
  "${CLANGD_GENERATED_DIR}"
)

target_link_libraries(PseudoModuleTests
  PRIVATE
  obj.clangd_pseudo
  Pseudo
  PseudoCXX
  PseudoGrammar
  LLVMTestingAnnotations
  LLVMTestingSupport
  clangDaemon
  clangdSupport
)

# The clang libraries must follow the Pseudo libraries on the link line:
# libPseudo.a uses clang::Lexer, and GNU ld resolves static archives in a
# single pass (see tool/CMakeLists.txt for the same ordering).
clang_target_link_libraries(PseudoModuleTests
  PRIVATE
  clangAST
  clangBasic
  clangFrontend
  clangIndex
  clangLex
  clangSema
  clangSerialization
  clangTooling
  clangToolingCore
)

# Some tests parse real files from the llvm-project tree.
if(PSEUDO_LLVM_SOURCE_DIR)
  target_compile_definitions(PseudoModuleTests PRIVATE
    PSEUDO_LLVM_SOURCE_DIR="${PSEUDO_LLVM_SOURCE_DIR}")
endif()
