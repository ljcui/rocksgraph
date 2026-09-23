
// Generated from Cypher.g4 by ANTLR 4.13.2

#pragma once


#include "antlr4-runtime.h"




class  CypherLexer : public antlr4::Lexer {
public:
  enum {
    T__0 = 1, T__1 = 2, T__2 = 3, T__3 = 4, T__4 = 5, T__5 = 6, T__6 = 7, 
    T__7 = 8, T__8 = 9, T__9 = 10, T__10 = 11, T__11 = 12, T__12 = 13, T__13 = 14, 
    T__14 = 15, T__15 = 16, T__16 = 17, T__17 = 18, T__18 = 19, T__19 = 20, 
    T__20 = 21, T__21 = 22, T__22 = 23, T__23 = 24, T__24 = 25, T__25 = 26, 
    T__26 = 27, T__27 = 28, T__28 = 29, T__29 = 30, T__30 = 31, T__31 = 32, 
    T__32 = 33, T__33 = 34, T__34 = 35, T__35 = 36, T__36 = 37, T__37 = 38, 
    T__38 = 39, T__39 = 40, T__40 = 41, T__41 = 42, T__42 = 43, T__43 = 44, 
    T__44 = 45, UNION = 46, ALL = 47, OPTIONAL = 48, MATCH = 49, UNWIND = 50, 
    AS = 51, MERGE = 52, ON = 53, CREATE = 54, SET = 55, DETACH = 56, DELETE = 57, 
    REMOVE = 58, CALL = 59, YIELD = 60, WITH = 61, RETURN = 62, DISTINCT = 63, 
    ORDER = 64, BY = 65, L_SKIP = 66, LIMIT = 67, ASCENDING = 68, ASC = 69, 
    DESCENDING = 70, DESC = 71, WHERE = 72, OR = 73, XOR = 74, AND = 75, 
    NOT = 76, STARTS = 77, ENDS = 78, CONTAINS = 79, IN = 80, IS = 81, NULL_ = 82, 
    COUNT = 83, CASE = 84, ELSE = 85, END = 86, WHEN = 87, THEN = 88, ANY = 89, 
    NONE = 90, SINGLE = 91, EXISTS = 92, EXPLAIN = 93, TRUE = 94, FALSE = 95, 
    HexInteger = 96, DecimalInteger = 97, OctalInteger = 98, HexLetter = 99, 
    HexDigit = 100, Digit = 101, NonZeroDigit = 102, NonZeroOctDigit = 103, 
    OctDigit = 104, ZeroDigit = 105, ExponentDecimalReal = 106, RegularDecimalReal = 107, 
    StringLiteral = 108, EscapedChar = 109, CONSTRAINT = 110, DO = 111, 
    FOR = 112, REQUIRE = 113, UNIQUE = 114, MANDATORY = 115, SCALAR = 116, 
    OF = 117, ADD = 118, DROP = 119, REDUCE = 120, SHORTESTPATH = 121, ALLSHORTESTPATHS = 122, 
    FILTER = 123, EXTRACT = 124, UnescapedSymbolicName = 125, IdentifierStart = 126, 
    IdentifierPart = 127, EscapedSymbolicName = 128, SP = 129, WHITESPACE = 130, 
    Comment = 131
  };

  explicit CypherLexer(antlr4::CharStream *input);

  ~CypherLexer() override;


  std::string getGrammarFileName() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const std::vector<std::string>& getChannelNames() const override;

  const std::vector<std::string>& getModeNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;

  const antlr4::atn::ATN& getATN() const override;

  // By default the static state used to implement the lexer is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:

  // Individual action functions triggered by action() above.

  // Individual semantic predicate functions triggered by sempred() above.

};

