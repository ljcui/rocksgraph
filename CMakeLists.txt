cmake_minimum_required(VERSION 3.15)
project(rocksgraph)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(GENERATED_SOURCES
    ast/CypherLexer.cpp
    ast/CypherParser.cpp
    ast/CypherVisitor.cpp
)
include_directories(/usr/local/include/antlr4-runtime)
add_executable(rg-server
    main.cc
    ast/ast_builder.cpp
    ast/ast_printer.cpp
    ${GENERATED_SOURCES}
)
target_link_libraries(rg-server antlr4-runtime)
