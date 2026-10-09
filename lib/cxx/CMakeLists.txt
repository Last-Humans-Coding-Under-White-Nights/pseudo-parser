set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(PseudoCXX
  CXX.cpp

  DEPENDS
  cxx_gen

  LINK_LIBS
  Pseudo
  PseudoGrammar
  )

clang_target_link_libraries(PseudoCXX
  PRIVATE
  clangBasic
  )
