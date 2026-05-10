#include "../include/ast.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/sema.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <string>

#include <cstdio>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h>

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> TheBuilder;

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/TargetParser/Host.h"

bool EmitObject = false;

void EmitObjectFile(const std::string &Filename) {
  std::string TripleStr = "i386-pc-none-elf";
  llvm::Triple TargetTriple(TripleStr);

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget("", TargetTriple, Error);

  if (!Target) {
    llvm::errs() << Error;
    return;
  }

  auto CPU = "i686";
  auto Features = "";

  llvm::TargetOptions opt;

  auto RM = std::optional<llvm::Reloc::Model>(llvm::Reloc::Static);

  auto TheTargetMachine =
      Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);

  TheModule->setDataLayout(TheTargetMachine->createDataLayout());
  TheModule->setTargetTriple(TargetTriple);

  for (auto &F : *TheModule) {
    F.addFnAttr(llvm::Attribute::NoRedZone);
    F.addFnAttr(llvm::Attribute::NoUnwind);
    F.addFnAttr("disable-tail-calls", "true");
  }

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
  std::cout << "Wrote 32-bit bare-metal object to " << Filename << "\n";
}

int main(int argc, char **argv) {
  std::string InputFile = "";
  std::string OutputFile = "";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-o") {
      if (i + 1 < argc) {
        OutputFile = argv[++i];
        EmitObject = true;
      } else {
        OutputFile = "kernel";
        EmitObject = true;
      }
    } else {
      InputFile = arg;
    }
  }

  if (!InputFile.empty()) {
    if (!freopen(InputFile.c_str(), "r", stdin)) {
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
  ExitOnErr(TheJIT->getMainJITDylib().define(
      llvm::orc::absoluteSymbols(std::move(Symbols))));

  getNextToken();
  SemanticAnalyzer Sema;

  while (CurTok != tok_eof) {
    switch (CurTok) {
    case ';':
      getNextToken();
      break;
    case tok_def:
      if (auto FnAST = ParseDefinition()) {
        Sema.AnalyzeFunction(FnAST.get());
        FnAST->codegen();
      } else {
        getNextToken();
      }
      break;
    case tok_struct:
      if (auto StructAST = ParseStructDefinition()) {
        Sema.RegisterStruct(StructAST.get());
        StructAST->codegen();
      } else {
        getNextToken();
      }
      break;
    case tok_extern:
      if (auto ProtoAST = ParseExtern()) {
        Sema.DeclareFunction(ProtoAST->getName(), ProtoAST->getRetType());
        ProtoAST->codegen();
      } else {
        getNextToken();
      }
      break;
    case tok_int:
    case tok_double:
      if (auto GlobalAST = ParseGlobal()) {
        Sema.DeclareVariable(GlobalAST->getName(), GlobalAST->getType());
        Sema.Analyze(GlobalAST.get());
        GlobalAST->codegen();
      }
      break;
    case tok_identifier:
      if (PeekToken(0) == ':') {
        if (auto GlobalAST = ParseGlobal()) {
          Sema.DeclareVariable(GlobalAST->getName(), GlobalAST->getType());
          Sema.Analyze(GlobalAST.get());
          GlobalAST->codegen();
        }
      } else {
        if (auto FnAST = ParseTopLevelExpr()) {
          Sema.AnalyzeFunction(FnAST.get());

          if (auto *FnIR = FnAST->codegen()) {
            FnIR->viewCFG();
          }
        } else {
          getNextToken();
        }
      }
      break;
    default:
      if (auto FnAST = ParseTopLevelExpr()) {
        Sema.AnalyzeFunction(FnAST.get());
        FnAST->codegen();

        if (auto *FnIR = FnAST->codegen()) {
          FnIR->viewCFG();
        }
      } else {
        getNextToken();
      }
      break;
    }
  }

  if (EmitObject) {
    EmitObjectFile(OutputFile + ".o");
  } else {
    auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule),
                                           std::move(TheContext));
    ExitOnErr(TheJIT->addIRModule(std::move(TSM)));

    auto MainSymbol = TheJIT->lookup("main");
    if (MainSymbol) {
      auto (*FP)() = MainSymbol->toPtr<double (*)()>();
      FP();
    }
  }

  return 0;
}
