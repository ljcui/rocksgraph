cmake_minimum_required(VERSION 3.15)
project(rocksgraph)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(GENERATED_SOURCES
        ast/CypherLexer.cc
        ast/CypherParser.cc
        ast/CypherVisitor.cc
)
include_directories(/usr/local/include/antlr4-runtime)
add_executable(rg-server
    main.cc
        ast/ast_builder.cc
        ast/ast_printer.cc
    ${GENERATED_SOURCES}
)
target_link_libraries(rg-server antlr4-runtime)
