if(NOT ANTLR4_JAR_LOCATION OR NOT EXISTS "${ANTLR4_JAR_LOCATION}")
  message(
    FATAL_ERROR
      "ANTLR4 generator JAR was not found. Configure with "
      "-DANTLR4_JAR_LOCATION=/path/to/antlr-4.13.2-complete.jar.")
endif()

if(NOT ANTLR4_JAVA_EXECUTABLE OR NOT EXISTS "${ANTLR4_JAVA_EXECUTABLE}")
  message(
    FATAL_ERROR
      "Java runtime was not found. Install Java or configure "
      "-DANTLR4_JAVA_EXECUTABLE=/path/to/java.")
endif()

set(grammar_file "${ROCKSGRAPH_CYPHER_GRAMMAR_DIR}/Cypher.g4")
if(NOT EXISTS "${grammar_file}")
  message(FATAL_ERROR "Cypher grammar was not found: ${grammar_file}")
endif()

file(REMOVE_RECURSE "${ROCKSGRAPH_CYPHER_WORK_DIR}")
file(MAKE_DIRECTORY "${ROCKSGRAPH_CYPHER_WORK_DIR}")

execute_process(
  COMMAND "${ANTLR4_JAVA_EXECUTABLE}" -jar "${ANTLR4_JAR_LOCATION}" -Werror
          -Dlanguage=Cpp -no-listener -visitor -o
          "${ROCKSGRAPH_CYPHER_WORK_DIR}" Cypher.g4
  WORKING_DIRECTORY "${ROCKSGRAPH_CYPHER_GRAMMAR_DIR}"
  RESULT_VARIABLE antlr_result
  OUTPUT_VARIABLE antlr_stdout
  ERROR_VARIABLE antlr_stderr)

if(NOT antlr_result EQUAL 0)
  message(
    FATAL_ERROR
      "ANTLR failed with exit code ${antlr_result}.\n${antlr_stdout}${antlr_stderr}")
endif()

# ANTLR emits .cpp files, which is also the repository convention for the
# generated implementation files. Generated contents are copied byte-for-byte
# and are never formatted.
foreach(parser_name IN ITEMS CypherLexer CypherParser CypherVisitor)
  foreach(extension IN ITEMS cpp h)
    if(NOT EXISTS "${ROCKSGRAPH_CYPHER_WORK_DIR}/${parser_name}.${extension}")
      message(
        FATAL_ERROR
          "ANTLR did not generate the expected file: ${parser_name}.${extension}")
    endif()
  endforeach()

  execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ROCKSGRAPH_CYPHER_WORK_DIR}/${parser_name}.cpp"
            "${ROCKSGRAPH_CYPHER_SOURCE_DIR}/${parser_name}.cpp"
    RESULT_VARIABLE copy_result)
  if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to update ${parser_name}.cpp")
  endif()

  execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ROCKSGRAPH_CYPHER_WORK_DIR}/${parser_name}.h"
            "${ROCKSGRAPH_CYPHER_SOURCE_DIR}/${parser_name}.h"
    RESULT_VARIABLE copy_result)
  if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to update ${parser_name}.h")
  endif()
endforeach()

message(
  STATUS
    "Updated Cypher generated sources from ANTLR without formatting or content changes")
