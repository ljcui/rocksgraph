add_library(rocksgraph_value STATIC temporal.cc value.cc)

target_include_directories(rocksgraph_value PUBLIC ${PROJECT_SOURCE_DIR})
