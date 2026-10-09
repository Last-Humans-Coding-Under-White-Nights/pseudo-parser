set(LLVM_LINK_COMPONENTS
  Support
)

if(NOT TARGET LLVMTestingSupport)
  if(CLANGD_INCLUDE_DIRS)
    get_filename_component(_LLVM_PROJECT_ROOT "${CLANGD_INCLUDE_DIRS}/../.." ABSOLUTE)
  elseif(DEFINED LLVM_MAIN_SRC_DIR)
    get_filename_component(_LLVM_PROJECT_ROOT "${LLVM_MAIN_SRC_DIR}/.." ABSOLUTE)
  endif()

  if(_LLVM_PROJECT_ROOT AND EXISTS "${_LLVM_PROJECT_ROOT}/third-party/unittest/googletest/src/gtest-all.cc")
    add_library(llvm_gtest STATIC
      "${_LLVM_PROJECT_ROOT}/third-party/unittest/googletest/src/gtest-all.cc"
    )
    target_include_directories(llvm_gtest PUBLIC
      "${_LLVM_PROJECT_ROOT}/third-party/unittest/googletest/include"
      "${_LLVM_PROJECT_ROOT}/third-party/unittest/googletest"
    )
    target_link_libraries(llvm_gtest PUBLIC LLVMSupport)

    add_library(llvm_gtest_main STATIC
      "${_LLVM_PROJECT_ROOT}/third-party/unittest/UnitTestMain/TestMain.cpp"
    )
    target_include_directories(llvm_gtest_main PUBLIC
      "${_LLVM_PROJECT_ROOT}/llvm/include"
      "${_LLVM_PROJECT_ROOT}/third-party/unittest/googletest/include"
    )
    target_link_libraries(llvm_gtest_main PUBLIC llvm_gtest LLVMSupport)

    add_library(LLVMTestingSupport STATIC
      "${_LLVM_PROJECT_ROOT}/llvm/lib/Testing/Support/Error.cpp"
      "${_LLVM_PROJECT_ROOT}/llvm/lib/Testing/Support/SupportHelpers.cpp"
    )
    target_include_directories(LLVMTestingSupport PUBLIC
      "${_LLVM_PROJECT_ROOT}/llvm/include"
      "${_LLVM_PROJECT_ROOT}/third-party/unittest/googletest/include"
    )
    target_link_libraries(LLVMTestingSupport PUBLIC llvm_gtest LLVMSupport)

    add_library(LLVMTestingAnnotations STATIC
      "${_LLVM_PROJECT_ROOT}/llvm/lib/Testing/Annotations/Annotations.cpp"
    )
    target_include_directories(LLVMTestingAnnotations PUBLIC
      "${_LLVM_PROJECT_ROOT}/llvm/include"
    )
    target_link_libraries(LLVMTestingAnnotations PUBLIC LLVMSupport)
  endif()
endif()

if(NOT TARGET LLVMTestingSupport)
  message(WARNING "LLVMTestingSupport not available; skipping ClangPseudoTests.")
  return()
endif()

if(NOT COMMAND add_unittest)
  macro(add_unittest suite name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE llvm_gtest_main llvm_gtest LLVMSupport)
  endmacro()
endif()

add_custom_target(ClangPseudoUnitTests)
set_target_properties(ClangPseudoUnitTests PROPERTIES FOLDER "Tests")
add_unittest(ClangPseudoUnitTests ClangPseudoTests
  BracketTest.cpp
  CXXTest.cpp
  DirectiveTreeTest.cpp
  DisambiguateTest.cpp
  ForestTest.cpp
  GLRTest.cpp
  GrammarTest.cpp
  LRTableTest.cpp
  TokenTest.cpp
)

clang_target_link_libraries(ClangPseudoTests
  PRIVATE
  clangBasic
  clangLex
)

target_link_libraries(ClangPseudoTests
  PRIVATE
  Pseudo
  PseudoCXX
  PseudoGrammar
  LLVMTestingAnnotations
  LLVMTestingSupport
)

if(CLANGD_INCLUDE_DIRS AND TARGET clangDaemon)
  add_subdirectory(clangd)
endif()
