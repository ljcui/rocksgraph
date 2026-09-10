find_package(BLAS REQUIRED)
find_package(Boost REQUIRED)
find_package(OpenMP REQUIRED)
find_package(RocksDB REQUIRED)
find_package(Threads REQUIRED)

find_path(FAISS_INCLUDE_DIR faiss/Index.h REQUIRED)
find_library(FAISS_LIBRARY NAMES faiss REQUIRED)
add_library(graphdb_faiss STATIC IMPORTED)
set_target_properties(
  graphdb_faiss
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
add_custom_target(graphdb_ftindex_build DEPENDS "${FTINDEX_LIBRARY}")
add_library(graphdb_ftindex STATIC IMPORTED)
set_target_properties(
  graphdb_ftindex
  PROPERTIES IMPORTED_LOCATION "${FTINDEX_LIBRARY}"
             INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS};m")
add_dependencies(graphdb_ftindex graphdb_ftindex_build)

add_library(
  graphdb STATIC
  assistant_pool.cpp
  edge_iterator.cpp
  graph_db.cpp
  graph_entity.cpp
  hnsw_index.cpp
  id_generator.cpp
  index.cpp
  meta_info.cpp
  vector_property.cpp
  vector_store.cpp
  vertex_index_updater.cpp
  vertex_iterator.cpp)

target_compile_features(graphdb PUBLIC cxx_std_20)
target_include_directories(graphdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(
  graphdb
  PUBLIC Boost::headers
         RocksDB::rocksdb
         Threads::Threads
         bolt
         common
         graphdb_faiss
         graphdb_ftindex
         proto
         raft_driver
         transaction)
