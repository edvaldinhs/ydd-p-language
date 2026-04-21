#include "../include/ast.h"
#include "../include/lexer.h"
#include "../include/parser.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/TargetSelect.h"

#include "llvm/Support/raw_ostream.h"
#include <iostream>

#include <cstdio>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h>

extern "C" double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> TheBuilder;

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

void EmitObjectFile() {
  auto TargetTripleStr = llvm::sys::getDefaultTargetTriple();
  llvm::Triple TargetTriple(TargetTripleStr);

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

  if (!Target) {
    llvm::errs() << Error;
    return;
  }

  auto CPU = "generic";
  auto Features = "";
  llvm::TargetOptions opt;

  auto RM = std::optional<llvm::Reloc::Model>();
  auto TheTargetMachine =
      Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);

  TheModule->setDataLayout(TheTargetMachine->createDataLayout());
  TheModule->setTargetTriple(TargetTriple);

  auto Filename = "output.o";
  std::error_code EC;
  llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

  if (EC) {
    llvm::errs() << "Could not open file: " << EC.message();
    return;
  }

  llvm::legacy::PassManager pass;
  auto FileType = llvm::CodeGenFileType::ObjectFile;

  if (TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
    llvm::errs() << "TheTargetMachine can't emit a file of this type";
    return;
  }

  pass.run(*TheModule);
  dest.flush();
  std::cout << "Wrote " << Filename << "\n";
}
int main(int argc, char **argv) {
  if (argc > 1) {
    if (!freopen(argv[1], "r", stdin)) {
      perror("Could not open file");
      return 1;
    }
  }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  SetupPrecedence();

  TheContext = std::make_unique<llvm::LLVMContext>();
  TheModule = std::make_unique<llvm::Module>("MyJIT", *TheContext);
  TheBuilder = std::make_unique<llvm::IRBuilder<>>(*TheContext);
  auto ExitOnErr = llvm::ExitOnError();
  auto TheJIT = ExitOnErr(llvm::orc::LLJITBuilder().create());

  TheJIT->getMainJITDylib().addGenerator(
      ExitOnErr(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          TheJIT->getDataLayout().getGlobalPrefix())));

  llvm::orc::SymbolMap Symbols;

  Symbols[TheJIT->mangleAndIntern("printd")] = {
      llvm::orc::ExecutorAddr::fromPtr(&printd),
      llvm::JITSymbolFlags::Exported};

  ExitOnErr(TheJIT->getMainJITDylib().define(
      llvm::orc::absoluteSymbols(std::move(Symbols))));

  getNextToken();

  while (CurTok != tok_eof) {
    switch (CurTok) {
    case ';':
      getNextToken();
      break;
    case tok_def:
      if (auto FnAST = ParseDefinition())
        FnAST->codegen();
      else
        getNextToken();
      break;
    case tok_struct:
      if (auto StructAST = ParseStructDefinition())
        StructAST->codegen();
      else
        getNextToken();
      break;
    case tok_extern:
      if (auto ProtoAST = ParseExtern())
        ProtoAST->codegen();
      else
        getNextToken();
      break;
    case tok_int:
    case tok_double:
      if (auto GlobalAST = ParseGlobal())
        GlobalAST->codegen();
      break;
    case tok_identifier:
      if (PeekToken(0) == ':') {
        if (auto GlobalAST = ParseGlobal())
          GlobalAST->codegen();
      } else {
        if (auto FnAST = ParseTopLevelExpr())
          FnAST->codegen();
      }
      break;
    default:
      if (auto FnAST = ParseTopLevelExpr())
        FnAST->codegen();
      else
        getNextToken();
      break;
    }
  }

  auto TSM =
      llvm::orc::ThreadSafeModule(std::move(TheModule), std::move(TheContext));
  ExitOnErr(TheJIT->addIRModule(std::move(TSM)));

  auto MainSymbol = TheJIT->lookup("main");
  if (MainSymbol) {
    auto (*FP)() = MainSymbol->toPtr<double (*)()>();
    fprintf(stderr, "Main returned: %f\n", FP());
  }

  return 0;
}
