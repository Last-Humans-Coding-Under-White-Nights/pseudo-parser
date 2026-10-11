set(LLVM_LINK_COMPONENTS
  Support
)

include(LLVMTestingLibs)
pseudo_require_llvm_testing_libs()

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

target_link_libraries(ClangPseudoTests
  PRIVATE
  Pseudo
  PseudoCXX
  PseudoGrammar
  LLVMTestingAnnotations
  LLVMTestingSupport
)

# The clang libraries must follow the Pseudo libraries on the link line:
# libPseudo.a uses clang::Lexer, and GNU ld resolves static archives in a
# single pass (see tool/CMakeLists.txt for the same ordering).
clang_target_link_libraries(ClangPseudoTests
  PRIVATE
  clangBasic
  clangLex
)

if(CLANGD_INCLUDE_DIRS)
  add_subdirectory(clangd)
endif()
