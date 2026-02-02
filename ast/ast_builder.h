#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"

namespace ast {

struct ParseResult {
  std::unique_ptr<Statement> statement;
  std::vector<std::string> errors;
};

// Parse Cypher text into AST. If errors occur, statement will be nullptr
// and errors will contain human-readable messages.
ParseResult parseCypher(const std::string &input);

// Parse and apply default AST rewriters when parsing succeeds.
ParseResult parseCypherAndRewrite(const std::string &input);

}  // namespace ast
