find_package(Boost REQUIRED)
find_package(RocksDB REQUIRED)

add_library(transaction STATIC transaction.cpp)

target_compile_features(transaction PUBLIC cxx_std_20)
target_include_directories(transaction PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(transaction PUBLIC Boost::headers RocksDB::rocksdb common
                                         graphdb rocksgraph_value)
