find_package(gflags REQUIRED)
find_package(spdlog REQUIRED)

add_library(
  common STATIC
  exceptions.cpp
  flags.cpp
  logger.cpp)

target_compile_features(common PUBLIC cxx_std_20)
target_include_directories(common PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(
  common
  PUBLIC ${GFLAGS_TARGET} spdlog::spdlog)
