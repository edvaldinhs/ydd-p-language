#ifndef SEMA_H
#define SEMA_H

#include "ast.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

class SemanticAnalyzer {
  typedef std::map<std::string, MyType> Scope;
  std::vector<Scope> Scopes;

  std::map<std::string, StructInfo> StructTable;

  std::map<std::string, MyType> FunctionSignatures;

public:
  SemanticAnalyzer() { Scopes.push_back(Scope()); }

  void enterScope() { Scopes.push_back(Scope()); }
  void exitScope() { Scopes.pop_back(); }

  void Analyze(ExprAST *Node);

  void AnalyzeFunction(FunctionAST *Node);
  void AnalyzeBinary(BinaryExprAST *Node);
  void AnalyzeString(StringExprAST *Node);
  void AnalyzeVariable(VariableExprAST *Node);
  void AnalyzeCall(CallExprAST *Node);
  void AnalyzeIf(IfExprAST *Node);
  void AnalyzeFor(ForExprAST *Node);
  void AnalyzeBlock(BlockExprAST *Node);
  void AnalyzeVarExpr(VarExprAST *Node);
  void AnalyzeMemberAccess(MemberAccessExprAST *Node);
  void AnalyzeUnary(UnaryExprAST *Node);

  void DeclareVariable(std::string Name, MyType T);
  MyType LookupVariable(std::string Name);
  void RegisterStruct(StructDefinitionAST *S);
  void DeclareFunction(std::string Name, MyType Ret) {
    FunctionSignatures[Name] = Ret;
  }
};

#endif
