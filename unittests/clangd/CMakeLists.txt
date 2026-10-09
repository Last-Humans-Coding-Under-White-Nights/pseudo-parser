set(LLVM_LINK_COMPONENTS
  Support
)

add_unittest(ClangPseudoUnitTests PseudoModuleTests
  PseudoModuleTests.cpp
)

target_include_directories(PseudoModuleTests PRIVATE
  "${CLANGD_INCLUDE_DIRS}"
  "${CLANGD_INCLUDE_DIRS}/../include-cleaner/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../clangd"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
  "${CMAKE_BINARY_DIR}/include"
)

target_link_libraries(PseudoModuleTests PRIVATE
  Pseudo
  PseudoCXX
  PseudoGrammar
  clangBasic
  clangLex
  LLVMTestingAnnotations
  LLVMTestingSupport
)

if(TARGET clangDaemon)
  target_link_libraries(PseudoModuleTests PRIVATE
    clangDaemon
    clangdSupport
  )
endif()
