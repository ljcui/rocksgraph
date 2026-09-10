find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(bolt STATIC bolt_server.cpp connection.cpp hydrator.cpp pack.cpp)

target_compile_features(bolt PUBLIC cxx_std_20)
target_include_directories(bolt PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(bolt PUBLIC Boost::headers Threads::Threads common
                                  nlohmann_json::nlohmann_json rocksgraph_value)
