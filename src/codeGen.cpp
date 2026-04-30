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

llvm::Value *LogErrorV(const std::string &Str) {
  fprintf(stderr, "Error [Line %d, Col %d]: %s\n", CurLine, CurCol,
          Str.c_str());
  return nullptr;
}

llvm::Function *LogErrorF(const std::string &Str) {
  fprintf(stderr, "Error [Line %d, Col %d]: %s\n", CurLine, CurCol,
          Str.c_str());
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

llvm::FunctionCallee GetMalloc() {
  return TheModule->getOrInsertFunction("malloc",
                                        llvm::PointerType::get(*TheContext, 0),
                                        llvm::Type::getInt64Ty(*TheContext));
}

llvm::FunctionCallee GetFree() {
  return TheModule->getOrInsertFunction("free",
                                        llvm::Type::getVoidTy(*TheContext),
                                        llvm::PointerType::get(*TheContext, 0));
}

std::string ProcessEscapeSequences(const std::string &input) {
  std::string unescaped;
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\\' && i + 1 < input.size()) {
      switch (input[i + 1]) {
      case 'n':
        unescaped += '\n';
        break;
      case 't':
        unescaped += '\t';
        break;
      case 'r':
        unescaped += '\r';
        break;
      case '\\':
        unescaped += '\\';
        break;
      case '\"':
        unescaped += '\"';
        break;
      default:
        unescaped += input[i + 1];
        break;
      }
      i++;
    } else {
      unescaped += input[i];
    }
  }
  return unescaped;
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
  if (Opcode == '-') {
    llvm::Value *V = Operand->codegen();
    if (!V)
      return nullptr;

    if (V->getType()->isDoubleTy())
      return TheBuilder->CreateFNeg(V, "negtmp");

    return TheBuilder->CreateNeg(V, "negtmp");
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

  if (Op == '+') {
    if (L.PointerLevel > 0 || R.PointerLevel > 0) {
      MyType StrTy(TypeCategory::Int);
      StrTy.PointerLevel = 1;
      return StrTy;
    }
  }
  if (L.Category == TypeCategory::Double || R.Category == TypeCategory::Double)
    return MyType(TypeCategory::Double);
  return L;
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

llvm::Value *GenerateStrCat(llvm::Value *L, llvm::Value *R) {
  llvm::Function *F = TheBuilder->GetInsertBlock()->getParent();
  llvm::Type *Int8Ty = llvm::Type::getInt8Ty(*TheContext);
  llvm::Type *Int64Ty = llvm::Type::getInt64Ty(*TheContext);

  llvm::Value *LenL = GenerateStrLen(L);
  llvm::Value *LenR = GenerateStrLen(R);
  llvm::Value *TotalLen = TheBuilder->CreateAdd(LenL, LenR);
  llvm::Value *AllocLen = TheBuilder->CreateAdd(
      TotalLen, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 1));

  llvm::Value *NewStr =
      TheBuilder->CreateCall(GetMalloc(), {AllocLen}, "str_heap");

  llvm::BasicBlock *CopyLBB = llvm::BasicBlock::Create(*TheContext, "copyl", F);
  llvm::BasicBlock *CopyLCheckBB =
      llvm::BasicBlock::Create(*TheContext, "copyl_chk", F);
  llvm::BasicBlock *CopyRBB = llvm::BasicBlock::Create(*TheContext, "copyr", F);
  llvm::BasicBlock *CopyRCheckBB =
      llvm::BasicBlock::Create(*TheContext, "copyr_chk", F);
  llvm::BasicBlock *EndBB =
      llvm::BasicBlock::Create(*TheContext, "strcat_end", F);

  TheBuilder->CreateBr(CopyLCheckBB);
  TheBuilder->SetInsertPoint(CopyLCheckBB);
  llvm::PHINode *IdxL = TheBuilder->CreatePHI(Int64Ty, 2, "idx_l");
  IdxL->addIncoming(llvm::ConstantInt::get(Int64Ty, 0),
                    TheBuilder->GetInsertBlock()->getSinglePredecessor());

  llvm::Value *CondL = TheBuilder->CreateICmpSLT(IdxL, LenL);
  TheBuilder->CreateCondBr(CondL, CopyLBB, CopyRCheckBB);

  TheBuilder->SetInsertPoint(CopyLBB);
  llvm::Value *SrcPtrL = TheBuilder->CreateInBoundsGEP(Int8Ty, L, IdxL);
  llvm::Value *DstPtrL = TheBuilder->CreateInBoundsGEP(Int8Ty, NewStr, IdxL);
  TheBuilder->CreateStore(TheBuilder->CreateLoad(Int8Ty, SrcPtrL), DstPtrL);

  llvm::Value *NextIdxL =
      TheBuilder->CreateAdd(IdxL, llvm::ConstantInt::get(Int64Ty, 1));
  IdxL->addIncoming(NextIdxL, CopyLBB);
  TheBuilder->CreateBr(CopyLCheckBB);

  TheBuilder->SetInsertPoint(CopyRCheckBB);
  llvm::PHINode *IdxR = TheBuilder->CreatePHI(Int64Ty, 2, "idx_r");
  IdxR->addIncoming(llvm::ConstantInt::get(Int64Ty, 0), CopyLCheckBB);

  llvm::Value *CondR = TheBuilder->CreateICmpSLT(IdxR, LenR);
  TheBuilder->CreateCondBr(CondR, CopyRBB, EndBB);

  TheBuilder->SetInsertPoint(CopyRBB);
  llvm::Value *SrcPtrR = TheBuilder->CreateInBoundsGEP(Int8Ty, R, IdxR);

  llvm::Value *DstOffset = TheBuilder->CreateAdd(LenL, IdxR);
  llvm::Value *DstPtrR =
      TheBuilder->CreateInBoundsGEP(Int8Ty, NewStr, DstOffset);

  TheBuilder->CreateStore(TheBuilder->CreateLoad(Int8Ty, SrcPtrR), DstPtrR);

  llvm::Value *NextIdxR =
      TheBuilder->CreateAdd(IdxR, llvm::ConstantInt::get(Int64Ty, 1));
  IdxR->addIncoming(NextIdxR, CopyRBB);
  TheBuilder->CreateBr(CopyRCheckBB);

  // --- End ---
  TheBuilder->SetInsertPoint(EndBB);
  llvm::Value *NullPtr =
      TheBuilder->CreateInBoundsGEP(Int8Ty, NewStr, TotalLen);
  TheBuilder->CreateStore(llvm::ConstantInt::get(Int8Ty, 0), NullPtr);

  return NewStr;
}

std::pair<llvm::Value *, llvm::Value *> GenerateItoa(llvm::Value *Val) {
  llvm::Function *F = TheBuilder->GetInsertBlock()->getParent();

  llvm::Type *Int8Ty = llvm::Type::getInt8Ty(*TheContext);
  llvm::Type *Int32Ty = llvm::Type::getInt32Ty(*TheContext);
  llvm::Type *Int64Ty = llvm::Type::getInt64Ty(*TheContext);

  llvm::Value *Buffer = TheBuilder->CreateAlloca(
      Int8Ty, llvm::ConstantInt::get(Int32Ty, 32), "itoa_buf");

  llvm::Value *EndIdx = llvm::ConstantInt::get(Int32Ty, 31);
  llvm::Value *NullPtr = TheBuilder->CreateInBoundsGEP(Int8Ty, Buffer, EndIdx);
  TheBuilder->CreateStore(llvm::ConstantInt::get(Int8Ty, 0), NullPtr);

  llvm::Value *Val64 = TheBuilder->CreateSExt(Val, Int64Ty);

  llvm::Value *Zero64 = llvm::ConstantInt::get(Int64Ty, 0);

  llvm::Value *IsNeg = TheBuilder->CreateICmpSLT(Val64, Zero64, "isneg");

  llvm::Value *NegVal = TheBuilder->CreateSub(Zero64, Val64);

  llvm::Value *AbsVal =
      TheBuilder->CreateSelect(IsNeg, NegVal, Val64, "absval");

  // --- blocks
  llvm::BasicBlock *PreheaderBB = TheBuilder->GetInsertBlock();
  llvm::BasicBlock *LoopBB =
      llvm::BasicBlock::Create(*TheContext, "itoa.loop", F);
  llvm::BasicBlock *EndBB =
      llvm::BasicBlock::Create(*TheContext, "itoa.end", F);
  llvm::BasicBlock *NegBB =
      llvm::BasicBlock::Create(*TheContext, "itoa.neg", F);
  llvm::BasicBlock *ContBB =
      llvm::BasicBlock::Create(*TheContext, "itoa.cont", F);

  llvm::Value *StartIdx = llvm::ConstantInt::get(Int32Ty, 30);

  TheBuilder->CreateBr(LoopBB);

  // --- LOOP ---
  TheBuilder->SetInsertPoint(LoopBB);

  llvm::PHINode *CurrVal = TheBuilder->CreatePHI(Int64Ty, 2, "currval");
  llvm::PHINode *CurrIdx = TheBuilder->CreatePHI(Int32Ty, 2, "curridx");

  CurrVal->addIncoming(AbsVal, PreheaderBB);
  CurrIdx->addIncoming(StartIdx, PreheaderBB);

  llvm::Value *Digit =
      TheBuilder->CreateSRem(CurrVal, llvm::ConstantInt::get(Int64Ty, 10));

  llvm::Value *DigitChar =
      TheBuilder->CreateAdd(TheBuilder->CreateTrunc(Digit, Int8Ty),
                            llvm::ConstantInt::get(Int8Ty, '0'));

  llvm::Value *Ptr = TheBuilder->CreateInBoundsGEP(Int8Ty, Buffer, CurrIdx);
  TheBuilder->CreateStore(DigitChar, Ptr);

  llvm::Value *NextVal =
      TheBuilder->CreateSDiv(CurrVal, llvm::ConstantInt::get(Int64Ty, 10));

  llvm::Value *NextIdx =
      TheBuilder->CreateSub(CurrIdx, llvm::ConstantInt::get(Int32Ty, 1));

  CurrVal->addIncoming(NextVal, LoopBB);
  CurrIdx->addIncoming(NextIdx, LoopBB);

  llvm::Value *Cond = TheBuilder->CreateICmpSGT(NextVal, Zero64);

  TheBuilder->CreateCondBr(Cond, LoopBB, EndBB);

  // --- END ---
  TheBuilder->SetInsertPoint(EndBB);

  llvm::Value *FirstDigitIdx =
      TheBuilder->CreateAdd(NextIdx, llvm::ConstantInt::get(Int32Ty, 1));

  TheBuilder->CreateCondBr(IsNeg, NegBB, ContBB);

  // --- NEG ---
  TheBuilder->SetInsertPoint(NegBB);

  llvm::Value *MinusIdx =
      TheBuilder->CreateSub(FirstDigitIdx, llvm::ConstantInt::get(Int32Ty, 1));

  llvm::Value *MinusPtr =
      TheBuilder->CreateInBoundsGEP(Int8Ty, Buffer, MinusIdx);

  TheBuilder->CreateStore(llvm::ConstantInt::get(Int8Ty, '-'), MinusPtr);

  TheBuilder->CreateBr(ContBB);

  // --- CONT ---
  TheBuilder->SetInsertPoint(ContBB);

  llvm::PHINode *FinalIdx = TheBuilder->CreatePHI(Int32Ty, 2, "finalidx");

  FinalIdx->addIncoming(FirstDigitIdx, EndBB);
  FinalIdx->addIncoming(MinusIdx, NegBB);

  llvm::Value *StrPtr = TheBuilder->CreateInBoundsGEP(Int8Ty, Buffer, FinalIdx);

  llvm::Value *Len32 = TheBuilder->CreateSub(EndIdx, FinalIdx);

  llvm::Value *Len = TheBuilder->CreateZExt(Len32, Int64Ty);

  return {StrPtr, Len};
}

llvm::Value *GenerateFtoa(llvm::Value *Val) {
  llvm::Type *DoubleTy = llvm::Type::getDoubleTy(*TheContext);
  llvm::Type *Int64Ty = llvm::Type::getInt64Ty(*TheContext);

  llvm::Value *IntPart = TheBuilder->CreateFPToSI(Val, Int64Ty);
  llvm::Value *IntPartD = TheBuilder->CreateSIToFP(IntPart, DoubleTy);
  llvm::Value *Fract = TheBuilder->CreateFSub(Val, IntPartD);

  llvm::Value *AbsFract = TheBuilder->CreateSelect(
      TheBuilder->CreateFCmpOLT(Fract, llvm::ConstantFP::get(DoubleTy, 0.0)),
      TheBuilder->CreateFNeg(Fract), Fract);

  llvm::Value *ScaledFract = TheBuilder->CreateFMul(
      AbsFract, llvm::ConstantFP::get(DoubleTy, 1000000.0));
  llvm::Value *FractPart = TheBuilder->CreateFPToSI(ScaledFract, Int64Ty);

  auto [IntStr, IntLen] = GenerateItoa(
      TheBuilder->CreateTrunc(IntPart, llvm::Type::getInt32Ty(*TheContext)));
  auto [FractStr, FractLen] = GenerateItoa(
      TheBuilder->CreateTrunc(FractPart, llvm::Type::getInt32Ty(*TheContext)));

  llvm::Value *Dot = TheBuilder->CreateGlobalString(".");
  llvm::Value *WithDot = GenerateStrCat(IntStr, Dot);
  llvm::Value *FinalStr = GenerateStrCat(WithDot, FractStr);

  return FinalStr;
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

  if (Op == '+') {
    llvm::Type *LTy = L->getType();
    llvm::Type *RTy = R->getType();

    bool LIsPtr = LTy->isPointerTy();
    bool RIsPtr = RTy->isPointerTy();

    if (LIsPtr || RIsPtr) {
      llvm::Value *LFinal = L;
      llvm::Value *RFinal = R;

      if (!LIsPtr) {
        if (LTy->isDoubleTy())
          LFinal = GenerateFtoa(L);
        else
          LFinal = std::get<0>(GenerateItoa(
              TheBuilder->CreateTrunc(L, llvm::Type::getInt32Ty(*TheContext))));
      }

      if (!RIsPtr) {
        if (RTy->isDoubleTy())
          RFinal = GenerateFtoa(R);
        else
          RFinal = std::get<0>(GenerateItoa(
              TheBuilder->CreateTrunc(R, llvm::Type::getInt32Ty(*TheContext))));
      }

      return GenerateStrCat(LFinal, RFinal);
    }
  }

  if (L->getType()->isPointerTy() || R->getType()->isPointerTy()) {
    std::string ErrMsg = "Type Error: Cannot apply operator '";
    ErrMsg += Op;
    ErrMsg += "' to a string.";
    return LogErrorV(ErrMsg.c_str());
  }

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
  std::string LetHimCook = ProcessEscapeSequences(Val);
  return TheBuilder->CreateGlobalString(LetHimCook);
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

void GeneratePrintChar(char C) {
  llvm::Type *Int8Ty = llvm::Type::getInt8Ty(*TheContext);
  llvm::Type *Int64Ty = llvm::Type::getInt64Ty(*TheContext);

  llvm::Value *Ptr = TheBuilder->CreateAlloca(Int8Ty, nullptr, "char_tmp");
  TheBuilder->CreateStore(llvm::ConstantInt::get(Int8Ty, C), Ptr);

  GeneratePrintSyscall(Ptr, llvm::ConstantInt::get(Int64Ty, 1));
}

void GeneratePrintDouble(llvm::Value *Val) {
  llvm::Type *DoubleTy = llvm::Type::getDoubleTy(*TheContext);
  llvm::Type *Int64Ty = llvm::Type::getInt64Ty(*TheContext);
  llvm::Function *TheFunction = TheBuilder->GetInsertBlock()->getParent();

  llvm::Value *ZeroD = llvm::ConstantFP::get(DoubleTy, 0.0);
  llvm::Value *IsNeg = TheBuilder->CreateFCmpOLT(Val, ZeroD, "isneg");

  llvm::BasicBlock *EntryBB = TheBuilder->GetInsertBlock();
  llvm::BasicBlock *PrintMinusBB =
      llvm::BasicBlock::Create(*TheContext, "print_minus", TheFunction);
  llvm::BasicBlock *ContBB =
      llvm::BasicBlock::Create(*TheContext, "ftoa_cont", TheFunction);

  TheBuilder->CreateCondBr(IsNeg, PrintMinusBB, ContBB);

  TheBuilder->SetInsertPoint(PrintMinusBB);
  GeneratePrintChar('-');
  llvm::Value *NegVal = TheBuilder->CreateFNeg(Val, "neg_val");
  TheBuilder->CreateBr(ContBB);

  TheBuilder->SetInsertPoint(ContBB);
  llvm::PHINode *AbsVal = TheBuilder->CreatePHI(DoubleTy, 2, "abs_val");
  AbsVal->addIncoming(NegVal, PrintMinusBB);
  AbsVal->addIncoming(Val, EntryBB);

  llvm::Value *IntPart = TheBuilder->CreateFPToSI(AbsVal, Int64Ty, "intpart");
  auto [IntStr, IntLen] = GenerateItoa(IntPart);
  GeneratePrintSyscall(IntStr, IntLen);

  GeneratePrintChar('.');

  llvm::Value *IntPartAsDouble = TheBuilder->CreateSIToFP(IntPart, DoubleTy);
  llvm::Value *Fract = TheBuilder->CreateFSub(AbsVal, IntPartAsDouble, "fract");
  llvm::Value *Scaled =
      TheBuilder->CreateFMul(Fract, llvm::ConstantFP::get(DoubleTy, 1000000.0));

  llvm::Value *FractPart =
      TheBuilder->CreateFPToSI(Scaled, Int64Ty, "fractpart");

  auto [FractStr, FractLen] = GenerateItoa(FractPart);
  GeneratePrintSyscall(FractStr, FractLen);
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
    llvm::Value *LastSyscallResult = nullptr;

    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      bool isTemporary =
          (dynamic_cast<BinaryExprAST *>(Args[i].get()) != nullptr ||
           Args[i]->getType().Category == TypeCategory::Double ||
           Args[i]->getType().Category == TypeCategory::Int);

      llvm::Value *ArgV = Args[i]->codegen();
      if (!ArgV)
        return nullptr;

      llvm::Type *Ty = ArgV->getType();

      if (Ty->isIntegerTy()) {
        auto [StrPtr, Len] = GenerateItoa(ArgV);
        LastSyscallResult = GeneratePrintSyscall(StrPtr, Len);

      } else if (Ty->isPointerTy()) {
        llvm::Value *Len = GenerateStrLen(ArgV);
        LastSyscallResult = GeneratePrintSyscall(ArgV, Len);

      } else if (Ty->isDoubleTy()) {
        GeneratePrintDouble(ArgV);
        LastSyscallResult =
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 0);
        if (isTemporary && ArgV->getType()->isPointerTy()) {
          TheBuilder->CreateCall(GetFree(), {ArgV});
        }
      }
    }
    return LastSyscallResult
               ? LastSyscallResult
               : llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 0);
  }

  llvm::Function *CalleeF = getFunction(Callee);
  if (!CalleeF)
    return LogErrorV("Unknown function referenced");

  if (CalleeF->arg_size() != Args.size())
    return LogErrorV("Incorrect # arguments passed");

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
  NamedValues[VarName] = {Alloca, Start->getType()};

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
