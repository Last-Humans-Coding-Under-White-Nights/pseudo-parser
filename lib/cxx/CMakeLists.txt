add_library(PseudoCXX STATIC
  CXX.cpp
)
add_dependencies(PseudoCXX cxx_gen)
target_link_libraries(PseudoCXX PRIVATE Pseudo PseudoGrammar)

if(WIN32)
  clang_target_link_libraries(PseudoCXX
    PRIVATE
    clangBasic
    )
endif()
