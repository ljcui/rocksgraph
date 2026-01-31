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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(ANTLR4Runtime REQUIRED)
add_executable(rg-server
    main.cc
        ast/ast_builder.cc
        ast/ast_equal.cc
        ast/ast_printer.cc
        ast/ast_rewriter.cc
    ${GENERATED_SOURCES}
)
target_link_libraries(rg-server ANTLR4Runtime::antlr4_runtime)
set_target_properties(rg-server PROPERTIES BUILD_RPATH "/usr/local/lib")

option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest)
    if(GTest_FOUND)
        add_executable(ast_rewriter_test
            tests/ast_rewriter_test.cc
            ast/ast_builder.cc
            ast/ast_equal.cc
            ast/ast_printer.cc
            ast/ast_rewriter.cc
            ${GENERATED_SOURCES}
        )
        target_include_directories(ast_rewriter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(ast_rewriter_test GTest::gtest_main ANTLR4Runtime::antlr4_runtime)
        set_target_properties(ast_rewriter_test PROPERTIES BUILD_RPATH "/usr/local/lib")
        add_test(NAME ast_rewriter_test COMMAND ast_rewriter_test)
    endif()
endif()
