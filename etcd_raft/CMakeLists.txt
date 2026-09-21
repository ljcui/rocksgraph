find_package(Protobuf REQUIRED)

add_library(rocksgraph_etcd_raft STATIC raftpb/raft.pb.cc)
add_library(RocksGraph::etcd_raft ALIAS rocksgraph_etcd_raft)

target_compile_features(rocksgraph_etcd_raft PUBLIC cxx_std_20)
target_include_directories(
  rocksgraph_etcd_raft PUBLIC ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
                              ${CMAKE_CURRENT_SOURCE_DIR}/raftpb)
target_link_libraries(rocksgraph_etcd_raft PUBLIC protobuf::libprotobuf
                                                  RocksGraph::common)

option(ROCKSGRAPH_BUILD_RAFT_TESTS "Build etcd-raft-cpp tests" OFF)
if(ROCKSGRAPH_BUILD_RAFT_TESTS)
  find_package(GTest REQUIRED)

  add_executable(
    raft_test
    test_main.cc
    raftpb/confstate_test.cc
    tracker/inflights_test.cc
    tracker/progress_test.cc
    util_test.cc
    storage_test.cc
    log_unstable_test.cc
    log_test.cc
    raft_test.cc
    rawnode_test.cc
    raft_snap_test.cc
    raft_flow_control_test.cc
    raft_paper_test.cc)
  target_compile_definitions(raft_test PRIVATE RAFT_TEST=1 RAFT_LOG=1)
  target_link_libraries(raft_test PRIVATE GTest::gtest RocksGraph::etcd_raft
                                          Threads::Threads)
endif()
