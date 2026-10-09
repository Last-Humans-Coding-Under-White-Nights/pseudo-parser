add_subdirectory(cli)
add_subdirectory(cxx)

add_library(Pseudo STATIC
  Bracket.cpp
  DirectiveTree.cpp
  Disambiguate.cpp
  Forest.cpp
  GLR.cpp
  Lex.cpp
  Token.cpp
)
target_link_libraries(Pseudo PRIVATE PseudoGrammar)

if(COMMAND llvm_update_compile_flags)
  llvm_update_compile_flags(Pseudo)
endif()

target_include_directories(Pseudo INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../include>
)

