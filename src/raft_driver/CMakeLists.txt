find_package(Boost REQUIRED)
find_package(RocksDB REQUIRED)
find_package(Threads REQUIRED)

add_library(raft_driver STATIC logger.cpp raft_driver.cpp raft_log_store.cpp)

target_compile_features(raft_driver PUBLIC cxx_std_20)
target_include_directories(raft_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(
  raft_driver PUBLIC Boost::headers RocksDB::rocksdb Threads::Threads common
                     etcd_raft proto)
