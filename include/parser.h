#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"
#include <map>
#include <memory>

// Global state for the parser
extern int CurTok;
extern std::map<char, int> BinopPrecedence;

int getNextToken();
void SetupPrecedence();

int PeekToken(size_t n);

std::unique_ptr<ExprAST> ParseExpression();
std::unique_ptr<FunctionAST> ParseDefinition();
std::unique_ptr<PrototypeAST> ParseExtern();
std::unique_ptr<GlobalVarAST> ParseGlobal();
std::unique_ptr<FunctionAST> ParseTopLevelExpr();

#endif
