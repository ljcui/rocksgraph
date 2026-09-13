if(NOT DEFINED CYPHER_RUN OR CYPHER_RUN STREQUAL "")
  message(FATAL_ERROR "CYPHER_RUN is required")
endif()
if(NOT DEFINED TEST_DB OR TEST_DB STREQUAL "")
  message(FATAL_ERROR "TEST_DB is required")
endif()

file(REMOVE_RECURSE "${TEST_DB}")

execute_process(
  COMMAND "${CYPHER_RUN}" "RETURN 1"
  RESULT_VARIABLE missing_path_result
  OUTPUT_VARIABLE missing_path_output
  ERROR_VARIABLE missing_path_error)

execute_process(
  COMMAND "${CYPHER_RUN}" "--db_path=${TEST_DB}"
          "CREATE (:Person {name: 'Ada'})"
  RESULT_VARIABLE create_result
  OUTPUT_VARIABLE create_output
  ERROR_VARIABLE create_error)

execute_process(
  COMMAND "${CYPHER_RUN}" "--db_path=${TEST_DB}"
          "MATCH (n:Person) RETURN n.name AS name"
  RESULT_VARIABLE read_result
  OUTPUT_VARIABLE read_output
  ERROR_VARIABLE read_error)

file(REMOVE_RECURSE "${TEST_DB}")

if(missing_path_result EQUAL 0)
  message(
    FATAL_ERROR
      "cypher_run accepted a missing --db_path\n${missing_path_output}\n${missing_path_error}"
  )
endif()
if(NOT create_result EQUAL 0)
  message(
    FATAL_ERROR
      "cypher_run CREATE failed (${create_result})\n${create_output}\n${create_error}"
  )
endif()
if(NOT read_result EQUAL 0)
  message(
    FATAL_ERROR
      "cypher_run MATCH failed (${read_result})\n${read_output}\n${read_error}"
  )
endif()
if(NOT read_output MATCHES "name[\r\n]+\"Ada\"")
  message(FATAL_ERROR "unexpected cypher_run output:\n${read_output}")
endif()
