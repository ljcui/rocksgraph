find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(rocksgraph_bolt bolt_server.cc connection.cc hydrator.cc pack.cc)
add_library(RocksGraph::bolt ALIAS rocksgraph_bolt)

target_compile_features(rocksgraph_bolt PUBLIC cxx_std_20)
target_include_directories(rocksgraph_bolt PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(
  rocksgraph_bolt PUBLIC Boost::headers Threads::Threads RocksGraph::common
                         nlohmann_json::nlohmann_json RocksGraph::value)
