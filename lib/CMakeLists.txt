add_subdirectory(cli)
add_subdirectory(cxx)

set(LLVM_LINK_COMPONENTS Support)

if(TARGET ClangDriverOptions)
  set(CLANG_DRIVER_DEP DEPENDS ClangDriverOptions)
endif()

add_clang_library(Pseudo
  Bracket.cpp
  DirectiveTree.cpp
  Disambiguate.cpp
  Forest.cpp
  GLR.cpp
  Lex.cpp
  Token.cpp

  LINK_LIBS
  PseudoGrammar

  ${CLANG_DRIVER_DEP}
  )

target_include_directories(Pseudo INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../include>
)

if(WIN32)
  clang_target_link_libraries(Pseudo
    PRIVATE
    clangBasic
    clangLex
    )
endif()
