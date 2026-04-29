#ifndef AST_H
#define AST_H

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <llvm/IR/Type.h>
#include <map>

#include <memory>
#include <string>
#include <vector>

enum class TypeKind { Double, Int };

struct VarType {
  TypeKind Kind;
};

enum class TypeCategory { Int, Double, Struct, Char };

struct MyType {
  TypeCategory Category;
  std::string Name;
  int PointerLevel = 0;

  MyType() : Category(TypeCategory::Double), Name(""), PointerLevel(0) {}

  MyType(TypeKind TK) {
    if (TK == TypeKind::Int)
      Category = TypeCategory::Int;
    else
      Category = TypeCategory::Double;
    Name = "";
  }
  MyType(TypeCategory Cat, std::string N = "") : Category(Cat), Name(N) {}
};

llvm::Type *getLLVMType(MyType T);

struct StructInfo {
  std::string Name;
  std::vector<std::pair<std::string, MyType>> Members;
  std::map<std::string, unsigned> MemberIndex;
};

struct ArgInfo {
  std::string Name;
  MyType Type;
};

namespace llvm {
class Value;
class Function;
} // namespace llvm

// Base class for all expression nodes.
class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual llvm::Value *codegen() = 0;
  virtual MyType getType() = 0;

  virtual llvm::Value *getLValue() { return nullptr; }
};

// Expression class for: struct Name { x: double, y: double }
class StructDefinitionAST {
  std::string Name;
  std::vector<std::pair<std::string, MyType>> Members;

public:
  StructDefinitionAST(std::string Name,
                      std::vector<std::pair<std::string, MyType>> Members)
      : Name(Name), Members(std::move(Members)) {}
  llvm::Type *codegen();
};

// Expression class for member access: p.x
class MemberAccessExprAST : public ExprAST {
  std::unique_ptr<ExprAST> StructExpr;
  std::string MemberName;

public:
  MemberAccessExprAST(std::unique_ptr<ExprAST> StructExpr,
                      std::string MemberName)
      : StructExpr(std::move(StructExpr)), MemberName(MemberName) {}
  llvm::Value *codegen() override;
  MyType getType() override;
  llvm::Value *getLValue() override;
};

// Expression class for numeric literals.
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
  llvm::Value *codegen() override;
  MyType getType() override { return MyType(TypeCategory::Double); }
};

// Expression class for global variables.
class GlobalVarAST : public ExprAST {
  std::string Name;
  MyType Ty;
  double InitVal;

public:
  GlobalVarAST(const std::string &Name, MyType Ty, double InitVal)
      : Name(Name), Ty(Ty), InitVal(InitVal) {}
  llvm::Value *codegen() override;
  MyType getType() override { return Ty; }
};

// Expression class for unary variables / pointers.
class UnaryExprAST : public ExprAST {
  char Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
      : Opcode(Opcode), Operand(std::move(Operand)) {}
  virtual ~UnaryExprAST() {}

  llvm::Value *codegen() override;

  char getOpcode() const { return Opcode; }
  ExprAST *getOperand() const { return Operand.get(); }
  MyType getType() override;
  llvm::Value *getLValue() override;
};

// Expression class for creating variables
class VarExprAST : public ExprAST {
  std::string Name;
  MyType Ty;
  std::unique_ptr<ExprAST> Init;

public:
  VarExprAST(std::string Name, MyType Ty, std::unique_ptr<ExprAST> Init)
      : Name(std::move(Name)), Ty(std::move(Ty)), Init(std::move(Init)) {}
  llvm::Value *codegen() override;
  MyType getType() override { return Ty; }
};

// Expression class for referencing variables.
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
  const std::string &getName() const { return Name; }
  llvm::Value *codegen() override;
  MyType getType() override;
  llvm::Value *getLValue() override;
};

// Expression class for strings : ).
class StringExprAST : public ExprAST {
  std::string Val;

public:
  StringExprAST(const std::string &Val) : Val(Val) {}
  llvm::Value *codegen() override;
  size_t getLength() const { return Val.length(); }
  MyType getType() override {
    MyType T(TypeCategory::Int);
    T.PointerLevel = 1;
    return T;
  }
};

// Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  char &getOp() { return Op; };
  std::unique_ptr<ExprAST> &getLHS() { return LHS; };
  std::unique_ptr<ExprAST> &getRHS() { return RHS; };

  MyType getType() override;

  llvm::Value *codegen() override;

  llvm::Value *getLValue() override;
};

// Expression class for function calls.
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
  llvm::Value *codegen() override;
  MyType getType() override;
};

class PrototypeAST {
  std::string Name;
  std::vector<ArgInfo> Args;
  MyType RetType;

public:
  PrototypeAST(const std::string &Name, std::vector<ArgInfo> Args,
               MyType RetType)
      : Name(Name), Args(std::move(Args)), RetType(RetType) {}

  const std::string &getName() const { return Name; }
  llvm::Function *codegen();

  MyType getArgType(size_t i) const { return Args[i].Type; }
  MyType getRetType() const { return RetType; }
};

class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
  llvm::Function *codegen();
};

class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

public:
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
      : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}

  llvm::Value *codegen() override;
  MyType getType() override;
};

class ForExprAST : public ExprAST {
  std::string VarName;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Start,
             std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step,
             std::unique_ptr<ExprAST> Body)
      : VarName(VarName), Start(std::move(Start)), End(std::move(End)),
        Step(std::move(Step)), Body(std::move(Body)) {}

  llvm::Value *codegen() override;
  MyType getType() override { return MyType(TypeCategory::Double); }
};

class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Expressions;

public:
  BlockExprAST(std::vector<std::unique_ptr<ExprAST>> Expressions)
      : Expressions(std::move(Expressions)) {}

  llvm::Value *codegen() override;
  MyType getType() override;
};

#endif // !AST_H
