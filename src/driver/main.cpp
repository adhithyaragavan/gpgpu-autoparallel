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
#include "codegen/OmpRewriter.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::opt<bool>
    RewriteFlag("rewrite",
               llvm::cl::desc("Emit a sibling <name>.omp.c with OpenMP "
                              "target-offload pragmas for GPU_OFFLOAD loops "
                              "and #pragma omp parallel for for "
                              "CPU_PARALLEL loops"),
               llvm::cl::init(false), llvm::cl::cat(p05::optionCategory()));

llvm::cl::opt<std::string> RewriteOutputPath(
    "o",
    llvm::cl::desc("Output path for -rewrite (default: <input>.omp.c)"),
    llvm::cl::init(""), llvm::cl::cat(p05::optionCategory()));

/// Derives the -rewrite output path: RewriteOutputPath if given, else the
/// input path with its extension replaced by ".omp.c".
std::string rewriteOutputPath(StringRef InputFile) {
  if (!RewriteOutputPath.empty())
    return RewriteOutputPath;
  llvm::SmallString<256> Path(InputFile);
  llvm::sys::path::replace_extension(Path, ".omp.c");
  return std::string(Path);
}

class LoopAnalysisConsumer : public ASTConsumer {
public:
  LoopAnalysisConsumer(p05::MachineModel Machine, std::string InputFile)
      : Machine(Machine), InputFile(std::move(InputFile)) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    p05::LoopCollector Collector(Context);
    Collector.TraverseDecl(Context.getTranslationUnitDecl());

    p05::CallResolver Resolver(Context);
    p05::SafetyAnalyzer Safety(Resolver);
    p05::ProfitabilityAnalyzer Profitability(Resolver, Context, Machine);

    Rewriter Rewrite(Context.getSourceManager(), Context.getLangOpts());
    p05::OmpRewriter OmpCodegen(Rewrite, Resolver);

    const std::vector<p05::LoopInfo> &Loops = Collector.getLoops();
    llvm::outs() << "Analyzed " << Loops.size() << " loop(s).\n\n";
    for (const p05::LoopInfo &LI : Loops) {
      p05::printLoopReport(llvm::outs(), LI);
      p05::printCallChains(llvm::outs(), LI, Resolver);
      p05::LoopSafety SafetyVerdict = Safety.analyzeLoop(LI);
      p05::printSafetyReport(llvm::outs(), SafetyVerdict);
      p05::ProfitabilityVerdict ProfitVerdict =
          Profitability.analyzeLoop(LI, SafetyVerdict);
      p05::printProfitabilityReport(llvm::outs(), ProfitVerdict);

      if (RewriteFlag)
        OmpCodegen.rewriteLoop(LI, ProfitVerdict);
    }

    if (!RewriteFlag)
      return;
    OmpCodegen.finalize();
    p05::printRewriteReport(llvm::outs(), OmpCodegen.summary());

    const llvm::RewriteBuffer *Buffer = Rewrite.getRewriteBufferFor(
        Context.getSourceManager().getMainFileID());
    if (!Buffer) {
      llvm::outs() << "rewrite: nothing annotated, no output file written\n";
      return;
    }
    std::string OutPath = rewriteOutputPath(InputFile);
    std::error_code EC;
    llvm::raw_fd_ostream Out(OutPath, EC, llvm::sys::fs::OF_None);
    if (EC) {
      llvm::errs() << "rewrite: failed to write " << OutPath << ": "
                  << EC.message() << "\n";
      return;
    }
    Buffer->write(Out);
    llvm::outs() << "rewrite: wrote " << OutPath << "\n";
  }

private:
  p05::MachineModel Machine;
  std::string InputFile;
};

class LoopAnalysisAction : public ASTFrontendAction {
public:
  explicit LoopAnalysisAction(p05::MachineModel Machine) : Machine(Machine) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef File) override {
    return std::make_unique<LoopAnalysisConsumer>(Machine, File.str());
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

int main(int argc, const char **argv) {
  // All of this tool's own flags — -rewrite, -o, and every MachineModel
  // override — are registered against p05::optionCategory(). Passing that
  // same category here, rather than a second tool-local one, is what keeps
  // them visible in --help at all: CommonOptionsParser hides every cl::opt
  // not in the category it's given, so a second category would silently
  // bury both today's new flags and Days 11-13's nine machine-model
  // overrides — the ones the profitability model's own header comment calls
  // "the honest mitigation" for its parameters not being measured.
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, p05::optionCategory());
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  std::vector<std::string> Sources = OptionsParser.getSourcePathList();

  if (!RewriteOutputPath.empty() && Sources.size() > 1) {
    llvm::errs() << "p05tool: -o names a single file and cannot be used "
                    "with more than one input; drop -o and let each file "
                    "get its own <name>.omp.c, or run one input at a time\n";
    return 1;
  }

  ClangTool Tool(OptionsParser.getCompilations(), Sources);

  LoopAnalysisActionFactory Factory(p05::machineModelFromFlags());
  return Tool.run(&Factory);
}
