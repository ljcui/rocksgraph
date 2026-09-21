find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(gflags REQUIRED)
find_package(spdlog REQUIRED)

add_library(rocksgraph_common exception.cc flags.cc logger.cc)
add_library(RocksGraph::common ALIAS rocksgraph_common)

target_compile_features(rocksgraph_common PUBLIC cxx_std_20)
target_include_directories(rocksgraph_common PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(
  rocksgraph_common PUBLIC Boost::headers Threads::Threads ${GFLAGS_TARGET}
                           ${SPDLOG_TARGET})
