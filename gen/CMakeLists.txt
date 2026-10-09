set(LLVM_LINK_COMPONENTS Support)

add_clang_executable(pseudo-gen
  Main.cpp
  )
set_target_properties(pseudo-gen PROPERTIES OUTPUT_NAME "clang-pseudo-gen")

target_link_libraries(pseudo-gen
  PRIVATE
  PseudoGrammar
  )
