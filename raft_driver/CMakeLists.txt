find_package(Boost REQUIRED)
find_package(RocksDB REQUIRED)
find_package(Threads REQUIRED)

add_library(rocksgraph_raft_driver STATIC logger.cc raft_driver.cc
                                          raft_log_store.cc)
add_library(RocksGraph::raft_driver ALIAS rocksgraph_raft_driver)

target_compile_features(rocksgraph_raft_driver PUBLIC cxx_std_20)
target_include_directories(rocksgraph_raft_driver PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(
  rocksgraph_raft_driver
  PUBLIC Boost::headers RocksDB::rocksdb Threads::Threads RocksGraph::common
         RocksGraph::etcd_raft RocksGraph::proto)
