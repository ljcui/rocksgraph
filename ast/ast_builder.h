#pragma once

#include <memory>
#include <string>

#include "ast_node.h"

namespace ast {

// Parse Cypher text into AST. Throws a common::RocksGraphException with
// ParserException or CypherException on failure.
std::unique_ptr<Statement> ParseCypher(const std::string &input);

// Parse and apply default AST rewriters when parsing succeeds.
std::unique_ptr<Statement> ParseCypherAndRewrite(const std::string &input);

}  // namespace ast
