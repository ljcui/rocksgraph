find_package(Protobuf REQUIRED)

add_library(rocksgraph_proto STATIC meta.pb.cc)
add_library(RocksGraph::proto ALIAS rocksgraph_proto)

target_compile_features(rocksgraph_proto PUBLIC cxx_std_20)
target_include_directories(rocksgraph_proto PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_proto PUBLIC protobuf::libprotobuf
                                              RocksGraph::etcd_raft)
