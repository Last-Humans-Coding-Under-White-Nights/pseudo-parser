if(WIN32)
  set(LLVM_LINK_COMPONENTS Support)
endif()

add_clang_library(PseudoGrammar
  Grammar.cpp
  GrammarBNF.cpp
  LRGraph.cpp
  LRTable.cpp
  LRTableBuild.cpp
  )
