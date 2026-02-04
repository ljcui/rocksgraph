#include "ast_builder.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"
#include "ast_exception.h"
#include "common/exception.h"
#include "rewriters/rewriter_pipeline.h"
#include "semantic_validator.h"

namespace ast {

using common::InternalError;

namespace {

class ErrorCollector : public antlr4::BaseErrorListener {
 public:
  std::vector<std::string> errors;

  void syntaxError(antlr4::Recognizer *recognizer,
                   antlr4::Token *offendingSymbol, size_t line,
                   size_t charPositionInLine, const std::string &msg,
                   std::exception_ptr e) override {
    (void)recognizer;
    (void)offendingSymbol;
    (void)e;
    std::ostringstream oss;
    oss << "line " << line << ":" << charPositionInLine << " " << msg;
    errors.push_back(oss.str());
  }
};

static std::string unescapeSymbolicName(const std::string &text) {
  if (text.empty()) {
    return text;
  }
  if (text.front() != '`' || text.back() != '`') {
    return text;
  }
  std::string out;
  out.reserve(text.size());
  for (size_t i = 1; i + 1 < text.size(); ++i) {
    char c = text[i];
    if (c == '`') {
      if (i + 1 < text.size() - 1 && text[i + 1] == '`') {
        out.push_back('`');
        ++i;
      }
      continue;
    }
    out.push_back(c);
  }
  return out;
}

static int64_t parseIntegerLiteral(const std::string &text) {
  int base = 10;
  size_t start = 0;
  if (text.size() > 2 && (text[0] == '0') &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    start = 2;
  } else if (text.size() > 2 && (text[0] == '0') &&
             (text[1] == 'o' || text[1] == 'O')) {
    base = 8;
    start = 2;
  }
  return std::stoll(text.substr(start), nullptr, base);
}

static void appendUtf8(uint32_t codepoint, std::string &out) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

static std::string parseStringLiteral(const std::string &text) {
  if (text.size() < 2) {
    return text;
  }
  const char quote = text.front();
  std::string out;
  out.reserve(text.size() - 2);
  for (size_t i = 1; i + 1 < text.size(); ++i) {
    char c = text[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= text.size() - 1) {
      break;
    }
    char esc = text[++i];
    switch (esc) {
      case '\\':
        out.push_back('\\');
        break;
      case '\'':
        out.push_back('\'');
        break;
      case '"':
        out.push_back('"');
        break;
      case 'b':
      case 'B':
        out.push_back('\b');
        break;
      case 'f':
      case 'F':
        out.push_back('\f');
        break;
      case 'n':
      case 'N':
        out.push_back('\n');
        break;
      case 'r':
      case 'R':
        out.push_back('\r');
        break;
      case 't':
      case 'T':
        out.push_back('\t');
        break;
      case 'u':
      case 'U': {
        const size_t len = (esc == 'u') ? 4 : 8;
        if (i + len >= text.size()) {
          break;
        }
        uint32_t codepoint = 0;
        for (size_t j = 0; j < len; ++j) {
          char h = text[i + 1 + j];
          codepoint <<= 4;
          if (h >= '0' && h <= '9') {
            codepoint |= static_cast<uint32_t>(h - '0');
          } else if (h >= 'a' && h <= 'f') {
            codepoint |= static_cast<uint32_t>(h - 'a' + 10);
          } else if (h >= 'A' && h <= 'F') {
            codepoint |= static_cast<uint32_t>(h - 'A' + 10);
          } else {
            codepoint = 0;
            break;
          }
        }
        i += len;
        appendUtf8(codepoint, out);
        break;
      }
      default:
        out.push_back(esc);
        break;
    }
  }
  (void)quote;
  return out;
}

class ASTBuilder {
 public:
  std::unique_ptr<Statement> buildStatement(
      CypherParser::OC_StatementContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    auto query = buildQuery(ctx->oC_Query());
    return std::unique_ptr<Statement>(query.release());
  }

  std::unique_ptr<Query> buildQuery(CypherParser::OC_QueryContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    if (ctx->oC_RegularQuery()) {
      return buildRegularQuery(ctx->oC_RegularQuery());
    }
    if (ctx->oC_StandaloneCall()) {
      return buildStandaloneCall(ctx->oC_StandaloneCall());
    }
    return nullptr;
  }

  std::unique_ptr<RegularQuery> buildRegularQuery(
      CypherParser::OC_RegularQueryContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    auto node = std::make_unique<RegularQuery>();
    node->single_query = buildSingleQuery(ctx->oC_SingleQuery());
    for (auto *u : ctx->oC_Union()) {
      node->unions.push_back(buildUnionPart(u));
    }
    return node;
  }

  std::unique_ptr<UnionPart> buildUnionPart(
      CypherParser::OC_UnionContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    auto node = std::make_unique<UnionPart>();
    node->all = ctx->ALL() != nullptr;
    node->query = buildSingleQuery(ctx->oC_SingleQuery());
    return node;
  }

  std::unique_ptr<SingleQuery> buildSingleQuery(
      CypherParser::OC_SingleQueryContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    if (ctx->oC_SinglePartQuery()) {
      return buildSinglePartQuery(ctx->oC_SinglePartQuery());
    }
    if (ctx->oC_MultiPartQuery()) {
      return buildMultiPartQuery(ctx->oC_MultiPartQuery());
    }
    return nullptr;
  }

  std::unique_ptr<SinglePartQuery> buildSinglePartQuery(
      CypherParser::OC_SinglePartQueryContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    auto node = std::make_unique<SinglePartQuery>();
    for (auto *rc : ctx->oC_ReadingClause()) {
      node->reading_clauses.push_back(buildReadingClause(rc));
    }
    for (auto *uc : ctx->oC_UpdatingClause()) {
      node->updating_clauses.push_back(buildUpdatingClause(uc));
    }
    if (ctx->oC_Return()) {
      node->return_clause = buildReturn(ctx->oC_Return());
    }
    return node;
  }

  std::unique_ptr<MultiPartQuery> buildMultiPartQuery(
      CypherParser::OC_MultiPartQueryContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    auto node = std::make_unique<MultiPartQuery>();
    std::vector<std::unique_ptr<ReadingClause>> reading;
    std::vector<std::unique_ptr<UpdatingClause>> updating;
    for (auto *child : ctx->children) {
      if (auto *rc =
              dynamic_cast<CypherParser::OC_ReadingClauseContext *>(child)) {
        reading.push_back(buildReadingClause(rc));
        continue;
      }
      if (auto *uc =
              dynamic_cast<CypherParser::OC_UpdatingClauseContext *>(child)) {
        updating.push_back(buildUpdatingClause(uc));
        continue;
      }
      if (auto *wc = dynamic_cast<CypherParser::OC_WithContext *>(child)) {
        MultiPartQuery::WithPart part;
        part.reading_clauses = std::move(reading);
        part.updating_clauses = std::move(updating);
        part.with_clause = buildWith(wc);
        node->parts.push_back(std::move(part));
        reading.clear();
        updating.clear();
        continue;
      }
      if (auto *spq =
              dynamic_cast<CypherParser::OC_SinglePartQueryContext *>(child)) {
        node->final_single_part_query = buildSinglePartQuery(spq);
      }
    }
    return node;
  }

  std::unique_ptr<ReadingClause> buildReadingClause(
      CypherParser::OC_ReadingClauseContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    if (ctx->oC_Match()) {
      return buildMatch(ctx->oC_Match());
    }
    if (ctx->oC_Unwind()) {
      return buildUnwind(ctx->oC_Unwind());
    }
    if (ctx->oC_InQueryCall()) {
      return buildInQueryCall(ctx->oC_InQueryCall());
    }
    return nullptr;
  }

  std::unique_ptr<UpdatingClause> buildUpdatingClause(
      CypherParser::OC_UpdatingClauseContext *ctx) {
    if (!ctx) {
      return nullptr;
    }
    if (ctx->oC_Create()) {
      return buildCreate(ctx->oC_Create());
    }
    if (ctx->oC_Merge()) {
      return buildMerge(ctx->oC_Merge());
    }
    if (ctx->oC_Delete()) {
      return buildDelete(ctx->oC_Delete());
    }
    if (ctx->oC_Set()) {
      return buildSet(ctx->oC_Set());
    }
    if (ctx->oC_Remove()) {
      return buildRemove(ctx->oC_Remove());
    }
    return nullptr;
  }

  std::unique_ptr<Match> buildMatch(CypherParser::OC_MatchContext *ctx) {
    auto node = std::make_unique<Match>();
    node->optional_match = ctx->OPTIONAL() != nullptr;
    node->pattern = buildPattern(ctx->oC_Pattern());
    if (ctx->oC_Where()) {
      node->where = buildExpression(ctx->oC_Where()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Unwind> buildUnwind(CypherParser::OC_UnwindContext *ctx) {
    auto node = std::make_unique<Unwind>();
    node->expression = buildExpression(ctx->oC_Expression());
    node->variable = parseVariable(ctx->oC_Variable());
    return node;
  }

  std::unique_ptr<InQueryCall> buildInQueryCall(
      CypherParser::OC_InQueryCallContext *ctx) {
    auto node = std::make_unique<InQueryCall>();
    auto *call = ctx->oC_ExplicitProcedureInvocation();
    node->procedure_name = parseProcedureName(call->oC_ProcedureName());
    for (auto *arg : call->oC_Expression()) {
      node->arguments.push_back(buildExpression(arg));
    }
    if (ctx->oC_YieldItems()) {
      parseYieldItems(ctx->oC_YieldItems(), node->yield_items,
                      node->yield_where);
    }
    return node;
  }

  std::unique_ptr<StandaloneCall> buildStandaloneCall(
      CypherParser::OC_StandaloneCallContext *ctx) {
    auto node = std::make_unique<StandaloneCall>();
    if (ctx->oC_ExplicitProcedureInvocation()) {
      auto *call = ctx->oC_ExplicitProcedureInvocation();
      node->procedure_name = parseProcedureName(call->oC_ProcedureName());
      for (auto *arg : call->oC_Expression()) {
        node->arguments.push_back(buildExpression(arg));
      }
    } else if (ctx->oC_ImplicitProcedureInvocation()) {
      auto *call = ctx->oC_ImplicitProcedureInvocation();
      node->procedure_name = parseProcedureName(call->oC_ProcedureName());
    }
    if (ctx->YIELD()) {
      if (ctx->oC_YieldItems()) {
        parseYieldItems(ctx->oC_YieldItems(), node->yield_items,
                        node->yield_where);
      } else {
        node->yield_star = true;
      }
    }
    return node;
  }

  std::unique_ptr<Create> buildCreate(CypherParser::OC_CreateContext *ctx) {
    auto node = std::make_unique<Create>();
    node->pattern = buildPattern(ctx->oC_Pattern());
    return node;
  }

  std::unique_ptr<Merge> buildMerge(CypherParser::OC_MergeContext *ctx) {
    auto node = std::make_unique<Merge>();
    node->pattern_part = buildPatternPart(ctx->oC_PatternPart());
    for (auto *action : ctx->oC_MergeAction()) {
      bool on_match = action->MATCH() != nullptr;
      node->actions.emplace_back(on_match, buildSet(action->oC_Set()));
    }
    return node;
  }

  std::unique_ptr<Delete> buildDelete(CypherParser::OC_DeleteContext *ctx) {
    auto node = std::make_unique<Delete>();
    node->detach = ctx->DETACH() != nullptr;
    for (auto *expr : ctx->oC_Expression()) {
      node->expressions.push_back(buildExpression(expr));
    }
    return node;
  }

  std::unique_ptr<Set> buildSet(CypherParser::OC_SetContext *ctx) {
    auto node = std::make_unique<Set>();
    for (auto *item : ctx->oC_SetItem()) {
      node->items.push_back(buildSetItem(item));
    }
    return node;
  }

  std::unique_ptr<SetItem> buildSetItem(CypherParser::OC_SetItemContext *ctx) {
    auto node = std::make_unique<SetItem>();
    if (ctx->oC_PropertyExpression()) {
      node->type = SetItem::Type::Property;
      node->target = buildPropertyExpression(ctx->oC_PropertyExpression());
      node->value = buildExpression(ctx->oC_Expression());
      node->plus_equal = false;
      return node;
    }
    if (ctx->oC_NodeLabels()) {
      node->type = SetItem::Type::Labels;
      node->target = buildVariableExpression(ctx->oC_Variable());
      node->labels = buildNodeLabels(ctx->oC_NodeLabels());
      node->plus_equal = false;
      return node;
    }
    node->type = SetItem::Type::Variable;
    node->target = buildVariableExpression(ctx->oC_Variable());
    node->value = buildExpression(ctx->oC_Expression());
    node->plus_equal = hasOperator(ctx, "+=");
    return node;
  }

  std::unique_ptr<Remove> buildRemove(CypherParser::OC_RemoveContext *ctx) {
    auto node = std::make_unique<Remove>();
    for (auto *item : ctx->oC_RemoveItem()) {
      node->items.push_back(buildRemoveItem(item));
    }
    return node;
  }

  std::unique_ptr<RemoveItem> buildRemoveItem(
      CypherParser::OC_RemoveItemContext *ctx) {
    auto node = std::make_unique<RemoveItem>();
    if (ctx->oC_PropertyExpression()) {
      node->type = RemoveItem::Type::Property;
      node->target = buildPropertyExpression(ctx->oC_PropertyExpression());
      return node;
    }
    node->type = RemoveItem::Type::Labels;
    node->target = buildVariableExpression(ctx->oC_Variable());
    node->labels = buildNodeLabels(ctx->oC_NodeLabels());
    return node;
  }

  std::unique_ptr<With> buildWith(CypherParser::OC_WithContext *ctx) {
    auto node = std::make_unique<With>();
    node->body = buildProjectionBody(ctx->oC_ProjectionBody());
    if (ctx->oC_Where()) {
      node->where = buildExpression(ctx->oC_Where()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Return> buildReturn(CypherParser::OC_ReturnContext *ctx) {
    auto node = std::make_unique<Return>();
    node->body = buildProjectionBody(ctx->oC_ProjectionBody());
    return node;
  }

  std::unique_ptr<ProjectionBody> buildProjectionBody(
      CypherParser::OC_ProjectionBodyContext *ctx) {
    auto node = std::make_unique<ProjectionBody>();
    node->distinct = ctx->DISTINCT() != nullptr;
    auto *items_ctx = ctx->oC_ProjectionItems();
    if (items_ctx) {
      node->star = hasStar(items_ctx);
      for (auto *item : items_ctx->oC_ProjectionItem()) {
        node->items.push_back(buildProjectionItem(item));
      }
    }
    if (ctx->oC_Order()) {
      for (auto *item : ctx->oC_Order()->oC_SortItem()) {
        node->order_by.push_back(buildSortItem(item));
      }
    }
    if (ctx->oC_Skip()) {
      node->skip = buildExpression(ctx->oC_Skip()->oC_Expression());
    }
    if (ctx->oC_Limit()) {
      node->limit = buildExpression(ctx->oC_Limit()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<ProjectionItem> buildProjectionItem(
      CypherParser::OC_ProjectionItemContext *ctx) {
    auto node = std::make_unique<ProjectionItem>();
    node->expression = buildExpression(ctx->oC_Expression());
    if (ctx->oC_Variable()) {
      node->alias = parseVariable(ctx->oC_Variable());
    }
    return node;
  }

  std::unique_ptr<SortItem> buildSortItem(
      CypherParser::OC_SortItemContext *ctx) {
    auto node = std::make_unique<SortItem>();
    node->expression = buildExpression(ctx->oC_Expression());
    if (ctx->ASC() || ctx->ASCENDING()) {
      node->ascending = true;
    }
    if (ctx->DESC() || ctx->DESCENDING()) {
      node->ascending = false;
    }
    return node;
  }

  std::unique_ptr<Pattern> buildPattern(CypherParser::OC_PatternContext *ctx) {
    auto node = std::make_unique<Pattern>();
    for (auto *part : ctx->oC_PatternPart()) {
      node->parts.push_back(buildPatternPart(part));
    }
    return node;
  }

  std::unique_ptr<PatternPart> buildPatternPart(
      CypherParser::OC_PatternPartContext *ctx) {
    auto node = std::make_unique<PatternPart>();
    if (ctx->oC_Variable()) {
      node->variable = parseVariable(ctx->oC_Variable());
    }
    node->element = buildPatternElement(
        ctx->oC_AnonymousPatternPart()->oC_PatternElement());
    return node;
  }

  std::unique_ptr<PatternElement> buildPatternElement(
      CypherParser::OC_PatternElementContext *ctx) {
    if (ctx->oC_PatternElement()) {
      return buildPatternElement(ctx->oC_PatternElement());
    }
    auto node = std::make_unique<PatternElement>();
    node->node_pattern = buildNodePattern(ctx->oC_NodePattern());
    for (auto *chain_ctx : ctx->oC_PatternElementChain()) {
      auto rel = buildRelationshipPattern(chain_ctx->oC_RelationshipPattern());
      auto np = buildNodePattern(chain_ctx->oC_NodePattern());
      node->chain.emplace_back(std::move(rel), std::move(np));
    }
    return node;
  }

  std::unique_ptr<RelationshipsPattern> buildRelationshipsPattern(
      CypherParser::OC_RelationshipsPatternContext *ctx) {
    auto node = std::make_unique<RelationshipsPattern>();
    node->node_pattern = buildNodePattern(ctx->oC_NodePattern());
    for (auto *chain_ctx : ctx->oC_PatternElementChain()) {
      auto rel = buildRelationshipPattern(chain_ctx->oC_RelationshipPattern());
      auto np = buildNodePattern(chain_ctx->oC_NodePattern());
      node->chain.emplace_back(std::move(rel), std::move(np));
    }
    return node;
  }

  std::unique_ptr<NodePattern> buildNodePattern(
      CypherParser::OC_NodePatternContext *ctx) {
    auto node = std::make_unique<NodePattern>();
    if (ctx->oC_Variable()) {
      node->variable = parseVariable(ctx->oC_Variable());
    }
    if (ctx->oC_NodeLabels()) {
      node->labels = buildNodeLabels(ctx->oC_NodeLabels());
    }
    if (ctx->oC_Properties()) {
      node->properties = buildProperties(ctx->oC_Properties());
    }
    return node;
  }

  std::unique_ptr<RelationshipPattern> buildRelationshipPattern(
      CypherParser::OC_RelationshipPatternContext *ctx) {
    auto node = std::make_unique<RelationshipPattern>();
    node->left_arrow = ctx->oC_LeftArrowHead() != nullptr;
    node->right_arrow = ctx->oC_RightArrowHead() != nullptr;
    if (ctx->oC_RelationshipDetail()) {
      node->detail = buildRelationshipDetail(ctx->oC_RelationshipDetail());
    }
    return node;
  }

  std::unique_ptr<RelationshipDetail> buildRelationshipDetail(
      CypherParser::OC_RelationshipDetailContext *ctx) {
    auto node = std::make_unique<RelationshipDetail>();
    if (ctx->oC_Variable()) {
      node->variable = parseVariable(ctx->oC_Variable());
    }
    if (ctx->oC_RelationshipTypes()) {
      for (auto *rt : ctx->oC_RelationshipTypes()->oC_RelTypeName()) {
        node->types.push_back(parseSchemaName(rt->oC_SchemaName()));
      }
    }
    if (ctx->oC_RangeLiteral()) {
      node->range = buildRangeLiteral(ctx->oC_RangeLiteral());
    }
    if (ctx->oC_Properties()) {
      node->properties = buildProperties(ctx->oC_Properties());
    }
    return node;
  }

  RelationshipDetail::RangeLiteral buildRangeLiteral(
      CypherParser::OC_RangeLiteralContext *ctx) {
    RelationshipDetail::RangeLiteral range;
    int dot_index = -1;
    for (auto *child : ctx->children) {
      if (child->getText() == "..") {
        dot_index = static_cast<int>(child->getSourceInterval().a);
        break;
      }
    }
    if (ctx->oC_IntegerLiteral().empty()) {
      return range;
    }
    if (dot_index < 0) {
      range.min = parseIntegerLiteral(ctx->oC_IntegerLiteral(0)->getText());
      return range;
    }
    for (auto *lit : ctx->oC_IntegerLiteral()) {
      int tok_index = static_cast<int>(lit->getStart()->getTokenIndex());
      if (tok_index < dot_index) {
        range.min = parseIntegerLiteral(lit->getText());
      } else {
        range.max = parseIntegerLiteral(lit->getText());
      }
    }
    return range;
  }

  std::unique_ptr<Properties> buildProperties(
      CypherParser::OC_PropertiesContext *ctx) {
    auto node = std::make_unique<Properties>();
    if (ctx->oC_MapLiteral()) {
      node->map = buildMapLiteral(ctx->oC_MapLiteral());
    } else if (ctx->oC_Parameter()) {
      node->parameter = buildParameter(ctx->oC_Parameter());
    }
    return node;
  }

  std::unique_ptr<Expression> buildExpression(
      CypherParser::OC_ExpressionContext *ctx) {
    return buildOrExpression(ctx->oC_OrExpression());
  }

  std::unique_ptr<Expression> buildOrExpression(
      CypherParser::OC_OrExpressionContext *ctx) {
    auto parts = ctx->oC_XorExpression();
    auto expr = buildXorExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<OrExpression>();
      node->left = std::move(expr);
      node->right = buildXorExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> buildXorExpression(
      CypherParser::OC_XorExpressionContext *ctx) {
    auto parts = ctx->oC_AndExpression();
    auto expr = buildAndExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<XorExpression>();
      node->left = std::move(expr);
      node->right = buildAndExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> buildAndExpression(
      CypherParser::OC_AndExpressionContext *ctx) {
    auto parts = ctx->oC_NotExpression();
    auto expr = buildNotExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<AndExpression>();
      node->left = std::move(expr);
      node->right = buildNotExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> buildNotExpression(
      CypherParser::OC_NotExpressionContext *ctx) {
    auto expr = buildComparisonExpression(ctx->oC_ComparisonExpression());
    for (size_t i = 0; i < ctx->NOT().size(); ++i) {
      auto node = std::make_unique<NotExpression>();
      node->operand = std::move(expr);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> buildComparisonExpression(
      CypherParser::OC_ComparisonExpressionContext *ctx) {
    auto left = buildStringListNullPredicateExpression(
        ctx->oC_StringListNullPredicateExpression());
    auto parts = ctx->oC_PartialComparisonExpression();
    if (parts.empty()) {
      return left;
    }
    if (parts.size() == 1) {
      auto node = std::make_unique<ComparisonExpression>();
      node->left = std::move(left);
      node->op = extractComparisonOp(parts[0]);
      node->right = buildStringListNullPredicateExpression(
          parts[0]->oC_StringListNullPredicateExpression());
      return node;
    }
    auto node = std::make_unique<ComparisonChainExpression>();
    node->left = std::move(left);
    for (auto *part : parts) {
      node->rights.emplace_back(
          extractComparisonOp(part),
          buildStringListNullPredicateExpression(
              part->oC_StringListNullPredicateExpression()));
    }
    return node;
  }

  std::unique_ptr<Expression> buildStringListNullPredicateExpression(
      CypherParser::OC_StringListNullPredicateExpressionContext *ctx) {
    auto expr = buildAddOrSubtractExpression(ctx->oC_AddOrSubtractExpression());
    for (auto *child : ctx->children) {
      if (auto *string_pred =
              dynamic_cast<CypherParser::OC_StringPredicateExpressionContext *>(
                  child)) {
        auto node = std::make_unique<StringPredicateExpression>();
        node->left = std::move(expr);
        node->op = extractStringPredicateOp(string_pred);
        node->right = buildAddOrSubtractExpression(
            string_pred->oC_AddOrSubtractExpression());
        expr = std::move(node);
        continue;
      }
      if (auto *list_pred =
              dynamic_cast<CypherParser::OC_ListPredicateExpressionContext *>(
                  child)) {
        auto node = std::make_unique<ListPredicateExpression>();
        node->element = std::move(expr);
        node->list = buildAddOrSubtractExpression(
            list_pred->oC_AddOrSubtractExpression());
        expr = std::move(node);
        continue;
      }
      if (auto *null_pred =
              dynamic_cast<CypherParser::OC_NullPredicateExpressionContext *>(
                  child)) {
        auto node = std::make_unique<NullPredicateExpression>();
        node->operand = std::move(expr);
        node->is_null = !hasOperator(null_pred, "NOT");
        expr = std::move(node);
        continue;
      }
    }
    return expr;
  }

  std::unique_ptr<Expression> buildAddOrSubtractExpression(
      CypherParser::OC_AddOrSubtractExpressionContext *ctx) {
    auto parts = ctx->oC_MultiplyDivideModuloExpression();
    auto expr = buildMultiplyDivideModuloExpression(parts[0]);
    size_t expr_index = 1;
    for (auto *child : ctx->children) {
      if (child->getText() == "+" || child->getText() == "-") {
        auto rhs = buildMultiplyDivideModuloExpression(parts[expr_index++]);
        if (child->getText() == "+") {
          auto node = std::make_unique<AddExpression>();
          node->left = std::move(expr);
          node->right = std::move(rhs);
          expr = std::move(node);
        } else {
          auto node = std::make_unique<SubtractExpression>();
          node->left = std::move(expr);
          node->right = std::move(rhs);
          expr = std::move(node);
        }
      }
    }
    return expr;
  }

  std::unique_ptr<Expression> buildMultiplyDivideModuloExpression(
      CypherParser::OC_MultiplyDivideModuloExpressionContext *ctx) {
    auto parts = ctx->oC_PowerOfExpression();
    auto expr = buildPowerOfExpression(parts[0]);
    size_t expr_index = 1;
    for (auto *child : ctx->children) {
      const std::string text = child->getText();
      if (text == "*" || text == "/" || text == "%") {
        auto rhs = buildPowerOfExpression(parts[expr_index++]);
        if (text == "*") {
          auto node = std::make_unique<MultiplyExpression>();
          node->left = std::move(expr);
          node->right = std::move(rhs);
          expr = std::move(node);
        } else if (text == "/") {
          auto node = std::make_unique<DivideExpression>();
          node->left = std::move(expr);
          node->right = std::move(rhs);
          expr = std::move(node);
        } else {
          auto node = std::make_unique<ModuloExpression>();
          node->left = std::move(expr);
          node->right = std::move(rhs);
          expr = std::move(node);
        }
      }
    }
    return expr;
  }

  std::unique_ptr<Expression> buildPowerOfExpression(
      CypherParser::OC_PowerOfExpressionContext *ctx) {
    auto parts = ctx->oC_UnaryAddOrSubtractExpression();
    auto expr = buildUnaryAddOrSubtractExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<PowerExpression>();
      node->left = std::move(expr);
      node->right = buildUnaryAddOrSubtractExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> buildUnaryAddOrSubtractExpression(
      CypherParser::OC_UnaryAddOrSubtractExpressionContext *ctx) {
    auto base = buildNonArithmeticOperatorExpression(
        ctx->oC_NonArithmeticOperatorExpression());
    if (!ctx->children.empty()) {
      const std::string op = ctx->children.front()->getText();
      if (op == "+") {
        auto node = std::make_unique<UnaryPlusExpression>();
        node->operand = std::move(base);
        return node;
      }
      if (op == "-") {
        auto node = std::make_unique<UnaryMinusExpression>();
        node->operand = std::move(base);
        return node;
      }
    }
    return base;
  }

  std::unique_ptr<Expression> buildNonArithmeticOperatorExpression(
      CypherParser::OC_NonArithmeticOperatorExpressionContext *ctx) {
    auto expr = buildAtom(ctx->oC_Atom());
    for (auto *child : ctx->children) {
      if (auto *list_op =
              dynamic_cast<CypherParser::OC_ListOperatorExpressionContext *>(
                  child)) {
        expr = applyListOperator(std::move(expr), list_op);
      } else if (auto *prop =
                     dynamic_cast<CypherParser::OC_PropertyLookupContext *>(
                         child)) {
        expr = applyPropertyLookup(std::move(expr), prop);
      }
    }
    if (ctx->oC_NodeLabels()) {
      auto label_node = std::make_unique<LabelPredicateExpression>();
      label_node->expr = std::move(expr);
      label_node->labels = buildNodeLabels(ctx->oC_NodeLabels());
      expr = std::move(label_node);
    }
    return expr;
  }

  std::unique_ptr<Expression> buildAtom(CypherParser::OC_AtomContext *ctx) {
    if (ctx->oC_Literal()) {
      return buildLiteral(ctx->oC_Literal());
    }
    if (ctx->oC_Parameter()) {
      return buildParameter(ctx->oC_Parameter());
    }
    if (ctx->oC_CaseExpression()) {
      return buildCaseExpression(ctx->oC_CaseExpression());
    }
    if (ctx->COUNT()) {
      return std::make_unique<CountStarExpression>();
    }
    if (ctx->oC_ListComprehension()) {
      return buildListComprehension(ctx->oC_ListComprehension());
    }
    if (ctx->oC_PatternComprehension()) {
      return buildPatternComprehension(ctx->oC_PatternComprehension());
    }
    if (ctx->oC_Quantifier()) {
      return buildQuantifier(ctx->oC_Quantifier());
    }
    if (ctx->oC_PatternPredicate()) {
      auto node = std::make_unique<PatternPredicateExpression>();
      node->relationships_pattern = buildRelationshipsPattern(
          ctx->oC_PatternPredicate()->oC_RelationshipsPattern());
      return node;
    }
    if (ctx->oC_ParenthesizedExpression()) {
      auto node = std::make_unique<ParenthesizedExpression>();
      node->expr =
          buildExpression(ctx->oC_ParenthesizedExpression()->oC_Expression());
      return node;
    }
    if (ctx->oC_FunctionInvocation()) {
      return buildFunctionInvocation(ctx->oC_FunctionInvocation());
    }
    if (ctx->oC_ExistentialSubquery()) {
      return buildExistentialSubquery(ctx->oC_ExistentialSubquery());
    }
    if (ctx->oC_Variable()) {
      return buildVariableExpression(ctx->oC_Variable());
    }
    return nullptr;
  }

  std::unique_ptr<Literal> buildLiteral(CypherParser::OC_LiteralContext *ctx) {
    if (ctx->oC_BooleanLiteral()) {
      auto node = std::make_unique<BooleanLiteral>();
      node->value = ctx->oC_BooleanLiteral()->TRUE() != nullptr;
      return node;
    }
    if (ctx->NULL_()) {
      return std::make_unique<NullLiteral>();
    }
    if (ctx->oC_NumberLiteral()) {
      if (ctx->oC_NumberLiteral()->oC_DoubleLiteral()) {
        auto node = std::make_unique<DoubleLiteral>();
        node->value =
            std::stod(ctx->oC_NumberLiteral()->oC_DoubleLiteral()->getText());
        return node;
      }
      if (ctx->oC_NumberLiteral()->oC_IntegerLiteral()) {
        auto node = std::make_unique<IntegerLiteral>();
        node->value = parseIntegerLiteral(
            ctx->oC_NumberLiteral()->oC_IntegerLiteral()->getText());
        return node;
      }
    }
    if (ctx->StringLiteral()) {
      auto node = std::make_unique<StringLiteral>();
      node->value = parseStringLiteral(ctx->StringLiteral()->getText());
      return node;
    }
    if (ctx->oC_ListLiteral()) {
      auto node = std::make_unique<ListLiteral>();
      for (auto *expr : ctx->oC_ListLiteral()->oC_Expression()) {
        node->elements.push_back(buildExpression(expr));
      }
      return node;
    }
    if (ctx->oC_MapLiteral()) {
      return buildMapLiteral(ctx->oC_MapLiteral());
    }
    return nullptr;
  }

  std::unique_ptr<MapLiteral> buildMapLiteral(
      CypherParser::OC_MapLiteralContext *ctx) {
    auto node = std::make_unique<MapLiteral>();
    auto keys = ctx->oC_PropertyKeyName();
    auto values = ctx->oC_Expression();
    const size_t count = std::min(keys.size(), values.size());
    for (size_t i = 0; i < count; ++i) {
      node->entries.emplace_back(parsePropertyKeyName(keys[i]),
                                 buildExpression(values[i]));
    }
    return node;
  }

  std::unique_ptr<Parameter> buildParameter(
      CypherParser::OC_ParameterContext *ctx) {
    auto node = std::make_unique<Parameter>();
    if (ctx->oC_SymbolicName()) {
      node->name = parseSymbolicName(ctx->oC_SymbolicName());
    } else if (ctx->DecimalInteger()) {
      node->name = ctx->DecimalInteger()->getText();
    }
    return node;
  }

  std::unique_ptr<Expression> buildPropertyExpression(
      CypherParser::OC_PropertyExpressionContext *ctx) {
    auto expr = buildAtom(ctx->oC_Atom());
    for (auto *lookup : ctx->oC_PropertyLookup()) {
      expr = applyPropertyLookup(std::move(expr), lookup);
    }
    return expr;
  }

  std::unique_ptr<Variable> buildVariableExpression(
      CypherParser::OC_VariableContext *ctx) {
    auto node = std::make_unique<Variable>();
    node->name = parseVariable(ctx);
    return node;
  }

  std::unique_ptr<Expression> buildCaseExpression(
      CypherParser::OC_CaseExpressionContext *ctx) {
    auto node = std::make_unique<CaseExpression>();
    for (auto *alt : ctx->oC_CaseAlternative()) {
      auto when_expr = buildExpression(alt->oC_Expression(0));
      auto then_expr = buildExpression(alt->oC_Expression(1));
      node->alternatives.emplace_back(std::move(when_expr),
                                      std::move(then_expr));
    }
    std::vector<CypherParser::OC_ExpressionContext *> parent_exprs;
    for (auto *expr_ctx : ctx->oC_Expression()) {
      if (expr_ctx->parent == ctx) {
        parent_exprs.push_back(expr_ctx);
      }
    }
    if (parent_exprs.size() == 1) {
      if (ctx->ELSE()) {
        node->else_expr = buildExpression(parent_exprs[0]);
      } else {
        node->test = buildExpression(parent_exprs[0]);
      }
    } else if (parent_exprs.size() >= 2) {
      auto *first = parent_exprs[0];
      auto *second = parent_exprs[1];
      if (first->getStart()->getTokenIndex() <
          second->getStart()->getTokenIndex()) {
        node->test = buildExpression(first);
        node->else_expr = buildExpression(second);
      } else {
        node->test = buildExpression(second);
        node->else_expr = buildExpression(first);
      }
    }
    return node;
  }

  std::unique_ptr<Expression> buildListComprehension(
      CypherParser::OC_ListComprehensionContext *ctx) {
    auto node = std::make_unique<ListComprehension>();
    auto *filter = ctx->oC_FilterExpression();
    auto *id_in_coll = filter->oC_IdInColl();
    node->variable = parseVariable(id_in_coll->oC_Variable());
    node->list_expr = buildExpression(id_in_coll->oC_Expression());
    if (filter->oC_Where()) {
      node->where_expr = buildExpression(filter->oC_Where()->oC_Expression());
    }
    if (ctx->oC_Expression()) {
      node->eval_expr = buildExpression(ctx->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Expression> buildPatternComprehension(
      CypherParser::OC_PatternComprehensionContext *ctx) {
    auto node = std::make_unique<PatternComprehension>();
    if (ctx->oC_Variable()) {
      node->variable = parseVariable(ctx->oC_Variable());
    }
    node->relationships_pattern =
        buildRelationshipsPattern(ctx->oC_RelationshipsPattern());
    if (ctx->oC_Where()) {
      node->where_expr = buildExpression(ctx->oC_Where()->oC_Expression());
    }
    node->eval_expr = buildExpression(ctx->oC_Expression());
    return node;
  }

  std::unique_ptr<Expression> buildQuantifier(
      CypherParser::OC_QuantifierContext *ctx) {
    auto *filter = ctx->oC_FilterExpression();
    auto *id_in_coll = filter->oC_IdInColl();
    std::unique_ptr<Quantifier> node;
    if (ctx->ALL()) {
      node = std::make_unique<AllQuantifier>();
    } else if (ctx->ANY()) {
      node = std::make_unique<AnyQuantifier>();
    } else if (ctx->NONE()) {
      node = std::make_unique<NoneQuantifier>();
    } else {
      node = std::make_unique<SingleQuantifier>();
    }
    node->variable = parseVariable(id_in_coll->oC_Variable());
    node->list_expr = buildExpression(id_in_coll->oC_Expression());
    if (filter->oC_Where()) {
      node->predicate = buildExpression(filter->oC_Where()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Expression> buildFunctionInvocation(
      CypherParser::OC_FunctionInvocationContext *ctx) {
    auto node = std::make_unique<FunctionInvocation>();
    node->function_name =
        parseQualifiedName(ctx->oC_FunctionName()->oC_Namespace(),
                           ctx->oC_FunctionName()->oC_SymbolicName());
    node->distinct = ctx->DISTINCT() != nullptr;
    for (auto *expr : ctx->oC_Expression()) {
      node->arguments.push_back(buildExpression(expr));
    }
    return node;
  }

  std::unique_ptr<Expression> buildExistentialSubquery(
      CypherParser::OC_ExistentialSubqueryContext *ctx) {
    auto node = std::make_unique<ExistentialSubquery>();
    if (ctx->oC_RegularQuery()) {
      node->query = buildRegularQuery(ctx->oC_RegularQuery());
    } else if (ctx->oC_Pattern()) {
      node->pattern = buildPattern(ctx->oC_Pattern());
      if (ctx->oC_Where()) {
        node->where_expr = buildExpression(ctx->oC_Where()->oC_Expression());
      }
    }
    return node;
  }

  std::unique_ptr<Expression> applyPropertyLookup(
      std::unique_ptr<Expression> base,
      CypherParser::OC_PropertyLookupContext *ctx) {
    auto node = std::make_unique<PropertyExpression>();
    node->object = std::move(base);
    node->property_key = parsePropertyKeyName(ctx->oC_PropertyKeyName());
    return node;
  }

  std::unique_ptr<Expression> applyListOperator(
      std::unique_ptr<Expression> base,
      CypherParser::OC_ListOperatorExpressionContext *ctx) {
    bool is_slice = false;
    int dot_index = -1;
    for (auto *child : ctx->children) {
      if (child->getText() == "..") {
        is_slice = true;
        dot_index = static_cast<int>(child->getSourceInterval().a);
        break;
      }
    }
    if (!is_slice) {
      auto node = std::make_unique<ListIndexExpression>();
      node->list = std::move(base);
      node->index = buildExpression(ctx->oC_Expression(0));
      return node;
    }
    auto node = std::make_unique<ListSliceExpression>();
    node->list = std::move(base);
    auto exprs = ctx->oC_Expression();
    if (exprs.size() == 1) {
      auto *expr_ctx = exprs[0];
      int tok_index = static_cast<int>(expr_ctx->getStart()->getTokenIndex());
      if (dot_index >= 0 && tok_index < dot_index) {
        node->start_index = buildExpression(expr_ctx);
      } else {
        node->end_index = buildExpression(expr_ctx);
      }
    } else if (exprs.size() >= 2) {
      node->start_index = buildExpression(exprs[0]);
      node->end_index = buildExpression(exprs[1]);
    }
    return node;
  }

  std::vector<std::string> buildNodeLabels(
      CypherParser::OC_NodeLabelsContext *ctx) {
    std::vector<std::string> labels;
    for (auto *label : ctx->oC_NodeLabel()) {
      labels.push_back(parseSchemaName(label->oC_LabelName()->oC_SchemaName()));
    }
    return labels;
  }

  std::string parseSymbolicName(CypherParser::OC_SymbolicNameContext *ctx) {
    if (!ctx) {
      return {};
    }
    return unescapeSymbolicName(ctx->getText());
  }

  std::string parseSchemaName(CypherParser::OC_SchemaNameContext *ctx) {
    if (ctx->oC_SymbolicName()) {
      return parseSymbolicName(ctx->oC_SymbolicName());
    }
    if (ctx->oC_ReservedWord()) {
      return ctx->oC_ReservedWord()->getText();
    }
    return {};
  }

  std::string parsePropertyKeyName(
      CypherParser::OC_PropertyKeyNameContext *ctx) {
    return parseSchemaName(ctx->oC_SchemaName());
  }

  std::string parseVariable(CypherParser::OC_VariableContext *ctx) {
    return parseSymbolicName(ctx->oC_SymbolicName());
  }

  std::string parseProcedureName(CypherParser::OC_ProcedureNameContext *ctx) {
    return parseQualifiedName(ctx->oC_Namespace(), ctx->oC_SymbolicName());
  }

  std::string parseQualifiedName(CypherParser::OC_NamespaceContext *ns,
                                 CypherParser::OC_SymbolicNameContext *name) {
    std::string out;
    if (ns) {
      for (auto *sym : ns->oC_SymbolicName()) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += parseSymbolicName(sym);
      }
    }
    if (!out.empty()) {
      out.push_back('.');
    }
    out += parseSymbolicName(name);
    return out;
  }

  std::string extractComparisonOp(
      CypherParser::OC_PartialComparisonExpressionContext *ctx) {
    if (!ctx || ctx->children.empty()) {
      return {};
    }
    return ctx->children.front()->getText();
  }

  std::string extractStringPredicateOp(
      CypherParser::OC_StringPredicateExpressionContext *ctx) {
    for (auto *child : ctx->children) {
      const std::string text = toUpperAscii(child->getText());
      if (text == "STARTS" || text == "ENDS") {
        return text + " WITH";
      }
      if (text == "CONTAINS") {
        return text;
      }
    }
    return "CONTAINS";
  }

  void parseYieldItems(CypherParser::OC_YieldItemsContext *ctx,
                       std::vector<StandaloneCall::YieldItem> &items,
                       std::unique_ptr<Expression> &where_expr) {
    for (auto *item_ctx : ctx->oC_YieldItem()) {
      StandaloneCall::YieldItem item;
      if (item_ctx->oC_ProcedureResultField()) {
        item.result_field = parseSymbolicName(
            item_ctx->oC_ProcedureResultField()->oC_SymbolicName());
      }
      item.variable = parseVariable(item_ctx->oC_Variable());
      items.push_back(std::move(item));
    }
    if (ctx->oC_Where()) {
      where_expr = buildExpression(ctx->oC_Where()->oC_Expression());
    }
  }

  static std::string toUpperAscii(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
      if (c >= 'a' && c <= 'z') {
        out.push_back(static_cast<char>(c - 'a' + 'A'));
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  bool hasOperator(antlr4::ParserRuleContext *ctx, const std::string &op) {
    const std::string op_upper = toUpperAscii(op);
    for (auto *child : ctx->children) {
      if (toUpperAscii(child->getText()) == op_upper) {
        return true;
      }
    }
    return false;
  }

  bool hasStar(CypherParser::OC_ProjectionItemsContext *ctx) {
    if (!ctx) {
      return false;
    }
    for (auto *child : ctx->children) {
      if (child->getText() == "*") {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

std::unique_ptr<Statement> parseCypher(const std::string &input) {
  antlr4::ANTLRInputStream input_stream(input);
  CypherLexer lexer(&input_stream);
  antlr4::CommonTokenStream tokens(&lexer);
  CypherParser parser(&tokens);

  ErrorCollector errors;
  lexer.removeErrorListeners();
  parser.removeErrorListeners();
  lexer.addErrorListener(&errors);
  parser.addErrorListener(&errors);

  auto *tree = parser.oC_Cypher();
  if (!errors.errors.empty()) {
    THROW(ParseError, std::move(errors.errors));
  }
  if (!tree || !tree->oC_Statement()) {
    std::vector<std::string> parse_errors;
    parse_errors.emplace_back("failed to parse statement");
    THROW(ParseError, std::move(parse_errors));
  }
  ASTBuilder builder;
  auto statement = builder.buildStatement(tree->oC_Statement());
  if (!statement) {
    THROW(InternalError, "failed to build AST");
  }
  validateStatement(*statement);
  return statement;
}

std::unique_ptr<Statement> parseCypherAndRewrite(const std::string &input) {
  auto statement = parseCypher(input);
  applyDefaultRewriters(*statement);
  return statement;
}

}  // namespace ast
