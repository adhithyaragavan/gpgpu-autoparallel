// p05tool driver — wiring only.
//
// The tool parses a translation unit, runs the loop analysis over it, and
// prints a report. All actual analysis lives in src/analysis/; codegen will
// live in src/codegen/. Keep this file free of analysis logic.
//
// Build:
//   cmake -G Ninja -S . -B build -DCMAKE_PREFIX_PATH=<path to LLVM/Clang cmake config>
//   ninja -C build
// Run:
//   ./build/p05tool benchmarks/example.c --

#include "analysis/LoopAnalysis.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;

namespace {

class LoopAnalysisConsumer : public ASTConsumer {
public:
  void HandleTranslationUnit(ASTContext &Context) override {
    p05::LoopCollector Collector(Context);
    Collector.TraverseDecl(Context.getTranslationUnitDecl());

    const std::vector<p05::LoopInfo> &Loops = Collector.getLoops();
    llvm::outs() << "Analyzed " << Loops.size() << " loop(s).\n\n";
    for (const p05::LoopInfo &LI : Loops)
      p05::printLoopReport(llvm::outs(), LI);
  }
};

class LoopAnalysisAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef File) override {
    return std::make_unique<LoopAnalysisConsumer>();
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

  return Tool.run(newFrontendActionFactory<LoopAnalysisAction>().get());
}
