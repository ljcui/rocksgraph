#pragma once

#include <memory>
#include <string>

#include "ast_node.h"

namespace ast {

// Parse Cypher text into AST. Throws ast::ParseError or ast::SemanticError on
// failure.
std::unique_ptr<Statement> parseCypher(const std::string &input);

// Parse and apply default AST rewriters when parsing succeeds.
std::unique_ptr<Statement> parseCypherAndRewrite(const std::string &input);

}  // namespace ast
