#include "../include/ast.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
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

llvm::Value *GetLValueAddress(ExprAST *Expr) {
  if (auto *VE = dynamic_cast<VariableExprAST *>(Expr)) {
    if (NamedValues.count(VE->getName()))
      return NamedValues[VE->getName()].V;
    return TheModule->getNamedGlobal(VE->getName());
  }

  if (auto *BE = dynamic_cast<BinaryExprAST *>(Expr)) {
    if (BE->getOp() == '.') {
      llvm::Value *StructPtr = GetLValueAddress(BE->getLHS().get());
      if (!StructPtr)
        return nullptr;

      if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(StructPtr)) {
        auto *STy = llvm::cast<llvm::StructType>(Alloca->getAllocatedType());

        auto *MemberExpr = dynamic_cast<VariableExprAST *>(BE->getRHS().get());
        if (!MemberExpr)
          return LogErrorV("RHS of '.' must be a member name");

        std::string MemberName = MemberExpr->getName();
        unsigned Index =
            StructDefs[STy->getName().str()].MemberIndex[MemberName];

        return TheBuilder->CreateStructGEP(STy, StructPtr, Index);
      } else {
        return LogErrorV("Struct access on non-alloca not supported yet");
      }
    }
  }
  return nullptr;
}

llvm::Value *EmitCast(llvm::Value *V, llvm::Type *DestTy) {
  llvm::Type *SrcTy = V->getType();
  if (SrcTy == DestTy)
    return V;

  // 2 to 2.0
  if (SrcTy->isIntegerTy() && DestTy->isDoubleTy())
    return TheBuilder->CreateSIToFP(V, DestTy, "itofp");

  // 2.5 to 2
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

llvm::Value *NumberExprAST::codegen() {
  if (Val == static_cast<int64_t>(Val)) {
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*TheContext),
                                  static_cast<int64_t>(Val));
  }
  return llvm::ConstantFP::get(*TheContext, llvm::APFloat(Val));
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

llvm::Type *getLLVMType(MyType T) {
  switch (T.Category) {
  case TypeCategory::Int:
    return llvm::Type::getInt32Ty(*TheContext);
  case TypeCategory::Double:
    return llvm::Type::getDoubleTy(*TheContext);
  case TypeCategory::Struct:
    if (StructTypeMap.count(T.Name))
      return StructTypeMap[T.Name];
    fprintf(stderr, "Unknown struct type: %s\n", T.Name.c_str());
    return nullptr;
  }
  return nullptr;
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

llvm::Value *BinaryExprAST::codegen() {
  if (Op == '=') {
    llvm::Value *VariableAddr = GetLValueAddress(LHS.get());
    if (!VariableAddr)
      return LogErrorV("Destination of '=' must be an L-Value");

    llvm::Value *Val = RHS->codegen();
    if (!Val)
      return nullptr;

    llvm::Type *DestTy = nullptr;
    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(VariableAddr))
      DestTy = Alloca->getAllocatedType();
    else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(VariableAddr))
      DestTy = GEP->getResultElementType();

    if (DestTy)
      Val = EmitCast(Val, DestTy);
    TheBuilder->CreateStore(Val, VariableAddr);
    return Val;
  }

  if (Op == '.') {
    llvm::Value *StructPtr = GetLValueAddress(LHS.get());
    if (!StructPtr)
      return LogErrorV("LHS of '.' must have an address");

    auto *MemberExpr = dynamic_cast<VariableExprAST *>(RHS.get());
    if (!MemberExpr)
      return LogErrorV("RHS of '.' must be a member name");
    std::string MemberName = MemberExpr->getName();

    auto *Alloca = llvm::cast<llvm::AllocaInst>(StructPtr);
    auto *STy = llvm::cast<llvm::StructType>(Alloca->getAllocatedType());
    unsigned Index = StructDefs[STy->getName().str()].MemberIndex[MemberName];

    llvm::Value *MemberPtr = TheBuilder->CreateStructGEP(STy, StructPtr, Index);
    return TheBuilder->CreateLoad(STy->getElementType(Index), MemberPtr,
                                  MemberName.c_str());
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

llvm::Value *CallExprAST::codegen() {
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

llvm::Value *MemberAccessExprAST::codegen() {

  VariableExprAST *V = dynamic_cast<VariableExprAST *>(StructExpr.get());
  if (!V)
    return LogErrorV("Struct access LHS must be a variable");
  llvm::Value *StructPtr = nullptr;
  if (NamedValues.count(V->getName())) {
    StructPtr = NamedValues[V->getName()].V;
  } else {
    StructPtr = TheModule->getNamedGlobal(V->getName());
  }

  if (!StructPtr)
    return LogErrorV("Unknown struct variable");

  llvm::AllocaInst *Alloca = llvm::cast<llvm::AllocaInst>(StructPtr);
  llvm::StructType *STy =
      llvm::cast<llvm::StructType>(Alloca->getAllocatedType());

  std::string TypeName = STy->getName().str();
  unsigned Index = StructDefs[TypeName].MemberIndex[MemberName];

  llvm::Value *MemberPtr = TheBuilder->CreateStructGEP(STy, StructPtr, Index);

  return TheBuilder->CreateLoad(STy->getElementType(Index), MemberPtr,
                                MemberName.c_str());
}
