set(CLANG_PSEUDO_TEST_DEPS
  pseudo-tool
  ClangPseudoTests
  )

foreach(dep clang-pseudo-fuzzer PseudoModuleTests FileCheck not count)
  if(TARGET ${dep})
    list(APPEND CLANG_PSEUDO_TEST_DEPS ${dep})
  endif()
endforeach()

# Run lit from the llvm-project source tree when the LLVM package does not
# install llvm-lit (installed packages never do).
if(NOT LLVM_EXTERNAL_LIT AND PSEUDO_LLVM_SOURCE_DIR
   AND EXISTS "${PSEUDO_LLVM_SOURCE_DIR}/llvm/utils/lit/lit.py")
  set(LLVM_EXTERNAL_LIT "${PSEUDO_LLVM_SOURCE_DIR}/llvm/utils/lit/lit.py"
      CACHE STRING "Command used to spawn lit" FORCE)
endif()

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
  )

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/Unit/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.cfg.py
  )

add_lit_testsuite(check-clang-pseudo "Running the clang-pseudo regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${CLANG_PSEUDO_TEST_DEPS})
