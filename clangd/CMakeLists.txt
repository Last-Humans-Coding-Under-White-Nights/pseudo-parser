set(LLVM_LINK_COMPONENTS
  Support
)

set(PSEUDO_MODULE_SOURCES
  PseudoModule.cpp
  pseudo/AST.cpp
  pseudo/Completion.cpp
  pseudo/Diagnostics.cpp
  pseudo/Headers.cpp
  pseudo/Highlight.cpp
  pseudo/Hover.cpp
  pseudo/Parse.cpp
  pseudo/Scopes.cpp
  pseudo/Symbols.cpp
  pseudo/Types.cpp
  pseudo/XRefs.cpp
)

if(COMMAND llvm_add_library)
  llvm_add_library(
    clangd_pseudo
    MODULE
    ${PSEUDO_MODULE_SOURCES}
    PLUGIN_TOOL clangd
    PARTIAL_SOURCES_INTENDED
  )
else()
  add_library(
    clangd_pseudo
    MODULE
    ${PSEUDO_MODULE_SOURCES}
  )
endif()

target_include_directories(clangd_pseudo PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CLANGD_INCLUDE_DIRS}"
  "${CLANGD_INCLUDE_DIRS}/../include-cleaner/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${CMAKE_BINARY_DIR}/include"
)

if(APPLE)
  target_link_options(clangd_pseudo PRIVATE
    "-Wl,-flat_namespace"
    "-Wl,-undefined,dynamic_lookup"
  )
elseif(UNIX)
  target_link_options(clangd_pseudo PRIVATE
    "-Wl,--unresolved-symbols=ignore-all"
  )
endif()

target_link_libraries(clangd_pseudo
  PRIVATE
  Pseudo
  PseudoCXX
  PseudoGrammar
  clangBasic
  clangLex
)
