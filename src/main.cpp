#include "../include/ast.h"
#include "../include/lexer.h"
#include "../include/parser.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/TargetSelect.h"

#include "llvm/Support/raw_ostream.h"
#include <iostream>

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> TheBuilder;

int main() {

  SetupPrecedence();
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  TheContext = std::make_unique<llvm::LLVMContext>();
  TheModule = std::make_unique<llvm::Module>("JIT", *TheContext);
  TheBuilder = std::make_unique<llvm::IRBuilder<>>(*TheContext);

  auto ExitOnErr = llvm::ExitOnError();
  auto TheJIT = ExitOnErr(llvm::orc::LLJITBuilder().create());

  std::cout << "Wellcome to Yde compiler!" << std::endl;
  std::cout << ">> ";

  getNextToken();

  while (true) {
    switch (CurTok) {
    case tok_eof:
      return 0;
    case ';':
      getNextToken();
      break;
    case tok_def: {
      if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()) {
          std::cout << "Read function definition:" << std::endl;
          FnIR->print(llvm::errs());
          std::cerr << "\n";
        }
      } else {
        getNextToken();
      }
      break;
    }
    default:
      if (auto FnAST = ParseTopLevelExpr()) {
        if (auto *FnIR = FnAST->codegen()) {

          auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule),
                                                 std::move(TheContext));
          ExitOnErr(TheJIT->addIRModule(std::move(TSM)));

          auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));

          double (*FP)() = ExprSymbol.toPtr<double (*)()>();

          fprintf(stderr, "Evaluated to: %f\n", FP());

          TheContext = std::make_unique<llvm::LLVMContext>();
          TheModule = std::make_unique<llvm::Module>("JIT", *TheContext);
          TheBuilder = std::make_unique<llvm::IRBuilder<>>(*TheContext);
        }
      } else {
        getNextToken();
      }
      break;
    }
    std::cout << ">> ";
  }

  return 0;
}
