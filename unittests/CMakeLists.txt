set(LLVM_LINK_COMPONENTS
  Support
)

if(NOT TARGET LLVMTestingSupport)
  message(STATUS "LLVMTestingSupport target not found in LLVM installation. Skipping unittests.")
  return()
endif()

add_custom_target(ClangPseudoUnitTests)
set_target_properties(ClangPseudoUnitTests PROPERTIES FOLDER "Tests")
add_unittest(ClangPseudoUnitTests ClangPseudoTests
  BracketTest.cpp
  CXXTest.cpp
  DirectiveTreeTest.cpp
  DisambiguateTest.cpp
  ForestTest.cpp
  GLRTest.cpp
  GrammarTest.cpp
  LRTableTest.cpp
  TokenTest.cpp
)

clang_target_link_libraries(ClangPseudoTests
  PRIVATE
  clangBasic
  clangLex
)

target_link_libraries(ClangPseudoTests
  PRIVATE
  Pseudo
  PseudoCXX
  PseudoGrammar
  LLVMTestingAnnotations
  LLVMTestingSupport
)

if(CLANGD_INCLUDE_DIRS AND TARGET clangDaemon)
  add_subdirectory(clangd)
endif()
