find_package(Protobuf REQUIRED)

add_library(proto STATIC meta.pb.cc)

target_compile_features(proto PUBLIC cxx_std_20)
target_include_directories(proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(proto PUBLIC protobuf::libprotobuf etcd_raft)
