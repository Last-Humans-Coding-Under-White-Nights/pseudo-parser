if(WIN32)
  set(LLVM_LINK_COMPONENTS
    Support
  )
endif()

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

if(WIN32 AND NOT TARGET clangd)
  find_library(CLANGD_IMPORT_LIB NAMES clangd PATHS
    ${LLVM_LIBRARY_DIRS}
    "${LLVM_DIR}/../../.."
    "${LLVM_DIR}/../.."
    "${LLVM_DIR}/../../lib"
    "${LLVM_DIR}/../../bin"
    "${CMAKE_CURRENT_BINARY_DIR}/../llvm-build/lib"
    "${CMAKE_CURRENT_BINARY_DIR}/../llvm-build/bin"
  )
  if(CLANGD_IMPORT_LIB)
    message(STATUS "Found Clangd import library: ${CLANGD_IMPORT_LIB}")
    add_library(clangd UNKNOWN IMPORTED)
    set_target_properties(clangd PROPERTIES IMPORTED_IMPLIB "${CLANGD_IMPORT_LIB}" IMPORTED_LOCATION "${CLANGD_IMPORT_LIB}")
  else()
    message(WARNING "Clangd import library (clangd.lib) was not found! Plugin link might fail.")
  endif()
endif()

add_library(
  clangd_pseudo
  MODULE
  ${PSEUDO_MODULE_SOURCES}
)

if(COMMAND llvm_update_compile_flags)
  llvm_update_compile_flags(clangd_pseudo)
endif()

if(MSVC)
  target_compile_options(clangd_pseudo PRIVATE /GR-)
else()
  target_compile_options(clangd_pseudo PRIVATE -fno-rtti)
endif()

target_include_directories(clangd_pseudo PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CLANGD_INCLUDE_DIRS}"
  "${CLANGD_INCLUDE_DIRS}/../include-cleaner/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${CMAKE_BINARY_DIR}/include"
)

if(APPLE)
  set_target_properties(clangd_pseudo PROPERTIES SUFFIX ".dylib")
  target_link_options(clangd_pseudo PRIVATE
    "-Wl,-flat_namespace"
    "-Wl,-undefined,dynamic_lookup"
  )
elseif(UNIX)
  target_link_options(clangd_pseudo PRIVATE
    "-Wl,--unresolved-symbols=ignore-all"
    "-Wl,--as-needed"
  )
endif()

target_link_libraries(clangd_pseudo
  PRIVATE
  Pseudo
  PseudoCXX
  PseudoGrammar
)

if(WIN32)
  if(TARGET clangd)
    target_link_libraries(clangd_pseudo PRIVATE clangd)
  endif()
  set_property(TARGET clangd_pseudo APPEND_STRING PROPERTY LINK_FLAGS " /FORCE:MULTIPLE")
  target_link_options(clangd_pseudo PRIVATE "LINKER:/FORCE:MULTIPLE")
endif()
