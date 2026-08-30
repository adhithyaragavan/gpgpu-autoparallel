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

#include "analysis/CallResolver.h"
#include "analysis/LoopAnalysis.h"
#include "analysis/Profitability.h"
#include "analysis/SafetyAnalysis.h"

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
  explicit LoopAnalysisConsumer(p05::MachineModel Machine)
      : Machine(Machine) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    p05::LoopCollector Collector(Context);
    Collector.TraverseDecl(Context.getTranslationUnitDecl());

    p05::CallResolver Resolver(Context);
    p05::SafetyAnalyzer Safety(Resolver);
    p05::ProfitabilityAnalyzer Profitability(Resolver, Context, Machine);

    const std::vector<p05::LoopInfo> &Loops = Collector.getLoops();
    llvm::outs() << "Analyzed " << Loops.size() << " loop(s).\n\n";
    for (const p05::LoopInfo &LI : Loops) {
      p05::printLoopReport(llvm::outs(), LI);
      p05::printCallChains(llvm::outs(), LI, Resolver);
      p05::LoopSafety SafetyVerdict = Safety.analyzeLoop(LI);
      p05::printSafetyReport(llvm::outs(), SafetyVerdict);
      p05::printProfitabilityReport(
          llvm::outs(), Profitability.analyzeLoop(LI, SafetyVerdict));
    }
  }

private:
  p05::MachineModel Machine;
};

class LoopAnalysisAction : public ASTFrontendAction {
public:
  explicit LoopAnalysisAction(p05::MachineModel Machine) : Machine(Machine) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef File) override {
    return std::make_unique<LoopAnalysisConsumer>(Machine);
  }

private:
  p05::MachineModel Machine;
};

/// Factory binding the MachineModel (read from cl::opt flags once, after
/// CommonOptionsParser has parsed argv) into every LoopAnalysisAction the
/// tool creates — one per input file.
class LoopAnalysisActionFactory : public FrontendActionFactory {
public:
  explicit LoopAnalysisActionFactory(p05::MachineModel Machine)
      : Machine(Machine) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<LoopAnalysisAction>(Machine);
  }

private:
  p05::MachineModel Machine;
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

  LoopAnalysisActionFactory Factory(p05::machineModelFromFlags());
  return Tool.run(&Factory);
}
