#include "../include/parser.h"
#include <deque>
#include <memory>

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> TheBuilder;

int CurTok;
std::map<char, int> BinopPrecedence;

extern int CurLine;
extern int CurCol;

// --- Token Management ---

std::deque<int> TokenBuffer;
std::deque<std::string> IdBuffer;

int AdvanceToken() {
  if (TokenBuffer.empty()) {
    CurTok = gettok();
  } else {
    CurTok = TokenBuffer.front();
    IdentifierStr = IdBuffer.front();
    TokenBuffer.pop_front();
    IdBuffer.pop_front();
  }
  return CurTok;
}

int getNextToken() { return AdvanceToken(); }

int PeekToken(size_t n = 0) {
  while (TokenBuffer.size() <= n) {
    TokenBuffer.push_back(gettok());
    IdBuffer.push_back(IdentifierStr);
  }
  return TokenBuffer[n];
}

// --- Error Handling ---

std::string getTokenName(int tok) {
  switch (tok) {
  case tok_eof:
    return "EOF";
  case tok_def:
    return "fun";
  case tok_extern:
    return "extern";
  case tok_identifier:
    return "identifier";
  case tok_number:
    return "number";
  case tok_if:
    return "if";
  case tok_then:
    return "then";
  case tok_else:
    return "else";
  case tok_for:
    return "for";
  case tok_in:
    return "in";
  case tok_int:
    return "int";
  case tok_double:
    return "double";
  default:
    if (isascii(tok))
      return std::string(1, (char)tok);
    return "unknown token (" + std::to_string(tok) + ")";
  }
}

std::unique_ptr<ExprAST> LogError(const std::string &Msg) {
  fprintf(stderr, "Error [Line %d, Col %d]: %s\n", CurLine, CurCol,
          Msg.c_str());
  return nullptr;
}

bool Expect(int ExpectedTok, const std::string &Context) {
  if (CurTok == ExpectedTok) {
    getNextToken();
    return true;
  }

  std::string ErrorMsg =
      "Expected '" + getTokenName(ExpectedTok) + "' " + Context;
  ErrorMsg += ". Found '" + getTokenName(CurTok) + "' instead";

  if (CurTok == tok_identifier)
    ErrorMsg += " (\"" + IdentifierStr + "\")";

  if (CurTok == tok_identifier && ExpectedTok == tok_then)
    ErrorMsg += " (Hint: check for missing 'then')";

  LogError(ErrorMsg);
  return false;
}

static int GetTokPrecedence() {
  if (!isascii(CurTok))
    return -1;
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
}

// --- Logic for Primary Expressions ---
static TypeKind ParseType() {
  if (CurTok == tok_int) {
    getNextToken();
    return TypeKind::Int;
  }
  if (CurTok == tok_double) {
    getNextToken();
    return TypeKind::Double;
  }
  return TypeKind::Double;
}

static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;
  getNextToken();

  if (CurTok == ':') {
    getNextToken();
    TypeKind Ty = ParseType();
    std::unique_ptr<ExprAST> Init = nullptr;
    if (CurTok == '=') {
      getNextToken();
      Init = ParseExpression();
    }
    return std::make_unique<VarExprAST>(IdName, Ty, std::move(Init));
  }

  if (CurTok == '(') {
    getNextToken();
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != ')') {
      while (true) {
        if (auto Arg = ParseExpression())
          Args.push_back(std::move(Arg));
        else
          return nullptr;

        if (CurTok == ')')
          break;
        if (!Expect(',', "between function arguments"))
          return nullptr;
      }
    }
    getNextToken();
    return std::make_unique<CallExprAST>(IdName, std::move(Args));
  }

  return std::make_unique<VariableExprAST>(IdName);
}

static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto Result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken();
  return Result;
}

std::unique_ptr<ExprAST> ParseVarExpr() {
  TypeKind Ty = ParseType();
  if (CurTok != tok_identifier)
    return LogError("Expected identifier");

  std::string Name = IdentifierStr;
  getNextToken();

  std::unique_ptr<ExprAST> Init = nullptr;
  if (CurTok == '=') {
    getNextToken();
    Init = ParseExpression();
  }
  return std::make_unique<VarExprAST>(Name, Ty, std::move(Init));
}

std::unique_ptr<GlobalVarAST> ParseGlobal() {
  TypeKind Ty;
  std::string Name;

  if (CurTok == tok_int || CurTok == tok_double) {
    Ty = (CurTok == tok_int) ? TypeKind::Int : TypeKind::Double;
    getNextToken();
    if (CurTok != tok_identifier) {
      LogError("Expected identifier after type");
      return nullptr;
    }
    Name = IdentifierStr;
    getNextToken();
  } else {
    Name = IdentifierStr;
    getNextToken();
    if (CurTok != ':') {
      LogError("Expected ':' after global identifier");
      return nullptr;
    }
    getNextToken();
    Ty = ParseType();
  }

  double Val = 0;
  if (CurTok == '=') {
    getNextToken();
    if (CurTok == tok_number) {
      Val = NumVal;
      getNextToken();
    } else {
      LogError("Global initializer must be a numeric literal");
    }
  }

  if (CurTok == ';')
    getNextToken();

  return std::make_unique<GlobalVarAST>(Name, Ty, Val);
}

static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken();
  auto V = ParseExpression();
  if (!V)
    return nullptr;

  if (!Expect(')', "to close expression"))
    return nullptr;
  return V;
}

static std::unique_ptr<ExprAST> ParseIfExpr() {
  getNextToken();

  auto Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  if (!Expect(tok_then, "after 'if' condition"))
    return nullptr;
  auto Then = ParseExpression();
  if (!Then)
    return nullptr;

  if (!Expect(tok_else, "to complete 'if'"))
    return nullptr;
  auto Else = ParseExpression();
  if (!Else)
    return nullptr;

  return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                     std::move(Else));
}

static std::unique_ptr<ExprAST> ParseForExpr() {
  getNextToken();

  if (CurTok != tok_identifier)
    return LogError("Expected identifier after 'for'");

  std::string IdName = IdentifierStr;
  getNextToken();

  if (!Expect('=', "after for-loop identifier"))
    return nullptr;

  auto Start = ParseExpression();
  if (!Start)
    return nullptr;

  if (!Expect(',', "after for-loop start value"))
    return nullptr;

  auto End = ParseExpression();
  if (!End)
    return nullptr;

  std::unique_ptr<ExprAST> Step;
  if (CurTok == ',') {
    getNextToken();
    Step = ParseExpression();
    if (!Step)
      return nullptr;
  }

  if (!Expect(tok_in, "to begin for-loop body"))
    return nullptr;

  auto Body = ParseExpression();
  if (!Body)
    return nullptr;

  return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
                                      std::move(Step), std::move(Body));
}

static std::unique_ptr<ExprAST> ParseBlockExpr() {
  getNextToken();
  std::vector<std::unique_ptr<ExprAST>> Exprs;

  while (CurTok != '}' && CurTok != tok_eof) {
    auto E = ParseExpression();
    if (!E)
      return nullptr;
    Exprs.push_back(std::move(E));

    if (CurTok == ';')
      getNextToken();
  }

  if (!Expect('}', "at end of block"))
    return nullptr;
  return std::make_unique<BlockExprAST>(std::move(Exprs));
}

static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case tok_if:
    return ParseIfExpr();
  case tok_for:
    return ParseForExpr();
  case tok_int:
  case tok_double:
    return ParseVarExpr();
  case '{':
    return ParseBlockExpr();
  case '(':
    return ParseParenExpr();
  default:
    return LogError("Unknown token '" + getTokenName(CurTok) +
                    "' when expecting an expression");
  }
}

// --- Logic for Binary Expressions ---

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = GetTokPrecedence();
    if (TokPrec < ExprPrec)
      return LHS;

    int BinOp = CurTok;
    getNextToken();

    auto RHS = ParsePrimary();
    if (!RHS)
      return nullptr;

    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    LHS =
        std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS)
    return nullptr;
  return ParseBinOpRHS(0, std::move(LHS));
}

// --- Logic for Functions/Globals ---

static ArgInfo ParseArgument() {
  TypeKind SelectedType = TypeKind::Double;
  std::string SelectedName;

  if (CurTok == tok_int || CurTok == tok_double) {
    SelectedType = (CurTok == tok_int) ? TypeKind::Int : TypeKind::Double;
    getNextToken();
    if (CurTok != tok_identifier) {
      LogError("Expected identifier after type in argument list");
      return {};
    }
  } else if (CurTok != tok_identifier) {
    LogError("Expected argument name or type");
    return {};
  }

  SelectedName = IdentifierStr;
  getNextToken();

  if (CurTok == ':') {
    getNextToken();
    SelectedType = ParseType();
  }

  return {SelectedName, SelectedType};
}

static std::unique_ptr<PrototypeAST> ParsePrototype() {
  TypeKind RetType = TypeKind::Double;
  if (CurTok == tok_int || CurTok == tok_double) {
    RetType = (CurTok == tok_int) ? TypeKind::Int : TypeKind::Double;
    getNextToken();
  }

  if (CurTok != tok_identifier) {
    LogError("Expected function name in prototype");
    return nullptr;
  }

  std::string FnName = IdentifierStr;
  getNextToken();

  if (!Expect('(', "after function name"))
    return nullptr;

  std::vector<ArgInfo> Args;
  if (CurTok != ')') {
    while (true) {
      auto Arg = ParseArgument();
      if (Arg.Name.empty())
        return nullptr;
      Args.push_back(Arg);

      if (CurTok == ')')
        break;
      if (!Expect(',', "between arguments"))
        return nullptr;
    }
  }
  getNextToken();

  return std::make_unique<PrototypeAST>(FnName, std::move(Args), RetType);
}

std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken();
  auto Proto = ParsePrototype();
  if (!Proto)
    return nullptr;

  if (auto E = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  return nullptr;
}

std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    std::vector<ArgInfo> EmptyArgs;
    auto Proto = std::make_unique<PrototypeAST>(
        "__anon_expr", std::move(EmptyArgs), TypeKind::Double);
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}

std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken();
  return ParsePrototype();
}

void SetupPrecedence() {
  BinopPrecedence['='] = 5;
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40;
}
