#ifndef RGRAPH_AST_BUILDER_H
#define RGRAPH_AST_BUILDER_H

#include <memory>
#include <string>
#include <vector>

#include "ast.h"

namespace cypher {

    struct ParseResult {
        std::unique_ptr<Statement> statement;
        std::vector<std::string> errors;
    };

    // Parse Cypher text into AST. If errors occur, statement will be nullptr
    // and errors will contain human-readable messages.
    ParseResult parseCypher(const std::string &input);

}  // namespace cypher

#endif  // RGRAPH_AST_BUILDER_H
