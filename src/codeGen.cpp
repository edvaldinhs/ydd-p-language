#include "../include/ast.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>

struct SymbolInfo {
  llvm::Value *V;
  MyType Type;
};

std::unique_ptr<llvm::LLVMContext> TheContext;
std::unique_ptr<llvm::IRBuilder<>> TheBuilder;
std::unique_ptr<llvm::Module> TheModule;
std::map<std::string, SymbolInfo> NamedValues;

std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
std::map<std::string, llvm::GlobalVariable *> GlobalValues;

std::map<std::string, StructInfo> StructDefs;
std::map<std::string, llvm::StructType *> StructTypeMap;

extern int CurLine;
extern int CurCol;

llvm::Value *LogErrorV(const char *Str) {
  fprintf(stderr, "Error [Line %d, Col %d]: %s\n", CurLine, CurCol, Str);
  return nullptr;
}

llvm::Function *LogErrorF(const char *Str) {
  fprintf(stderr, "Error [Line %d, Col %d]: %s\n", CurLine, CurCol, Str);
  return nullptr;
}

llvm::Function *getFunction(std::string Name) {
  if (auto *F = TheModule->getFunction(Name))
    return F;

  auto FI = FunctionProtos.find(Name);
  if (FI != FunctionProtos.end())
    return FI->second->codegen();

  return nullptr;
}

llvm::Type *getLLVMType(MyType T) {
  llvm::Type *BaseTy = nullptr;

  switch (T.Category) {
  case TypeCategory::Char:
    BaseTy = llvm::Type::getInt8Ty(*TheContext);
    break;
  case TypeCategory::Int:
    BaseTy = llvm::Type::getInt32Ty(*TheContext);
    break;
  case TypeCategory::Double:
    BaseTy = llvm::Type::getDoubleTy(*TheContext);
    break;
  case TypeCategory::Struct:
    BaseTy = StructTypeMap[T.Name];
    break;
  }

  if (!BaseTy)
    return nullptr;

  for (int i = 0; i < T.PointerLevel; ++i) {
    BaseTy = llvm::PointerType::get(*TheContext, 0);
  }
  return BaseTy;
}

llvm::Value *EmitCast(llvm::Value *V, llvm::Type *DestTy) {
  llvm::Type *SrcTy = V->getType();
  if (SrcTy == DestTy)
    return V;
  if (SrcTy->isIntegerTy() && DestTy->isDoubleTy())
    return TheBuilder->CreateSIToFP(V, DestTy, "itofp");
  if (SrcTy->isDoubleTy() && DestTy->isIntegerTy())
    return TheBuilder->CreateFPToSI(V, DestTy, "fptosi");

  return V;
}

static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                                const std::string &VarName,
                                                llvm::Type *Ty) {
  llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(Ty, nullptr, VarName);
}

MyType VariableExprAST::getType() {
  if (NamedValues.count(Name))
    return NamedValues[Name].Type;
  return MyType(TypeCategory::Double);
}
llvm::Value *VariableExprAST::getLValue() {

  auto it = NamedValues.find(Name);
  if (it != NamedValues.end()) {
    return it->second.V;
  }

  if (GlobalValues.count(Name)) {
    return GlobalValues[Name];
  }

  fprintf(stderr, "Error [Line %d, Col %d]: Unknown variable name %s\n",
          CurLine, CurCol, Name.c_str());
  return nullptr;
}

llvm::Value *VariableExprAST::codegen() {
  auto it = NamedValues.find(Name);
  if (it != NamedValues.end()) {
    llvm::Value *V = it->second.V;
    auto *Alloca = llvm::cast<llvm::AllocaInst>(V);
    return TheBuilder->CreateLoad(Alloca->getAllocatedType(), V, Name.c_str());
  }

  if (llvm::GlobalVariable *G = TheModule->getNamedGlobal(Name)) {
    return TheBuilder->CreateLoad(G->getValueType(), G, Name.c_str());
  }

  return LogErrorV("Unknown variable name");
}

MyType UnaryExprAST::getType() {
  MyType T = Operand->getType();
  if (Opcode == '*') {
    if (T.PointerLevel > 0)
      T.PointerLevel--;
  } else if (Opcode == '&') {
    T.PointerLevel++;
  }
  return T;
}

llvm::Value *UnaryExprAST::getLValue() {
  if (Opcode == '*') {
    return Operand->codegen();
  }
  return nullptr;
}

llvm::Value *UnaryExprAST::codegen() {

  if (Opcode == '&') {
    return Operand->getLValue();
  }

  if (Opcode == '*') {

    llvm::Value *Addr = Operand->codegen();
    if (!Addr) {
      return nullptr;
    }

    return TheBuilder->CreateLoad(getLLVMType(this->getType()), Addr,
                                  "ptr_val");
  }

  return nullptr;
}

llvm::Value *BinaryExprAST::getLValue() {
  if (Op == '.') {

    llvm::Value *StructAddr = LHS->getLValue();
    if (!StructAddr) {
      return nullptr;
    }

    auto *MemExpr = static_cast<VariableExprAST *>(RHS.get());
    std::string MemberName = MemExpr->getName();
    std::string StructName = LHS->getType().Name;

    if (StructDefs.find(StructName) == StructDefs.end()) {
      return nullptr;
    }

    unsigned MemberIdx = StructDefs[StructName].MemberIndex[MemberName];

    return TheBuilder->CreateStructGEP(StructTypeMap[StructName], StructAddr,
                                       MemberIdx);
  }

  return nullptr;
}

MyType BinaryExprAST::getType() {
  if (Op == '.') {
    MyType LHSStore = LHS->getType();
    if (LHSStore.Category == TypeCategory::Struct) {
      auto &SInfo = StructDefs[LHSStore.Name];
      auto *MemberVar = dynamic_cast<VariableExprAST *>(RHS.get());
      if (MemberVar) {
        for (auto &m : SInfo.Members) {
          if (m.first == MemberVar->getName())
            return m.second;
        }
      }
    }
  }

  MyType L = LHS->getType();
  MyType R = RHS->getType();
  if (L.Category == TypeCategory::Double || R.Category == TypeCategory::Double)
    return MyType(TypeCategory::Double);
  return L;
}

llvm::Value *BinaryExprAST::codegen() {
  if (auto *V = dynamic_cast<VariableExprAST *>(LHS.get())) {
  } else if (auto *B = dynamic_cast<BinaryExprAST *>(LHS.get())) {
  }
  if (Op == '=') {
    if (auto *U = dynamic_cast<UnaryExprAST *>(LHS.get())) {
    } else if (auto *B = dynamic_cast<BinaryExprAST *>(LHS.get())) {
    } else if (auto *V = dynamic_cast<VariableExprAST *>(LHS.get())) {
    }

    llvm::Value *LHSAddr = LHS->getLValue();
    if (!LHSAddr) {
      return LogErrorV("Destination of '=' must be an L-Value (Cannot assign "
                       "to this expression)");
    }

    llvm::Value *Val = RHS->codegen();
    if (!Val) {
      return nullptr;
    }

    Val = EmitCast(Val, getLLVMType(LHS->getType()));
    TheBuilder->CreateStore(Val, LHSAddr);

    return Val;
  }

  if (Op == '.') {
    llvm::Value *Ptr = this->getLValue();
    if (!Ptr)
      return nullptr;
    return TheBuilder->CreateLoad(getLLVMType(getType()), Ptr, "structtmp");
  }

  llvm::Value *L = LHS->codegen();
  llvm::Value *R = RHS->codegen();
  if (!L || !R)
    return nullptr;

  llvm::Type *CommonTy = L->getType();
  if (R->getType()->isDoubleTy())
    CommonTy = R->getType();

  L = EmitCast(L, CommonTy);
  R = EmitCast(R, CommonTy);

  bool isDouble = CommonTy->isDoubleTy();

  switch (Op) {
  case '+':
    return isDouble ? TheBuilder->CreateFAdd(L, R, "addtmp")
                    : TheBuilder->CreateAdd(L, R, "addtmp");
  case '-':
    return isDouble ? TheBuilder->CreateFSub(L, R, "subtmp")
                    : TheBuilder->CreateSub(L, R, "subtmp");
  case '*':
    return isDouble ? TheBuilder->CreateFMul(L, R, "multmp")
                    : TheBuilder->CreateMul(L, R, "multmp");
  case '<':
    if (isDouble) {
      L = TheBuilder->CreateFCmpULT(L, R, "cmptmp");
      return TheBuilder->CreateUIToFP(L, llvm::Type::getDoubleTy(*TheContext),
                                      "booltmp");
    } else {
      L = TheBuilder->CreateICmpSLT(L, R, "cmptmp");
      return TheBuilder->CreateZExt(L, llvm::Type::getInt32Ty(*TheContext),
                                    "booltmp");
    }
  default:
    return nullptr;
  }
}

MyType MemberAccessExprAST::getType() {
  MyType BaseTy = StructExpr->getType();
  auto &SInfo = StructDefs[BaseTy.Name];
  for (auto &m : SInfo.Members) {
    if (m.first == MemberName)
      return m.second;
  }
  return MyType(TypeCategory::Double);
}

llvm::Value *MemberAccessExprAST::getLValue() {
  llvm::Value *StructPtr = StructExpr->getLValue();
  if (!StructPtr)
    return nullptr;

  MyType BaseTy = StructExpr->getType();
  if (BaseTy.Category != TypeCategory::Struct)
    return nullptr;

  llvm::StructType *STy = StructTypeMap[BaseTy.Name];
  if (!STy)
    return nullptr;

  unsigned Index = StructDefs[BaseTy.Name].MemberIndex[MemberName];
  return TheBuilder->CreateStructGEP(STy, StructPtr, Index);
}

llvm::Value *MemberAccessExprAST::codegen() {
  llvm::Value *Ptr = this->getLValue();
  if (!Ptr)
    return nullptr;

  MyType BaseTy = StructExpr->getType();
  llvm::StructType *STy = StructTypeMap[BaseTy.Name];
  unsigned Index = StructDefs[BaseTy.Name].MemberIndex[MemberName];

  return TheBuilder->CreateLoad(STy->getElementType(Index), Ptr,
                                MemberName.c_str());
}

llvm::Value *NumberExprAST::codegen() {
  if (Val == static_cast<int64_t>(Val)) {
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*TheContext),
                                  static_cast<int64_t>(Val));
  }
  return llvm::ConstantFP::get(*TheContext, llvm::APFloat(Val));
}

llvm::Value *StringExprAST::codegen() {
  return TheBuilder->CreateGlobalString(Val);
}

llvm::Value *GlobalVarAST::codegen() {
  llvm::Type *Type = getLLVMType(Ty);

  llvm::Constant *Initializer;
  if (Ty.Category == TypeCategory::Int)
    Initializer = llvm::ConstantInt::get(Type, (int64_t)InitVal);
  else
    Initializer = llvm::ConstantFP::get(Type, InitVal);

  auto *GV = new llvm::GlobalVariable(*TheModule, Type, false,
                                      llvm::GlobalValue::ExternalLinkage,
                                      Initializer, Name);
  return GV;
}

llvm::Value *VarExprAST::codegen() {
  llvm::Function *TheFunction = TheBuilder->GetInsertBlock()->getParent();

  llvm::Type *LLVMTy = getLLVMType(Ty);
  llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Name, LLVMTy);

  if (Init) {
    llvm::Value *InitVal = Init->codegen();
    if (!InitVal)
      return nullptr;

    InitVal = EmitCast(InitVal, LLVMTy);
    TheBuilder->CreateStore(InitVal, Alloca);
  }

  NamedValues[Name] = {Alloca, MyType(Ty)};
  return Alloca;
}

llvm::Value *GenerateStrLen(llvm::Value *StrPtr) {
  llvm::Function *TheFunction = TheBuilder->GetInsertBlock()->getParent();

  llvm::BasicBlock *EntryBB = TheBuilder->GetInsertBlock();
  llvm::BasicBlock *LoopBB =
      llvm::BasicBlock::Create(*TheContext, "strlen.loop", TheFunction);
  llvm::BasicBlock *EndBB =
      llvm::BasicBlock::Create(*TheContext, "strlen.end", TheFunction);

  TheBuilder->CreateBr(LoopBB);
  TheBuilder->SetInsertPoint(LoopBB);

  llvm::PHINode *Idx =
      TheBuilder->CreatePHI(llvm::Type::getInt64Ty(*TheContext), 2, "idx");
  Idx->addIncoming(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 0), EntryBB);

  llvm::Value *CharAddr = TheBuilder->CreateInBoundsGEP(
      llvm::Type::getInt8Ty(*TheContext), StrPtr, Idx, "charaddr");
  llvm::Value *Char = TheBuilder->CreateLoad(llvm::Type::getInt8Ty(*TheContext),
                                             CharAddr, "char");

  llvm::Value *IsEnd = TheBuilder->CreateICmpEQ(
      Char, llvm::ConstantInt::get(llvm::Type::getInt8Ty(*TheContext), 0),
      "isend");

  llvm::Value *NextIdx = TheBuilder->CreateAdd(
      Idx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 1),
      "nextidx");
  Idx->addIncoming(NextIdx, LoopBB);

  TheBuilder->CreateCondBr(IsEnd, EndBB, LoopBB);

  TheBuilder->SetInsertPoint(EndBB);
  return Idx;
}

llvm::Value *GeneratePrintSyscall(llvm::Value *StrPtr, llvm::Value *Len) {
  llvm::FunctionType *FTy = llvm::FunctionType::get(
      llvm::Type::getInt64Ty(*TheContext),
      {llvm::Type::getInt64Ty(*TheContext), llvm::Type::getInt64Ty(*TheContext),
       StrPtr->getType(), llvm::Type::getInt64Ty(*TheContext)},
      false);

  llvm::InlineAsm *IA = llvm::InlineAsm::get(
      FTy, "syscall", "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11}", true);

  llvm::Value *SyscallNum =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 1);
  llvm::Value *FileDesc =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 1);

  return TheBuilder->CreateCall(FTy, IA, {SyscallNum, FileDesc, StrPtr, Len});
}

MyType CallExprAST::getType() {
  if (Callee == "print") {
    return MyType(TypeCategory::Int);
  }

  auto it = FunctionProtos.find(Callee);
  if (it != FunctionProtos.end())
    return it->second->getRetType();

  return MyType(TypeCategory::Double);
}

llvm::Value *CallExprAST::codegen() {
  if (Callee == "print") {
    if (Args.empty())
      return LogErrorV("print expects 1 argument");
    llvm::Value *StrPtr = Args[0]->codegen();
    if (!StrPtr)
      return nullptr;
    llvm::Value *Len = GenerateStrLen(StrPtr);
    return GeneratePrintSyscall(StrPtr, Len);
  }

  llvm::Function *CalleeF = getFunction(Callee);
  if (!CalleeF)
    return LogErrorV("Unknown function referenced");

  std::vector<llvm::Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    llvm::Value *ArgV = Args[i]->codegen();
    if (!ArgV)
      return nullptr;

    llvm::Type *ParamTy = CalleeF->getFunctionType()->getParamType(i);
    ArgsV.push_back(EmitCast(ArgV, ParamTy));
  }

  return TheBuilder->CreateCall(CalleeF, ArgsV, "calltmp");
}

MyType IfExprAST::getType() { return Then->getType(); }

llvm::Value *IfExprAST::codegen() {
  llvm::Value *CondV = Cond->codegen();
  if (!CondV)
    return nullptr;

  if (CondV->getType()->isIntegerTy()) {
    CondV = TheBuilder->CreateICmpNE(
        CondV, llvm::ConstantInt::get(CondV->getType(), 0), "ifcond");
  } else {
    CondV = TheBuilder->CreateFCmpONE(
        CondV, llvm::ConstantFP::get(*TheContext, llvm::APFloat(0.0)),
        "ifcond");
  }

  llvm::Function *TheFunction = TheBuilder->GetInsertBlock()->getParent();

  llvm::BasicBlock *ThenBB =
      llvm::BasicBlock::Create(*TheContext, "then", TheFunction);
  llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(*TheContext, "else");
  llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(*TheContext, "ifcont");

  TheBuilder->CreateCondBr(CondV, ThenBB, ElseBB);

  TheBuilder->SetInsertPoint(ThenBB);
  llvm::Value *ThenV = Then->codegen();
  if (!ThenV)
    return nullptr;
  TheBuilder->CreateBr(MergeBB);
  ThenBB = TheBuilder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), ElseBB);
  TheBuilder->SetInsertPoint(ElseBB);
  llvm::Value *ElseV = Else->codegen();
  if (!ElseV)
    return nullptr;
  TheBuilder->CreateBr(MergeBB);
  ElseBB = TheBuilder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), MergeBB);
  TheBuilder->SetInsertPoint(MergeBB);

  llvm::Type *ResTy = ThenV->getType();
  if (ElseV->getType()->isDoubleTy())
    ResTy = ElseV->getType();

  ThenV = EmitCast(ThenV, ResTy);
  ElseV = EmitCast(ElseV, ResTy);

  llvm::PHINode *PN = TheBuilder->CreatePHI(ResTy, 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);

  return PN;
}

llvm::Value *ForExprAST::codegen() {
  llvm::Value *StartVal = Start->codegen();
  if (!StartVal)
    return nullptr;

  llvm::Function *TheFunction = TheBuilder->GetInsertBlock()->getParent();

  llvm::Type *VarTy = StartVal->getType();
  llvm::AllocaInst *Alloca =
      CreateEntryBlockAlloca(TheFunction, VarName, VarTy);

  TheBuilder->CreateStore(StartVal, Alloca);

  llvm::BasicBlock *LoopBB =
      llvm::BasicBlock::Create(*TheContext, "loop", TheFunction);

  TheBuilder->CreateBr(LoopBB);
  TheBuilder->SetInsertPoint(LoopBB);

  SymbolInfo OldVal;
  bool hadOldValue = false;
  if (NamedValues.count(VarName)) {
    OldVal = NamedValues[VarName];
    hadOldValue = true;
  }
  NamedValues[VarName] = {Alloca, MyType(TypeCategory::Double)};

  if (!Body->codegen())
    return nullptr;

  llvm::Value *StepVal = nullptr;
  if (Step) {
    StepVal = Step->codegen();
    if (!StepVal)
      return nullptr;
    StepVal = EmitCast(StepVal, VarTy);
  } else {
    if (VarTy->isDoubleTy())
      StepVal = llvm::ConstantFP::get(*TheContext, llvm::APFloat(1.0));
    else
      StepVal = llvm::ConstantInt::get(VarTy, 1);
  }

  llvm::Value *CurVar =
      TheBuilder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName);

  llvm::Value *NextVar;
  if (VarTy->isDoubleTy())
    NextVar = TheBuilder->CreateFAdd(CurVar, StepVal, "nextvar");
  else
    NextVar = TheBuilder->CreateAdd(CurVar, StepVal, "nextvar");

  TheBuilder->CreateStore(NextVar, Alloca);

  llvm::Value *EndCond = End->codegen();
  if (!EndCond)
    return nullptr;

  if (EndCond->getType()->isDoubleTy()) {
    EndCond = TheBuilder->CreateFCmpONE(
        EndCond, llvm::ConstantFP::get(*TheContext, llvm::APFloat(0.0)),
        "loopcond");
  } else {
    EndCond = TheBuilder->CreateICmpNE(
        EndCond, llvm::ConstantInt::get(EndCond->getType(), 0), "loopcond");
  }

  llvm::BasicBlock *AfterBB =
      llvm::BasicBlock::Create(*TheContext, "afterloop", TheFunction);
  TheBuilder->CreateCondBr(EndCond, LoopBB, AfterBB);
  TheBuilder->SetInsertPoint(AfterBB);

  if (hadOldValue)
    NamedValues[VarName] = OldVal;
  else
    NamedValues.erase(VarName);

  return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*TheContext));
}

MyType BlockExprAST::getType() {
  if (Expressions.empty())
    return MyType(TypeCategory::Double);
  return Expressions.back()->getType();
}

llvm::Value *BlockExprAST::codegen() {
  llvm::Value *LastVal = nullptr;
  for (auto &E : Expressions) {
    LastVal = E->codegen();
    if (!LastVal)
      return nullptr;
  }

  if (!LastVal)
    return llvm::ConstantFP::get(*TheContext, llvm::APFloat(0.0));

  return LastVal;
}

llvm::Type *StructDefinitionAST::codegen() {
  if (StructTypeMap.count(Name)) {
    return StructTypeMap[Name];
  }

  std::vector<llvm::Type *> ElementTypes;
  StructInfo Info;
  Info.Name = Name;

  for (unsigned i = 0; i < Members.size(); ++i) {
    ElementTypes.push_back(getLLVMType(Members[i].second));
    Info.Members.push_back(Members[i]);
    Info.MemberIndex[Members[i].first] = i;
  }

  llvm::StructType *ST =
      llvm::StructType::create(*TheContext, ElementTypes, Name);
  StructTypeMap[Name] = ST;
  StructDefs[Name] = Info;
  return ST;
}

llvm::Function *PrototypeAST::codegen() {
  std::vector<llvm::Type *> ArgTypes;
  for (auto &Arg : Args)
    ArgTypes.push_back(getLLVMType(MyType(Arg.Type)));

  llvm::FunctionType *FT =
      llvm::FunctionType::get(getLLVMType(MyType(RetType)), ArgTypes, false);

  llvm::Function *F = llvm::Function::Create(
      FT, llvm::Function::ExternalLinkage, Name, TheModule.get());

  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Args[Idx++].Name);

  return F;
}

llvm::Function *FunctionAST::codegen() {
  auto &P = *Proto;
  FunctionProtos[P.getName()] = std::move(Proto);
  llvm::Function *TheFunction = getFunction(P.getName());

  if (!TheFunction)
    return nullptr;

  TheFunction->setLinkage(llvm::Function::ExternalLinkage);
  TheFunction->setVisibility(llvm::Function::DefaultVisibility);

  llvm::BasicBlock *BB =
      llvm::BasicBlock::Create(*TheContext, "entry", TheFunction);
  TheBuilder->SetInsertPoint(BB);

  NamedValues.clear();
  unsigned Idx = 0;
  for (auto &Arg : TheFunction->args()) {
    llvm::Type *ArgTy = getLLVMType(P.getArgType(Idx));

    llvm::AllocaInst *Alloca =
        CreateEntryBlockAlloca(TheFunction, std::string(Arg.getName()), ArgTy);
    TheBuilder->CreateStore(&Arg, Alloca);
    NamedValues[std::string(Arg.getName())] = {Alloca,
                                               MyType(P.getArgType(Idx))};
    Idx++;
  }

  if (llvm::Value *RetVal = Body->codegen()) {
    RetVal = EmitCast(RetVal, TheFunction->getReturnType());

    TheBuilder->CreateRet(RetVal);
    llvm::verifyFunction(*TheFunction);
    return TheFunction;
  }

  TheFunction->eraseFromParent();
  return nullptr;
}
