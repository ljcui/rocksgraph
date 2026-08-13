#include "ast_builder.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "antlr4-runtime.h"
#include "ast_exception.h"
#include "common/exception.h"
#include "cypher/CypherLexer.h"
#include "cypher/CypherParser.h"
#include "rewriters/rewriter_pipeline.h"
#include "semantic_validator.h"

namespace ast {

using common::InternalError;

namespace {

class ErrorCollector : public antlr4::BaseErrorListener {
 public:
  std::vector<std::string> errors;

  void syntaxError(antlr4::Recognizer *recognizer,
                   antlr4::Token *offending_symbol, size_t line,
                   size_t char_position_in_line, const std::string &msg,
                   std::exception_ptr e) override {
    (void)recognizer;
    (void)offending_symbol;
    (void)e;
    std::ostringstream oss;
    oss << "line " << line << ":" << char_position_in_line << " " << msg;
    errors.push_back(oss.str());
  }
};

std::string UnescapeSymbolicName(const std::string &text) {
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

uint64_t ParseIntegerMagnitude(const std::string &text) {
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
  uint64_t value = 0;
  const char *begin = text.data() + start;
  const char *end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value, base);
  if (result.ec == std::errc::result_out_of_range || result.ptr != end) {
    std::vector<std::string> errors = {"integer literal is out of range"};
    THROW(ParseError, std::move(errors));
  }
  return value;
}

int64_t ParseIntegerLiteral(const std::string &text) {
  const uint64_t value = ParseIntegerMagnitude(text);
  if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    std::vector<std::string> errors = {"integer literal is out of range"};
    THROW(ParseError, std::move(errors));
  }
  return static_cast<int64_t>(value);
}

void AppendUtf8(uint32_t codepoint, std::string &out) {
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

std::string ParseStringLiteral(const std::string &text) {
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
        AppendUtf8(codepoint, out);
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
  std::unique_ptr<Statement> BuildStatement(
      CypherParser::OC_StatementContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    auto query = BuildQuery(ctx->oC_Query());
    return std::unique_ptr<Statement>(query.release());
  }

  std::unique_ptr<Query> BuildQuery(CypherParser::OC_QueryContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    if (ctx->oC_RegularQuery() != nullptr) {
      return BuildRegularQuery(ctx->oC_RegularQuery());
    }
    if (ctx->oC_StandaloneCall() != nullptr) {
      return BuildStandaloneCall(ctx->oC_StandaloneCall());
    }
    return nullptr;
  }

  std::unique_ptr<RegularQuery> BuildRegularQuery(
      CypherParser::OC_RegularQueryContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    auto node = std::make_unique<RegularQuery>();
    node->single_query = BuildSingleQuery(ctx->oC_SingleQuery());
    for (auto *u : ctx->oC_Union()) {
      node->unions.push_back(BuildUnionPart(u));
    }
    return node;
  }

  std::unique_ptr<UnionPart> BuildUnionPart(
      CypherParser::OC_UnionContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    auto node = std::make_unique<UnionPart>();
    node->all = ctx->ALL() != nullptr;
    node->query = BuildSingleQuery(ctx->oC_SingleQuery());
    return node;
  }

  std::unique_ptr<SingleQuery> BuildSingleQuery(
      CypherParser::OC_SingleQueryContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    if (ctx->oC_SinglePartQuery() != nullptr) {
      return BuildSinglePartQuery(ctx->oC_SinglePartQuery());
    }
    if (ctx->oC_MultiPartQuery() != nullptr) {
      return BuildMultiPartQuery(ctx->oC_MultiPartQuery());
    }
    return nullptr;
  }

  std::unique_ptr<SinglePartQuery> BuildSinglePartQuery(
      CypherParser::OC_SinglePartQueryContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    auto node = std::make_unique<SinglePartQuery>();
    for (auto *rc : ctx->oC_ReadingClause()) {
      node->reading_clauses.push_back(BuildReadingClause(rc));
    }
    for (auto *uc : ctx->oC_UpdatingClause()) {
      node->updating_clauses.push_back(BuildUpdatingClause(uc));
    }
    if (ctx->oC_Return() != nullptr) {
      node->return_clause = BuildReturn(ctx->oC_Return());
    }
    return node;
  }

  std::unique_ptr<MultiPartQuery> BuildMultiPartQuery(
      CypherParser::OC_MultiPartQueryContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    auto node = std::make_unique<MultiPartQuery>();
    std::vector<std::unique_ptr<ReadingClause>> reading;
    std::vector<std::unique_ptr<UpdatingClause>> updating;
    for (auto *child : ctx->children) {
      if (auto *rc =
              dynamic_cast<CypherParser::OC_ReadingClauseContext *>(child)) {
        reading.push_back(BuildReadingClause(rc));
        continue;
      }
      if (auto *uc =
              dynamic_cast<CypherParser::OC_UpdatingClauseContext *>(child)) {
        updating.push_back(BuildUpdatingClause(uc));
        continue;
      }
      if (auto *wc = dynamic_cast<CypherParser::OC_WithContext *>(child)) {
        MultiPartQuery::WithPart part;
        part.reading_clauses = std::move(reading);
        part.updating_clauses = std::move(updating);
        part.with_clause = BuildWith(wc);
        node->parts.push_back(std::move(part));
        reading.clear();
        updating.clear();
        continue;
      }
      if (auto *spq =
              dynamic_cast<CypherParser::OC_SinglePartQueryContext *>(child)) {
        node->final_single_part_query = BuildSinglePartQuery(spq);
      }
    }
    return node;
  }

  std::unique_ptr<ReadingClause> BuildReadingClause(
      CypherParser::OC_ReadingClauseContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    if (ctx->oC_Match() != nullptr) {
      return BuildMatch(ctx->oC_Match());
    }
    if (ctx->oC_Unwind() != nullptr) {
      return BuildUnwind(ctx->oC_Unwind());
    }
    if (ctx->oC_InQueryCall() != nullptr) {
      return BuildInQueryCall(ctx->oC_InQueryCall());
    }
    return nullptr;
  }

  std::unique_ptr<UpdatingClause> BuildUpdatingClause(
      CypherParser::OC_UpdatingClauseContext *ctx) {
    if (ctx == nullptr) {
      return nullptr;
    }
    if (ctx->oC_Create() != nullptr) {
      return BuildCreate(ctx->oC_Create());
    }
    if (ctx->oC_Merge() != nullptr) {
      return BuildMerge(ctx->oC_Merge());
    }
    if (ctx->oC_Delete() != nullptr) {
      return BuildDelete(ctx->oC_Delete());
    }
    if (ctx->oC_Set() != nullptr) {
      return BuildSet(ctx->oC_Set());
    }
    if (ctx->oC_Remove() != nullptr) {
      return BuildRemove(ctx->oC_Remove());
    }
    return nullptr;
  }

  std::unique_ptr<Match> BuildMatch(CypherParser::OC_MatchContext *ctx) {
    auto node = std::make_unique<Match>();
    node->optional_match = ctx->OPTIONAL() != nullptr;
    node->pattern = BuildPattern(ctx->oC_Pattern());
    if (ctx->oC_Where() != nullptr) {
      node->where = BuildExpression(ctx->oC_Where()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Unwind> BuildUnwind(CypherParser::OC_UnwindContext *ctx) {
    auto node = std::make_unique<Unwind>();
    node->expression = BuildExpression(ctx->oC_Expression());
    node->variable = ParseVariable(ctx->oC_Variable());
    return node;
  }

  std::unique_ptr<InQueryCall> BuildInQueryCall(
      CypherParser::OC_InQueryCallContext *ctx) {
    auto node = std::make_unique<InQueryCall>();
    auto *call = ctx->oC_ExplicitProcedureInvocation();
    node->procedure_name = ParseProcedureName(call->oC_ProcedureName());
    for (auto *arg : call->oC_Expression()) {
      node->arguments.push_back(BuildExpression(arg));
    }
    if (ctx->oC_YieldItems() != nullptr) {
      ParseYieldItems(ctx->oC_YieldItems(), node->yield_items,
                      node->yield_where);
    }
    return node;
  }

  std::unique_ptr<StandaloneCall> BuildStandaloneCall(
      CypherParser::OC_StandaloneCallContext *ctx) {
    auto node = std::make_unique<StandaloneCall>();
    if (ctx->oC_ExplicitProcedureInvocation() != nullptr) {
      auto *call = ctx->oC_ExplicitProcedureInvocation();
      node->procedure_name = ParseProcedureName(call->oC_ProcedureName());
      for (auto *arg : call->oC_Expression()) {
        node->arguments.push_back(BuildExpression(arg));
      }
    } else if (ctx->oC_ImplicitProcedureInvocation() != nullptr) {
      auto *call = ctx->oC_ImplicitProcedureInvocation();
      node->procedure_name = ParseProcedureName(call->oC_ProcedureName());
    }
    if (ctx->YIELD() != nullptr) {
      if (ctx->oC_YieldItems() != nullptr) {
        ParseYieldItems(ctx->oC_YieldItems(), node->yield_items,
                        node->yield_where);
      } else {
        node->yield_star = true;
      }
    }
    return node;
  }

  std::unique_ptr<Create> BuildCreate(CypherParser::OC_CreateContext *ctx) {
    auto node = std::make_unique<Create>();
    node->pattern = BuildPattern(ctx->oC_Pattern());
    return node;
  }

  std::unique_ptr<Merge> BuildMerge(CypherParser::OC_MergeContext *ctx) {
    auto node = std::make_unique<Merge>();
    node->pattern_part = BuildPatternPart(ctx->oC_PatternPart());
    for (auto *action : ctx->oC_MergeAction()) {
      bool on_match = action->MATCH() != nullptr;
      node->actions.emplace_back(on_match, BuildSet(action->oC_Set()));
    }
    return node;
  }

  std::unique_ptr<Delete> BuildDelete(CypherParser::OC_DeleteContext *ctx) {
    auto node = std::make_unique<Delete>();
    node->detach = ctx->DETACH() != nullptr;
    for (auto *expr : ctx->oC_Expression()) {
      node->expressions.push_back(BuildExpression(expr));
    }
    return node;
  }

  std::unique_ptr<Set> BuildSet(CypherParser::OC_SetContext *ctx) {
    auto node = std::make_unique<Set>();
    for (auto *item : ctx->oC_SetItem()) {
      node->items.push_back(BuildSetItem(item));
    }
    return node;
  }

  std::unique_ptr<SetItem> BuildSetItem(CypherParser::OC_SetItemContext *ctx) {
    auto node = std::make_unique<SetItem>();
    if (ctx->oC_PropertyExpression() != nullptr) {
      node->type = SetItem::Type::kProperty;
      node->target = BuildPropertyExpression(ctx->oC_PropertyExpression());
      node->value = BuildExpression(ctx->oC_Expression());
      node->plus_equal = false;
      return node;
    }
    if (ctx->oC_NodeLabels() != nullptr) {
      node->type = SetItem::Type::kLabels;
      node->target = BuildVariableExpression(ctx->oC_Variable());
      node->labels = BuildNodeLabels(ctx->oC_NodeLabels());
      node->plus_equal = false;
      return node;
    }
    node->type = SetItem::Type::kVariable;
    node->target = BuildVariableExpression(ctx->oC_Variable());
    node->value = BuildExpression(ctx->oC_Expression());
    node->plus_equal = HasOperator(ctx, "+=");
    return node;
  }

  std::unique_ptr<Remove> BuildRemove(CypherParser::OC_RemoveContext *ctx) {
    auto node = std::make_unique<Remove>();
    for (auto *item : ctx->oC_RemoveItem()) {
      node->items.push_back(BuildRemoveItem(item));
    }
    return node;
  }

  std::unique_ptr<RemoveItem> BuildRemoveItem(
      CypherParser::OC_RemoveItemContext *ctx) {
    auto node = std::make_unique<RemoveItem>();
    if (ctx->oC_PropertyExpression() != nullptr) {
      node->type = RemoveItem::Type::kProperty;
      node->target = BuildPropertyExpression(ctx->oC_PropertyExpression());
      return node;
    }
    node->type = RemoveItem::Type::kLabels;
    node->target = BuildVariableExpression(ctx->oC_Variable());
    node->labels = BuildNodeLabels(ctx->oC_NodeLabels());
    return node;
  }

  std::unique_ptr<With> BuildWith(CypherParser::OC_WithContext *ctx) {
    auto node = std::make_unique<With>();
    node->body = BuildProjectionBody(ctx->oC_ProjectionBody());
    if (ctx->oC_Where() != nullptr) {
      node->where = BuildExpression(ctx->oC_Where()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Return> BuildReturn(CypherParser::OC_ReturnContext *ctx) {
    auto node = std::make_unique<Return>();
    node->body = BuildProjectionBody(ctx->oC_ProjectionBody());
    return node;
  }

  std::unique_ptr<ProjectionBody> BuildProjectionBody(
      CypherParser::OC_ProjectionBodyContext *ctx) {
    auto node = std::make_unique<ProjectionBody>();
    node->distinct = ctx->DISTINCT() != nullptr;
    auto *items_ctx = ctx->oC_ProjectionItems();
    if (items_ctx != nullptr) {
      node->star = HasStar(items_ctx);
      for (auto *item : items_ctx->oC_ProjectionItem()) {
        node->items.push_back(BuildProjectionItem(item));
      }
    }
    if (ctx->oC_Order() != nullptr) {
      for (auto *item : ctx->oC_Order()->oC_SortItem()) {
        node->order_by.push_back(BuildSortItem(item));
      }
    }
    if (ctx->oC_Skip() != nullptr) {
      node->skip = BuildExpression(ctx->oC_Skip()->oC_Expression());
    }
    if (ctx->oC_Limit() != nullptr) {
      node->limit = BuildExpression(ctx->oC_Limit()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<ProjectionItem> BuildProjectionItem(
      CypherParser::OC_ProjectionItemContext *ctx) {
    auto node = std::make_unique<ProjectionItem>();
    node->expression = BuildExpression(ctx->oC_Expression());
    if (ctx->oC_Variable() != nullptr) {
      node->alias = ParseVariable(ctx->oC_Variable());
    }
    return node;
  }

  std::unique_ptr<SortItem> BuildSortItem(
      CypherParser::OC_SortItemContext *ctx) {
    auto node = std::make_unique<SortItem>();
    node->expression = BuildExpression(ctx->oC_Expression());
    if ((ctx->ASC() != nullptr) || (ctx->ASCENDING() != nullptr)) {
      node->ascending = true;
    }
    if ((ctx->DESC() != nullptr) || (ctx->DESCENDING() != nullptr)) {
      node->ascending = false;
    }
    return node;
  }

  std::unique_ptr<Pattern> BuildPattern(CypherParser::OC_PatternContext *ctx) {
    auto node = std::make_unique<Pattern>();
    for (auto *part : ctx->oC_PatternPart()) {
      node->parts.push_back(BuildPatternPart(part));
    }
    return node;
  }

  std::unique_ptr<PatternPart> BuildPatternPart(
      CypherParser::OC_PatternPartContext *ctx) {
    auto node = std::make_unique<PatternPart>();
    if (ctx->oC_Variable() != nullptr) {
      node->variable = ParseVariable(ctx->oC_Variable());
    }
    node->element = BuildPatternElement(
        ctx->oC_AnonymousPatternPart()->oC_PatternElement());
    return node;
  }

  std::unique_ptr<PatternElement> BuildPatternElement(
      CypherParser::OC_PatternElementContext *ctx) {
    if (ctx->oC_PatternElement() != nullptr) {
      return BuildPatternElement(ctx->oC_PatternElement());
    }
    auto node = std::make_unique<PatternElement>();
    node->node_pattern = BuildNodePattern(ctx->oC_NodePattern());
    for (auto *chain_ctx : ctx->oC_PatternElementChain()) {
      auto rel = BuildRelationshipPattern(chain_ctx->oC_RelationshipPattern());
      auto np = BuildNodePattern(chain_ctx->oC_NodePattern());
      node->chain.emplace_back(std::move(rel), std::move(np));
    }
    return node;
  }

  std::unique_ptr<RelationshipsPattern> BuildRelationshipsPattern(
      CypherParser::OC_RelationshipsPatternContext *ctx) {
    auto node = std::make_unique<RelationshipsPattern>();
    node->node_pattern = BuildNodePattern(ctx->oC_NodePattern());
    for (auto *chain_ctx : ctx->oC_PatternElementChain()) {
      auto rel = BuildRelationshipPattern(chain_ctx->oC_RelationshipPattern());
      auto np = BuildNodePattern(chain_ctx->oC_NodePattern());
      node->chain.emplace_back(std::move(rel), std::move(np));
    }
    return node;
  }

  std::unique_ptr<NodePattern> BuildNodePattern(
      CypherParser::OC_NodePatternContext *ctx) {
    auto node = std::make_unique<NodePattern>();
    if (ctx->oC_Variable() != nullptr) {
      node->variable = ParseVariable(ctx->oC_Variable());
    }
    if (ctx->oC_NodeLabels() != nullptr) {
      node->labels = BuildNodeLabels(ctx->oC_NodeLabels());
    }
    if (ctx->oC_Properties() != nullptr) {
      node->properties = BuildProperties(ctx->oC_Properties());
    }
    return node;
  }

  std::unique_ptr<RelationshipPattern> BuildRelationshipPattern(
      CypherParser::OC_RelationshipPatternContext *ctx) {
    auto node = std::make_unique<RelationshipPattern>();
    node->left_arrow = ctx->oC_LeftArrowHead() != nullptr;
    node->right_arrow = ctx->oC_RightArrowHead() != nullptr;
    if (ctx->oC_RelationshipDetail() != nullptr) {
      node->detail = BuildRelationshipDetail(ctx->oC_RelationshipDetail());
    }
    return node;
  }

  std::unique_ptr<RelationshipDetail> BuildRelationshipDetail(
      CypherParser::OC_RelationshipDetailContext *ctx) {
    auto node = std::make_unique<RelationshipDetail>();
    if (ctx->oC_Variable() != nullptr) {
      node->variable = ParseVariable(ctx->oC_Variable());
    }
    if (ctx->oC_RelationshipTypes() != nullptr) {
      for (auto *rt : ctx->oC_RelationshipTypes()->oC_RelTypeName()) {
        node->types.push_back(ParseSchemaName(rt->oC_SchemaName()));
      }
    }
    if (ctx->oC_RangeLiteral() != nullptr) {
      node->range = BuildRangeLiteral(ctx->oC_RangeLiteral());
    }
    if (ctx->oC_Properties() != nullptr) {
      node->properties = BuildProperties(ctx->oC_Properties());
    }
    return node;
  }

  static RelationshipDetail::RangeLiteral BuildRangeLiteral(
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
      range.min = ParseIntegerLiteral(ctx->oC_IntegerLiteral(0)->getText());
      return range;
    }
    for (auto *lit : ctx->oC_IntegerLiteral()) {
      int tok_index = static_cast<int>(lit->getStart()->getTokenIndex());
      if (tok_index < dot_index) {
        range.min = ParseIntegerLiteral(lit->getText());
      } else {
        range.max = ParseIntegerLiteral(lit->getText());
      }
    }
    return range;
  }

  std::unique_ptr<Properties> BuildProperties(
      CypherParser::OC_PropertiesContext *ctx) {
    auto node = std::make_unique<Properties>();
    if (ctx->oC_MapLiteral() != nullptr) {
      node->map = BuildMapLiteral(ctx->oC_MapLiteral());
    } else if (ctx->oC_Parameter() != nullptr) {
      node->parameter = BuildParameter(ctx->oC_Parameter());
    }
    return node;
  }

  std::unique_ptr<Expression> BuildExpression(
      CypherParser::OC_ExpressionContext *ctx) {
    return BuildOrExpression(ctx->oC_OrExpression());
  }

  std::unique_ptr<Expression> BuildOrExpression(
      CypherParser::OC_OrExpressionContext *ctx) {
    auto parts = ctx->oC_XorExpression();
    auto expr = BuildXorExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<OrExpression>();
      node->left = std::move(expr);
      node->right = BuildXorExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> BuildXorExpression(
      CypherParser::OC_XorExpressionContext *ctx) {
    auto parts = ctx->oC_AndExpression();
    auto expr = BuildAndExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<XorExpression>();
      node->left = std::move(expr);
      node->right = BuildAndExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> BuildAndExpression(
      CypherParser::OC_AndExpressionContext *ctx) {
    auto parts = ctx->oC_NotExpression();
    auto expr = BuildNotExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<AndExpression>();
      node->left = std::move(expr);
      node->right = BuildNotExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> BuildNotExpression(
      CypherParser::OC_NotExpressionContext *ctx) {
    auto expr = BuildComparisonExpression(ctx->oC_ComparisonExpression());
    for (size_t i = 0; i < ctx->NOT().size(); ++i) {
      auto node = std::make_unique<NotExpression>();
      node->operand = std::move(expr);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> BuildComparisonExpression(
      CypherParser::OC_ComparisonExpressionContext *ctx) {
    auto left = BuildStringListNullPredicateExpression(
        ctx->oC_StringListNullPredicateExpression());
    auto parts = ctx->oC_PartialComparisonExpression();
    if (parts.empty()) {
      return left;
    }
    if (parts.size() == 1) {
      auto node = std::make_unique<ComparisonExpression>();
      node->left = std::move(left);
      node->op = ExtractComparisonOp(parts[0]);
      node->right = BuildStringListNullPredicateExpression(
          parts[0]->oC_StringListNullPredicateExpression());
      return node;
    }
    auto node = std::make_unique<ComparisonChainExpression>();
    node->left = std::move(left);
    for (auto *part : parts) {
      node->rights.emplace_back(
          ExtractComparisonOp(part),
          BuildStringListNullPredicateExpression(
              part->oC_StringListNullPredicateExpression()));
    }
    return node;
  }

  std::unique_ptr<Expression> BuildStringListNullPredicateExpression(
      CypherParser::OC_StringListNullPredicateExpressionContext *ctx) {
    auto expr = BuildAddOrSubtractExpression(ctx->oC_AddOrSubtractExpression());
    for (auto *child : ctx->children) {
      if (auto *string_pred =
              dynamic_cast<CypherParser::OC_StringPredicateExpressionContext *>(
                  child)) {
        auto node = std::make_unique<StringPredicateExpression>();
        node->left = std::move(expr);
        node->op = ExtractStringPredicateOp(string_pred);
        node->right = BuildAddOrSubtractExpression(
            string_pred->oC_AddOrSubtractExpression());
        expr = std::move(node);
        continue;
      }
      if (auto *list_pred =
              dynamic_cast<CypherParser::OC_ListPredicateExpressionContext *>(
                  child)) {
        auto node = std::make_unique<ListPredicateExpression>();
        node->element = std::move(expr);
        node->list = BuildAddOrSubtractExpression(
            list_pred->oC_AddOrSubtractExpression());
        expr = std::move(node);
        continue;
      }
      if (auto *null_pred =
              dynamic_cast<CypherParser::OC_NullPredicateExpressionContext *>(
                  child)) {
        auto node = std::make_unique<NullPredicateExpression>();
        node->operand = std::move(expr);
        node->is_null = !HasOperator(null_pred, "NOT");
        expr = std::move(node);
        continue;
      }
    }
    return expr;
  }

  std::unique_ptr<Expression> BuildAddOrSubtractExpression(
      CypherParser::OC_AddOrSubtractExpressionContext *ctx) {
    auto parts = ctx->oC_MultiplyDivideModuloExpression();
    auto expr = BuildMultiplyDivideModuloExpression(parts[0]);
    size_t expr_index = 1;
    for (auto *child : ctx->children) {
      if (child->getText() == "+" || child->getText() == "-") {
        auto rhs = BuildMultiplyDivideModuloExpression(parts[expr_index++]);
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

  std::unique_ptr<Expression> BuildMultiplyDivideModuloExpression(
      CypherParser::OC_MultiplyDivideModuloExpressionContext *ctx) {
    auto parts = ctx->oC_PowerOfExpression();
    auto expr = BuildPowerOfExpression(parts[0]);
    size_t expr_index = 1;
    for (auto *child : ctx->children) {
      const std::string text = child->getText();
      if (text == "*" || text == "/" || text == "%") {
        auto rhs = BuildPowerOfExpression(parts[expr_index++]);
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

  std::unique_ptr<Expression> BuildPowerOfExpression(
      CypherParser::OC_PowerOfExpressionContext *ctx) {
    auto parts = ctx->oC_UnaryAddOrSubtractExpression();
    auto expr = BuildUnaryAddOrSubtractExpression(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
      auto node = std::make_unique<PowerExpression>();
      node->left = std::move(expr);
      node->right = BuildUnaryAddOrSubtractExpression(parts[i]);
      expr = std::move(node);
    }
    return expr;
  }

  std::unique_ptr<Expression> BuildUnaryAddOrSubtractExpression(
      CypherParser::OC_UnaryAddOrSubtractExpressionContext *ctx) {
    if (!ctx->children.empty()) {
      const std::string op = ctx->children.front()->getText();
      if (op == "-") {
        auto *non_arithmetic = ctx->oC_NonArithmeticOperatorExpression();
        auto *atom = non_arithmetic->oC_Atom();
        auto *literal = atom != nullptr ? atom->oC_Literal() : nullptr;
        auto *number =
            literal != nullptr ? literal->oC_NumberLiteral() : nullptr;
        auto *integer =
            number != nullptr ? number->oC_IntegerLiteral() : nullptr;
        if (integer != nullptr &&
            ParseIntegerMagnitude(integer->getText()) == uint64_t{1} << 63) {
          auto minimum = std::make_unique<IntegerLiteral>();
          minimum->value = std::numeric_limits<int64_t>::min();
          return minimum;
        }
      }

      auto base = BuildNonArithmeticOperatorExpression(
          ctx->oC_NonArithmeticOperatorExpression());
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
      return base;
    }
    return BuildNonArithmeticOperatorExpression(
        ctx->oC_NonArithmeticOperatorExpression());
  }

  std::unique_ptr<Expression> BuildNonArithmeticOperatorExpression(
      CypherParser::OC_NonArithmeticOperatorExpressionContext *ctx) {
    auto expr = BuildAtom(ctx->oC_Atom());
    for (auto *child : ctx->children) {
      if (auto *list_op =
              dynamic_cast<CypherParser::OC_ListOperatorExpressionContext *>(
                  child)) {
        expr = ApplyListOperator(std::move(expr), list_op);
      } else if (auto *prop =
                     dynamic_cast<CypherParser::OC_PropertyLookupContext *>(
                         child)) {
        expr = ApplyPropertyLookup(std::move(expr), prop);
      }
    }
    if (ctx->oC_NodeLabels() != nullptr) {
      auto label_node = std::make_unique<LabelPredicateExpression>();
      label_node->expr = std::move(expr);
      label_node->labels = BuildNodeLabels(ctx->oC_NodeLabels());
      expr = std::move(label_node);
    }
    return expr;
  }

  std::unique_ptr<Expression> BuildAtom(CypherParser::OC_AtomContext *ctx) {
    if (ctx->oC_Literal() != nullptr) {
      return BuildLiteral(ctx->oC_Literal());
    }
    if (ctx->oC_Parameter() != nullptr) {
      return BuildParameter(ctx->oC_Parameter());
    }
    if (ctx->oC_CaseExpression() != nullptr) {
      return BuildCaseExpression(ctx->oC_CaseExpression());
    }
    if (ctx->COUNT() != nullptr) {
      return std::make_unique<CountStarExpression>();
    }
    if (ctx->oC_ListComprehension() != nullptr) {
      return BuildListComprehension(ctx->oC_ListComprehension());
    }
    if (ctx->oC_PatternComprehension() != nullptr) {
      return BuildPatternComprehension(ctx->oC_PatternComprehension());
    }
    if (ctx->oC_Quantifier() != nullptr) {
      return BuildQuantifier(ctx->oC_Quantifier());
    }
    if (ctx->oC_PatternPredicate() != nullptr) {
      auto node = std::make_unique<PatternPredicateExpression>();
      node->relationships_pattern = BuildRelationshipsPattern(
          ctx->oC_PatternPredicate()->oC_RelationshipsPattern());
      return node;
    }
    if (ctx->oC_ParenthesizedExpression() != nullptr) {
      auto node = std::make_unique<ParenthesizedExpression>();
      node->expr =
          BuildExpression(ctx->oC_ParenthesizedExpression()->oC_Expression());
      return node;
    }
    if (ctx->oC_FunctionInvocation() != nullptr) {
      return BuildFunctionInvocation(ctx->oC_FunctionInvocation());
    }
    if (ctx->oC_ExistentialSubquery() != nullptr) {
      return BuildExistentialSubquery(ctx->oC_ExistentialSubquery());
    }
    if (ctx->oC_Variable() != nullptr) {
      return BuildVariableExpression(ctx->oC_Variable());
    }
    return nullptr;
  }

  std::unique_ptr<Expression> BuildLiteral(
      CypherParser::OC_LiteralContext *ctx) {
    if (ctx->oC_BooleanLiteral() != nullptr) {
      auto node = std::make_unique<BooleanLiteral>();
      node->value = ctx->oC_BooleanLiteral()->TRUE() != nullptr;
      return node;
    }
    if (ctx->NULL_() != nullptr) {
      return std::make_unique<NullLiteral>();
    }
    if (ctx->oC_NumberLiteral() != nullptr) {
      if (ctx->oC_NumberLiteral()->oC_DoubleLiteral() != nullptr) {
        auto node = std::make_unique<DoubleLiteral>();
        node->value =
            std::stod(ctx->oC_NumberLiteral()->oC_DoubleLiteral()->getText());
        return node;
      }
      if (ctx->oC_NumberLiteral()->oC_IntegerLiteral() != nullptr) {
        auto node = std::make_unique<IntegerLiteral>();
        node->value = ParseIntegerLiteral(
            ctx->oC_NumberLiteral()->oC_IntegerLiteral()->getText());
        return node;
      }
    }
    if (ctx->StringLiteral() != nullptr) {
      auto node = std::make_unique<StringLiteral>();
      node->value = ParseStringLiteral(ctx->StringLiteral()->getText());
      return node;
    }
    if (ctx->oC_ListLiteral() != nullptr) {
      auto node = std::make_unique<ListLiteral>();
      auto exprs = ctx->oC_ListLiteral()->oC_Expression();
      if (exprs.size() == 1) {
        auto element = BuildExpression(exprs[0]);
        if (element != nullptr &&
            element->Is(ASTNodeType::kListPredicateExpression)) {
          auto *predicate = CastAst<ListPredicateExpression>(element.get());
          if (predicate->element != nullptr &&
              predicate->element->Is(ASTNodeType::kVariable)) {
            auto *variable = CastAst<Variable>(predicate->element.get());
            auto comprehension = std::make_unique<ListComprehension>();
            comprehension->variable = variable->name;
            comprehension->list_expr = std::move(predicate->list);
            return comprehension;
          }
        }
        node->elements.push_back(std::move(element));
        return node;
      }
      for (auto *expr : exprs) {
        node->elements.push_back(BuildExpression(expr));
      }
      return node;
    }
    if (ctx->oC_MapLiteral() != nullptr) {
      return BuildMapLiteral(ctx->oC_MapLiteral());
    }
    return nullptr;
  }

  std::unique_ptr<MapLiteral> BuildMapLiteral(
      CypherParser::OC_MapLiteralContext *ctx) {
    auto node = std::make_unique<MapLiteral>();
    auto keys = ctx->oC_PropertyKeyName();
    auto values = ctx->oC_Expression();
    const size_t count = std::min(keys.size(), values.size());
    for (size_t i = 0; i < count; ++i) {
      node->entries.emplace_back(ParsePropertyKeyName(keys[i]),
                                 BuildExpression(values[i]));
    }
    return node;
  }

  std::unique_ptr<Parameter> BuildParameter(
      CypherParser::OC_ParameterContext *ctx) {
    auto node = std::make_unique<Parameter>();
    if (ctx->oC_SymbolicName() != nullptr) {
      node->name = ParseSymbolicName(ctx->oC_SymbolicName());
    } else if (ctx->DecimalInteger() != nullptr) {
      node->name = ctx->DecimalInteger()->getText();
    }
    return node;
  }

  std::unique_ptr<Expression> BuildPropertyExpression(
      CypherParser::OC_PropertyExpressionContext *ctx) {
    auto expr = BuildAtom(ctx->oC_Atom());
    for (auto *lookup : ctx->oC_PropertyLookup()) {
      expr = ApplyPropertyLookup(std::move(expr), lookup);
    }
    return expr;
  }

  std::unique_ptr<Variable> BuildVariableExpression(
      CypherParser::OC_VariableContext *ctx) {
    auto node = std::make_unique<Variable>();
    node->name = ParseVariable(ctx);
    return node;
  }

  std::unique_ptr<Expression> BuildCaseExpression(
      CypherParser::OC_CaseExpressionContext *ctx) {
    auto node = std::make_unique<CaseExpression>();
    for (auto *alt : ctx->oC_CaseAlternative()) {
      auto when_expr = BuildExpression(alt->oC_Expression(0));
      auto then_expr = BuildExpression(alt->oC_Expression(1));
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
      if (ctx->ELSE() != nullptr) {
        node->else_expr = BuildExpression(parent_exprs[0]);
      } else {
        node->test = BuildExpression(parent_exprs[0]);
      }
    } else if (parent_exprs.size() >= 2) {
      auto *first = parent_exprs[0];
      auto *second = parent_exprs[1];
      if (first->getStart()->getTokenIndex() <
          second->getStart()->getTokenIndex()) {
        node->test = BuildExpression(first);
        node->else_expr = BuildExpression(second);
      } else {
        node->test = BuildExpression(second);
        node->else_expr = BuildExpression(first);
      }
    }
    return node;
  }

  std::unique_ptr<Expression> BuildListComprehension(
      CypherParser::OC_ListComprehensionContext *ctx) {
    auto node = std::make_unique<ListComprehension>();
    auto *filter = ctx->oC_FilterExpression();
    auto *id_in_coll = filter->oC_IdInColl();
    node->variable = ParseVariable(id_in_coll->oC_Variable());
    node->list_expr = BuildExpression(id_in_coll->oC_Expression());
    if (filter->oC_Where() != nullptr) {
      node->where_expr = BuildExpression(filter->oC_Where()->oC_Expression());
    }
    if (ctx->oC_Expression() != nullptr) {
      node->eval_expr = BuildExpression(ctx->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Expression> BuildPatternComprehension(
      CypherParser::OC_PatternComprehensionContext *ctx) {
    auto node = std::make_unique<PatternComprehension>();
    if (ctx->oC_Variable() != nullptr) {
      node->variable = ParseVariable(ctx->oC_Variable());
    }
    node->relationships_pattern =
        BuildRelationshipsPattern(ctx->oC_RelationshipsPattern());
    if (ctx->oC_Where() != nullptr) {
      node->where_expr = BuildExpression(ctx->oC_Where()->oC_Expression());
    }
    node->eval_expr = BuildExpression(ctx->oC_Expression());
    return node;
  }

  std::unique_ptr<Expression> BuildQuantifier(
      CypherParser::OC_QuantifierContext *ctx) {
    auto *filter = ctx->oC_FilterExpression();
    auto *id_in_coll = filter->oC_IdInColl();
    std::unique_ptr<Quantifier> node;
    if (ctx->ALL() != nullptr) {
      node = std::make_unique<AllQuantifier>();
    } else if (ctx->ANY() != nullptr) {
      node = std::make_unique<AnyQuantifier>();
    } else if (ctx->NONE() != nullptr) {
      node = std::make_unique<NoneQuantifier>();
    } else {
      node = std::make_unique<SingleQuantifier>();
    }
    node->variable = ParseVariable(id_in_coll->oC_Variable());
    node->list_expr = BuildExpression(id_in_coll->oC_Expression());
    if (filter->oC_Where() != nullptr) {
      node->predicate = BuildExpression(filter->oC_Where()->oC_Expression());
    }
    return node;
  }

  std::unique_ptr<Expression> BuildFunctionInvocation(
      CypherParser::OC_FunctionInvocationContext *ctx) {
    auto node = std::make_unique<FunctionInvocation>();
    node->function_name =
        ParseQualifiedName(ctx->oC_FunctionName()->oC_Namespace(),
                           ctx->oC_FunctionName()->oC_SymbolicName());
    node->distinct = ctx->DISTINCT() != nullptr;
    for (auto *expr : ctx->oC_Expression()) {
      node->arguments.push_back(BuildExpression(expr));
    }
    return node;
  }

  std::unique_ptr<Expression> BuildExistentialSubquery(
      CypherParser::OC_ExistentialSubqueryContext *ctx) {
    auto node = std::make_unique<ExistentialSubquery>();
    if (ctx->oC_RegularQuery() != nullptr) {
      node->query = BuildRegularQuery(ctx->oC_RegularQuery());
    } else if (ctx->oC_Pattern() != nullptr) {
      node->pattern = BuildPattern(ctx->oC_Pattern());
      if (ctx->oC_Where() != nullptr) {
        node->where_expr = BuildExpression(ctx->oC_Where()->oC_Expression());
      }
    }
    return node;
  }

  std::unique_ptr<Expression> ApplyPropertyLookup(
      std::unique_ptr<Expression> base,
      CypherParser::OC_PropertyLookupContext *ctx) {
    auto node = std::make_unique<PropertyExpression>();
    node->object = std::move(base);
    node->property_key = ParsePropertyKeyName(ctx->oC_PropertyKeyName());
    return node;
  }

  std::unique_ptr<Expression> ApplyListOperator(
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
      node->index = BuildExpression(ctx->oC_Expression(0));
      return node;
    }
    auto node = std::make_unique<ListSliceExpression>();
    node->list = std::move(base);
    auto exprs = ctx->oC_Expression();
    if (exprs.size() == 1) {
      auto *expr_ctx = exprs[0];
      int tok_index = static_cast<int>(expr_ctx->getStart()->getTokenIndex());
      if (dot_index >= 0 && tok_index < dot_index) {
        node->start_index = BuildExpression(expr_ctx);
      } else {
        node->end_index = BuildExpression(expr_ctx);
      }
    } else if (exprs.size() >= 2) {
      node->start_index = BuildExpression(exprs[0]);
      node->end_index = BuildExpression(exprs[1]);
    }
    return node;
  }

  std::vector<std::string> BuildNodeLabels(
      CypherParser::OC_NodeLabelsContext *ctx) {
    std::vector<std::string> labels;
    for (auto *label : ctx->oC_NodeLabel()) {
      labels.push_back(ParseSchemaName(label->oC_LabelName()->oC_SchemaName()));
    }
    return labels;
  }

  static std::string ParseSymbolicName(
      CypherParser::OC_SymbolicNameContext *ctx) {
    if (ctx == nullptr) {
      return {};
    }
    return UnescapeSymbolicName(ctx->getText());
  }

  std::string ParseSchemaName(CypherParser::OC_SchemaNameContext *ctx) {
    if (ctx->oC_SymbolicName() != nullptr) {
      return ParseSymbolicName(ctx->oC_SymbolicName());
    }
    if (ctx->oC_ReservedWord() != nullptr) {
      return ctx->oC_ReservedWord()->getText();
    }
    return {};
  }

  std::string ParsePropertyKeyName(
      CypherParser::OC_PropertyKeyNameContext *ctx) {
    return ParseSchemaName(ctx->oC_SchemaName());
  }

  std::string ParseVariable(CypherParser::OC_VariableContext *ctx) {
    return ParseSymbolicName(ctx->oC_SymbolicName());
  }

  std::string ParseProcedureName(CypherParser::OC_ProcedureNameContext *ctx) {
    return ParseQualifiedName(ctx->oC_Namespace(), ctx->oC_SymbolicName());
  }

  std::string ParseQualifiedName(CypherParser::OC_NamespaceContext *ns,
                                 CypherParser::OC_SymbolicNameContext *name) {
    std::string out;
    if (ns != nullptr) {
      for (auto *sym : ns->oC_SymbolicName()) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += ParseSymbolicName(sym);
      }
    }
    if (!out.empty()) {
      out.push_back('.');
    }
    out += ParseSymbolicName(name);
    return out;
  }

  static std::string ExtractComparisonOp(
      CypherParser::OC_PartialComparisonExpressionContext *ctx) {
    if ((ctx == nullptr) || ctx->children.empty()) {
      return {};
    }
    return ctx->children.front()->getText();
  }

  static std::string ExtractStringPredicateOp(
      CypherParser::OC_StringPredicateExpressionContext *ctx) {
    for (auto *child : ctx->children) {
      const std::string text = ToUpperAscii(child->getText());
      if (text == "STARTS" || text == "ENDS") {
        return text + " WITH";
      }
      if (text == "CONTAINS") {
        return text;
      }
    }
    return "CONTAINS";
  }

  void ParseYieldItems(CypherParser::OC_YieldItemsContext *ctx,
                       std::vector<StandaloneCall::YieldItem> &items,
                       std::unique_ptr<Expression> &where_expr) {
    for (auto *item_ctx : ctx->oC_YieldItem()) {
      StandaloneCall::YieldItem item;
      if (item_ctx->oC_ProcedureResultField() != nullptr) {
        item.result_field = ParseSymbolicName(
            item_ctx->oC_ProcedureResultField()->oC_SymbolicName());
      }
      item.variable = ParseVariable(item_ctx->oC_Variable());
      items.push_back(std::move(item));
    }
    if (ctx->oC_Where() != nullptr) {
      where_expr = BuildExpression(ctx->oC_Where()->oC_Expression());
    }
  }

  static std::string ToUpperAscii(const std::string &text) {
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

  static bool HasOperator(antlr4::ParserRuleContext *ctx,
                          const std::string &op) {
    const std::string op_upper = ToUpperAscii(op);
    for (auto *child : ctx->children) {
      if (ToUpperAscii(child->getText()) == op_upper) {
        return true;
      }
    }
    return false;
  }

  static bool HasStar(CypherParser::OC_ProjectionItemsContext *ctx) {
    if (ctx == nullptr) {
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

std::unique_ptr<Statement> ParseCypher(const std::string &input) {
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
  if ((tree == nullptr) || (tree->oC_Statement() == nullptr)) {
    std::vector<std::string> parse_errors;
    parse_errors.emplace_back("failed to parse statement");
    THROW(ParseError, std::move(parse_errors));
  }
  ASTBuilder builder;
  auto statement = builder.BuildStatement(tree->oC_Statement());
  if (!statement) {
    THROW(InternalError, "failed to build AST");
  }
  ValidateStatement(*statement);
  return statement;
}

std::unique_ptr<Statement> ParseCypherAndRewrite(const std::string &input) {
  auto statement = ParseCypher(input);
  ApplyDefaultRewriters(*statement);
  ValidateStatement(*statement);
  return statement;
}

}  // namespace ast
