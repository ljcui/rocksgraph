find_package(date REQUIRED)
find_package(gflags REQUIRED)
find_package(spdlog REQUIRED)

add_library(
  common STATIC
  exceptions.cpp
  flags.cpp
  logger.cpp
  result.cpp
  temporal/date.cpp
  temporal/datetime.cpp
  temporal/duration.cpp
  temporal/localdatetime.cpp
  temporal/localtime.cpp
  temporal/time.cpp
  value.cpp)

target_compile_features(common PUBLIC cxx_std_20)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(
  common
  PUBLIC ${GFLAGS_TARGET} spdlog::spdlog
  PRIVATE date::date-tz)
