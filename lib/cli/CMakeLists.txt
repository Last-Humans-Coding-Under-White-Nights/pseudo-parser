set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(PseudoCLI
  CLI.cpp

  DEPENDS
  cxx_gen

  LINK_LIBS
  PseudoGrammar
  PseudoCXX
  )
