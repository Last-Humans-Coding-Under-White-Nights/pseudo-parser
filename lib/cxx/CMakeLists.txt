if(WIN32)
  set(LLVM_LINK_COMPONENTS
    Support
    )
endif()

add_clang_library(PseudoCXX
  CXX.cpp

  DEPENDS
  cxx_gen

  LINK_LIBS
  Pseudo
  PseudoGrammar
  )

if(WIN32)
  clang_target_link_libraries(PseudoCXX
    PRIVATE
    clangBasic
    )
endif()
