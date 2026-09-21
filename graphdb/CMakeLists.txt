find_package(BLAS REQUIRED)
find_package(Boost REQUIRED)
find_package(OpenMP REQUIRED)
find_package(RocksDB REQUIRED)
find_package(Threads REQUIRED)

find_path(FAISS_INCLUDE_DIR faiss/Index.h REQUIRED)
find_library(FAISS_LIBRARY NAMES faiss REQUIRED)
add_library(rocksgraph_graphdb_faiss STATIC IMPORTED)
set_target_properties(
  rocksgraph_graphdb_faiss
  PROPERTIES IMPORTED_LOCATION "${FAISS_LIBRARY}"
             INTERFACE_INCLUDE_DIRECTORIES "${FAISS_INCLUDE_DIR}"
             INTERFACE_LINK_LIBRARIES "BLAS::BLAS;OpenMP::OpenMP_CXX")

find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(FTINDEX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ftindex")
set(FTINDEX_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/ftindex-target")
set(FTINDEX_LIBRARY "${FTINDEX_TARGET_DIR}/release/libftindex.a")
file(
  GLOB_RECURSE
  FTINDEX_SOURCES
  CONFIGURE_DEPENDS
  "${FTINDEX_SOURCE_DIR}/src/*.rs"
  "${FTINDEX_SOURCE_DIR}/Cargo.toml"
  "${FTINDEX_SOURCE_DIR}/Cargo.lock"
  "${FTINDEX_SOURCE_DIR}/build.rs"
  "${FTINDEX_SOURCE_DIR}/stopwords_zh.txt")
add_custom_command(
  OUTPUT "${FTINDEX_LIBRARY}"
  COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${FTINDEX_TARGET_DIR}"
          "MAKEFLAGS=" ${CARGO_EXECUTABLE} build --release --locked
  WORKING_DIRECTORY "${FTINDEX_SOURCE_DIR}"
  DEPENDS ${FTINDEX_SOURCES}
  COMMENT "Building the full-text index library"
  VERBATIM)
add_custom_target(rocksgraph_graphdb_ftindex_build DEPENDS "${FTINDEX_LIBRARY}")
add_library(rocksgraph_graphdb_ftindex STATIC IMPORTED)
set_target_properties(
  rocksgraph_graphdb_ftindex
  PROPERTIES IMPORTED_LOCATION "${FTINDEX_LIBRARY}"
             INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS};m")
add_dependencies(rocksgraph_graphdb_ftindex rocksgraph_graphdb_ftindex_build)

add_library(
  rocksgraph_graphdb SHARED
  assistant_pool.cc
  edge_index_updater.cc
  edge_iterator.cc
  graph_db.cc
  graph_db_index_build.cc
  graph_db_internal.cc
  graph_db_property_index.cc
  graph_db_raft.cc
  graph_db_search_index.cc
  graph_entity.cc
  hnsw_index.cc
  id_generator.cc
  index.cc
  meta_info.cc
  transaction.cc
  value_codec.cc
  vector_property.cc
  vector_store.cc
  vertex_index_updater.cc
  vertex_iterator.cc)
add_library(RocksGraph::graphdb ALIAS rocksgraph_graphdb)

target_compile_features(rocksgraph_graphdb PUBLIC cxx_std_20)
target_include_directories(rocksgraph_graphdb PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(
  rocksgraph_graphdb
  PUBLIC Boost::headers
         ${ROCKSDB_TARGET}
         Threads::Threads
         RocksGraph::common
         rocksgraph_graphdb_faiss
         rocksgraph_graphdb_ftindex
         RocksGraph::graph_replication_proto
         RocksGraph::graph_storage_proto
         RocksGraph::raft_driver
         RocksGraph::value)
