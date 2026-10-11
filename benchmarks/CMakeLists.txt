if(NOT TARGET benchmark)
  message(FATAL_ERROR
    "PSEUDO_ENABLE_BENCHMARKS needs LLVM's google benchmark target, which is "
    "only exported from an LLVM build tree configured with "
    "LLVM_INCLUDE_BENCHMARKS=ON.")
endif()

add_benchmark(ClangPseudoBenchmark Benchmark.cpp)

target_link_libraries(ClangPseudoBenchmark
  PRIVATE
  Pseudo
  PseudoCLI
  PseudoGrammar
  LLVMSupport
  )

clang_target_link_libraries(ClangPseudoBenchmark
  PRIVATE
  clangBasic
  clangLex
  )
