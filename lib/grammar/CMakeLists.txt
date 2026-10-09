set(LLVM_LINK_COMPONENTS Support)

add_clang_library(PseudoGrammar
  Grammar.cpp
  GrammarBNF.cpp
  LRGraph.cpp
  LRTable.cpp
  LRTableBuild.cpp
  )
