// Day 0/1 starter: a trivial ClangTool that parses a file and walks the AST,
// printing every ForStmt and CallExpr it finds. This is the "hello world"
// checkpoint from the roadmap — once this builds and runs on a benchmark
// file, the environment-setup risk is basically retired.
//
// Build:
//   mkdir build && cd build
//   cmake -G Ninja .. -DCMAKE_PREFIX_PATH=<path to LLVM/Clang cmake config>
//   ninja
// Run:
//   ./p05tool ../benchmarks/example.c --

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;

namespace {

class LoopAndCallVisitor : public RecursiveASTVisitor<LoopAndCallVisitor> {
public:
  explicit LoopAndCallVisitor(ASTContext *Context) : Context(Context) {}

  bool VisitForStmt(ForStmt *FS) {
    auto &SM = Context->getSourceManager();
    llvm::outs() << "Found ForStmt at "
                 << FS->getBeginLoc().printToString(SM) << "\n";
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
      llvm::outs() << "  Found call to '" << Callee->getNameAsString()
                   << "' at "
                   << CE->getBeginLoc().printToString(
                          Context->getSourceManager())
                   << "\n";
    }
    return true;
  }

private:
  ASTContext *Context;
};

class LoopAndCallConsumer : public ASTConsumer {
public:
  explicit LoopAndCallConsumer(ASTContext *Context) : Visitor(Context) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  LoopAndCallVisitor Visitor;
};

class LoopAndCallAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef File) override {
    return std::make_unique<LoopAndCallConsumer>(&CI.getASTContext());
  }
};

} // namespace

static llvm::cl::OptionCategory ToolCategory("p05tool options");

int main(int argc, const char **argv) {
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, ToolCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());

  return Tool.run(newFrontendActionFactory<LoopAndCallAction>().get());
}
