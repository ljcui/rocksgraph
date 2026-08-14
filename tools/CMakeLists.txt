add_executable(cypher_run cypher_run.cc)
target_link_libraries(cypher_run PRIVATE rocksgraph_runtime ${GFLAGS_TARGET}
                                         ${SPDLOG_TARGET})
set_target_properties(cypher_run PROPERTIES BUILD_RPATH "/usr/local/lib")

add_executable(cypher_dump cypher_dump.cc)
target_link_libraries(cypher_dump PRIVATE rocksgraph_planner ${GFLAGS_TARGET}
                                          ${SPDLOG_TARGET})
set_target_properties(cypher_dump PROPERTIES BUILD_RPATH "/usr/local/lib")
