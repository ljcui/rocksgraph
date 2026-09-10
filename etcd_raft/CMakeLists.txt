find_package(Protobuf REQUIRED)

add_library(etcd_raft STATIC raftpb/raft.pb.cc)

target_compile_features(etcd_raft PUBLIC cxx_std_20)
target_include_directories(
  etcd_raft PUBLIC ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
                   ${CMAKE_CURRENT_SOURCE_DIR}/raftpb)
target_link_libraries(etcd_raft PUBLIC protobuf::libprotobuf)

option(ROCKSGRAPH_BUILD_RAFT_TESTS "Build etcd-raft-cpp tests" OFF)
if(ROCKSGRAPH_BUILD_RAFT_TESTS)
  find_package(GTest REQUIRED)

  add_executable(
    raft_test
    test_main.cc
    raftpb/confstate_test.cpp
    tracker/inflights_test.cpp
    tracker/progress_test.cpp
    util_test.cc
    storage_test.cc
    log_unstable_test.cpp
    log_test.cpp
    raft_test.cpp
    rawnode_test.cpp
    raft_snap_test.cpp
    raft_flow_control_test.cpp
    raft_paper_test.cpp)
  target_compile_definitions(raft_test PRIVATE RAFT_TEST=1 RAFT_LOG=1)
  target_link_libraries(raft_test PRIVATE GTest::gtest etcd_raft
                                          Threads::Threads)
endif()
