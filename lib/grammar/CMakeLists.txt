add_library(PseudoGrammar STATIC
  Grammar.cpp
  GrammarBNF.cpp
  LRGraph.cpp
  LRTable.cpp
  LRTableBuild.cpp
)

if(COMMAND llvm_update_compile_flags)
  llvm_update_compile_flags(PseudoGrammar)
endif()
