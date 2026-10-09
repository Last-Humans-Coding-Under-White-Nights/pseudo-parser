add_library(PseudoCXX STATIC
  CXX.cpp
)
add_dependencies(PseudoCXX cxx_gen)
target_link_libraries(PseudoCXX PRIVATE Pseudo PseudoGrammar)

if(COMMAND llvm_update_compile_flags)
  llvm_update_compile_flags(PseudoCXX)
endif()

if(WIN32)
  clang_target_link_libraries(PseudoCXX
    PRIVATE
    clangBasic
    )
endif()
