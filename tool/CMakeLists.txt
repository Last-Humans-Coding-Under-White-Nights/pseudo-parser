set(LLVM_LINK_COMPONENTS support)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_clang_tool(pseudo-tool
  ClangPseudo.cpp
  HTMLForest.cpp
  )
set_target_properties(pseudo-tool PROPERTIES OUTPUT_NAME "clang-pseudo")

clang_target_link_libraries(pseudo-tool
  PRIVATE
  clangBasic
  )

target_include_directories(pseudo-tool
  PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  )

target_link_libraries(pseudo-tool
  PRIVATE
  Pseudo
  PseudoGrammar
  PseudoCLI
  )

add_custom_command(OUTPUT HTMLForestResources.inc
  COMMAND "${Python3_EXECUTABLE}" ${CMAKE_CURRENT_SOURCE_DIR}/bundle_resources.py
  ${CMAKE_CURRENT_BINARY_DIR}/HTMLForestResources.inc
  HTMLForest.css HTMLForest.js HTMLForest.html
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Bundling HTMLForest resources"
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bundle_resources.py HTMLForest.css HTMLForest.js HTMLForest.html
  VERBATIM)
add_custom_target(clang-pseudo-resources DEPENDS HTMLForestResources.inc)
set_target_properties(clang-pseudo-resources PROPERTIES FOLDER "Resources")
add_dependencies(pseudo-tool clang-pseudo-resources)
