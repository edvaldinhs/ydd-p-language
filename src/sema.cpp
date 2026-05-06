#include "../include/sema.h"
#include <iostream>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <map>
#include <memory>
#include <string>

// #define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define DEBUG_MSG(msg) std::cerr << "[SEMA] " << msg << std::endl
#else
#define DEBUG_MSG(msg)
#endif

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::map<std::string, StructInfo> StructDefs;
extern std::map<std::string, llvm::StructType *> StructTypeMap;

extern int CurLine;
extern int CurCol;

llvm::Type *getLLVMType(MyType T);

void SemanticAnalyzer::Analyze(ExprAST *Node) {
  if (!Node)
    return;

  DEBUG_MSG("Analyzing Node at " << Node);

  if (auto *Bin = dynamic_cast<BinaryExprAST *>(Node)) {
    DEBUG_MSG("Type-checking Binary Op: " << Bin->getOp());
    AnalyzeBinary(Bin);
  } else if (auto *Var = dynamic_cast<VariableExprAST *>(Node)) {
    DEBUG_MSG("Type-checking Variable: " << Var->getName());
    AnalyzeVariable(Var);
  } else if (auto *Str = dynamic_cast<StringExprAST *>(Node)) {
    DEBUG_MSG("Type-checking String: " << Str);
    AnalyzeString(Str);
  } else if (auto *Call = dynamic_cast<CallExprAST *>(Node)) {
    DEBUG_MSG("Type-checking Call: " << Call);
    AnalyzeCall(Call);
  } else if (auto *If = dynamic_cast<IfExprAST *>(Node)) {
    DEBUG_MSG("Type-checking If: " << If);
    AnalyzeIf(If);
  } else if (auto *For = dynamic_cast<ForExprAST *>(Node)) {
    DEBUG_MSG("Type-checking For: " << For);
    AnalyzeFor(For);
  } else if (auto *Block = dynamic_cast<BlockExprAST *>(Node)) {
    DEBUG_MSG("Type-checking Block: " << Block);
    AnalyzeBlock(Block);
  } else if (auto *VExpr = dynamic_cast<VarExprAST *>(Node)) {
    DEBUG_MSG("Type-checking VarExpr: " << VExpr->getName());
    AnalyzeVarExpr(VExpr);
  } else if (auto *UExpr = dynamic_cast<UnaryExprAST *>(Node)) {
    DEBUG_MSG("Type-checking Unary op: " << UExpr->getOpcode());
    AnalyzeUnary(UExpr);
  } else if (auto *Mem = dynamic_cast<MemberAccessExprAST *>(Node)) {
    DEBUG_MSG("Type-checking MemberAcess: " << Mem->getMemberName());
    AnalyzeMemberAccess(Mem);
  }
}

void SemanticAnalyzer::AnalyzeString(StringExprAST *Node) {
  MyType T(TypeCategory::Char);
  T.PointerLevel = 1;
  Node->ResolvedType = T;
}

void SemanticAnalyzer::AnalyzeUnary(UnaryExprAST *Node) {
  Analyze(Node->getOperand());
  MyType SubType = Node->getOperand()->ResolvedType;

  if (Node->getOpcode() == '&') {
    Node->ResolvedType = SubType;
    Node->ResolvedType.PointerLevel++;
  } else if (Node->getOpcode() == '*') {
    if (SubType.PointerLevel <= 0) {
      std::cerr << "Error: Cannot dereference non-pointer\n";
      exit(1);
    }
    Node->ResolvedType = SubType;
    Node->ResolvedType.PointerLevel--;
  } else {
    Node->ResolvedType = SubType;
  }
}

void SemanticAnalyzer::AnalyzeFunction(FunctionAST *Node) {
  const PrototypeAST &Proto = Node->getProto();

  FunctionSignatures[Proto.getName()] = Proto.getRetType();

  enterScope();
  for (auto &Arg : Proto.getArguments()) {
    DeclareVariable(Arg.Name, Arg.Type);
  }

  if (Node->getBody()) {
    Analyze(const_cast<ExprAST *>(Node->getBody()));
  }
  exitScope();
}

void SemanticAnalyzer::AnalyzeCall(CallExprAST *Node) {
  if (FunctionSignatures.find(Node->getCallee()) == FunctionSignatures.end()) {
    if (Node->getCallee() == "print") {
      Node->ResolvedType = MyType(TypeCategory::Int);
    } else {
      std::cerr << "Semantic Error: Unknown function '" << Node->getCallee()
                << "'\n";
      exit(1);
    }
  } else {
    Node->ResolvedType = FunctionSignatures[Node->getCallee()];
  }

  for (auto &Arg : Node->getArgs()) {
    Analyze(Arg.get());
  }
}

void SemanticAnalyzer::AnalyzeIf(IfExprAST *Node) {
  Analyze(Node->getCond());
  Analyze(Node->getThen());
  if (Node->getElse())
    Analyze(Node->getElse());

  Node->ResolvedType = Node->getThen()->ResolvedType;
}

void SemanticAnalyzer::AnalyzeFor(ForExprAST *Node) {
  Analyze(Node->getStart());

  enterScope();

  DeclareVariable(Node->getVarName(), Node->getVarType());

  Analyze(Node->getBody());

  if (Node->getStep())
    Analyze(Node->getStep());

  Analyze(Node->getBody());

  exitScope();

  Node->ResolvedType = Node->getVarType();
}

void SemanticAnalyzer::AnalyzeBlock(BlockExprAST *Node) {
  for (auto &Expr : Node->getExpressions()) {
    Analyze(Expr.get());
  }
  if (!Node->getExpressions().empty()) {
    Node->ResolvedType = Node->getExpressions().back()->ResolvedType;
  }
}

void SemanticAnalyzer::AnalyzeBinary(BinaryExprAST *Node) {
  Analyze(Node->getLHS().get());

  if (Node->getOp() != '.') {
    Analyze(Node->getRHS().get());
  }

  MyType L = Node->getLHS()->ResolvedType;
  MyType R = Node->getRHS()->ResolvedType;

  if (Node->getOp() == '=') {
    Node->ResolvedType = L;
    return;
  }

  if (L.PointerLevel > 0 || R.PointerLevel > 0) {
    Node->ResolvedType = (L.PointerLevel > 0) ? L : R;
    return;
  }

  if (L.Category == TypeCategory::Double ||
      R.Category == TypeCategory::Double) {
    Node->ResolvedType = MyType(TypeCategory::Double);
  } else {
    Node->ResolvedType = MyType(TypeCategory::Int);
  }
}

void SemanticAnalyzer::AnalyzeVariable(VariableExprAST *Node) {
  Node->ResolvedType = LookupVariable(Node->getName());
}

void SemanticAnalyzer::AnalyzeVarExpr(VarExprAST *Node) {
  if (Node->getInit()) {
    Analyze(Node->getInit());
  }
  DeclareVariable(Node->getName(), Node->getType());
  Node->ResolvedType = Node->getType();
}

MyType SemanticAnalyzer::LookupVariable(std::string Name) {
  DEBUG_MSG("Looking up variable: '" << Name << "'");
  for (auto it = Scopes.rbegin(); it != Scopes.rend(); ++it) {
    if (it->count(Name))
      return (*it)[Name];
  }
  std::cerr << "Semantic Error [Line " << CurLine << ", Col " << CurCol
            << "]: Undefined variable '" << Name << "'\n";
  exit(1);
}

void SemanticAnalyzer::DeclareVariable(std::string Name, MyType T) {
  DEBUG_MSG("Declaring variable: '"
            << Name << "' with type category: " << (int)T.Category);
  Scopes.back()[Name] = T;
}

void SemanticAnalyzer::RegisterStruct(StructDefinitionAST *S) {
  DEBUG_MSG("Registering Struct: " << S->getName());
  StructInfo Info;
  Info.Name = S->getName();

  std::vector<llvm::Type *> MemberTypes;
  for (auto &Member : S->getMembers()) {
    Info.Members.push_back(Member);
    Info.MemberIndex[Member.first] = Info.Members.size() - 1;
    MemberTypes.push_back(getLLVMType(Member.second));
  }

  StructTable[Info.Name] = Info;
  StructDefs[Info.Name] = Info;

  llvm::StructType *STy = llvm::StructType::create(*TheContext, Info.Name);
  STy->setBody(MemberTypes);
  StructTypeMap[Info.Name] = STy;
  DEBUG_MSG("Struct " << S->getName() << " registered with "
                      << Info.Members.size() << " members.");
}

void SemanticAnalyzer::AnalyzeMemberAccess(MemberAccessExprAST *Node) {
  DEBUG_MSG("Analyzing Member Access...");
  Analyze(Node->getStructExpr());
  MyType ObjType = Node->getStructExpr()->ResolvedType;
  DEBUG_MSG("Accessing member of struct type: " << ObjType.Name);

  if (ObjType.Category != TypeCategory::Struct) {
    std::cerr << "Error: Expression is not a struct\n";
    exit(1);
  }

  auto &Info = StructTable[ObjType.Name];

  if (Info.MemberIndex.find(Node->getMemberName()) == Info.MemberIndex.end()) {
    std::cerr << "Error: Struct " << ObjType.Name << " has no member "
              << Node->getMemberName() << "\n";
    exit(1);
  }

  unsigned Index = Info.MemberIndex[Node->getMemberName()];
  Node->ResolvedType = Info.Members[Index].second;
}
