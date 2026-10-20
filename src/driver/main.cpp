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

#include "analysis/AffineAccess.h"
#include "analysis/CallResolver.h"
#include "analysis/DataRegions.h"
#include "analysis/Hoisting.h"
#include "analysis/LoopAnalysis.h"
#include "analysis/Profitability.h"
#include "analysis/Rejection.h"
#include "analysis/SafetyAnalysis.h"
#include "analysis/ScalarAnalysis.h"
#include "analysis/Summaries.h"
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
  LoopAnalysisConsumer(p05::MachineModel Machine, p05::Policy PolicyMode,
                       std::string InputFile)
      : Machine(Machine), PolicyMode(PolicyMode),
        InputFile(std::move(InputFile)) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    p05::LoopCollector Collector(Context);
    Collector.TraverseDecl(Context.getTranslationUnitDecl());

    p05::CallResolver Resolver(Context);
    p05::SafetyAnalyzer Safety(Resolver);
    p05::AffineAnalyzer Affine(Context, Collector.getLoops());
    p05::ScalarAnalyzer Scalars(Context, p05::fpReductionsFromFlags());
    p05::SummaryAnalyzer Summaries(Affine);
    p05::AliasAnalyzer Alias(Context, p05::wholeProgramFromFlags());
    if (p05::aliasModeFromFlags() == p05::AliasMode::Check)
      Safety.setAliasAnalyzer(&Alias);
    const bool UseSummaries =
        p05::ipaModeFromFlags() == p05::IpaMode::Summaries;
    p05::ProfitabilityAnalyzer Profitability(Resolver, Context, Machine,
                                             PolicyMode);
    const bool RangeMaps = p05::mapModeFromFlags() == p05::MapMode::Ranges;
    p05::RegionPlanner Regions(Context, Collector.getLoops());
    if (p05::aliasModeFromFlags() == p05::AliasMode::Check)
      Regions.setAliasAnalyzer(&Alias);
    if (RangeMaps)
      Profitability.setLoops(Collector.getLoops());

    Rewriter Rewrite(Context.getSourceManager(), Context.getLangOpts());
    p05::OmpRewriter OmpCodegen(Rewrite, Resolver, Context);

    const std::vector<p05::LoopInfo> &Loops = Collector.getLoops();
    llvm::outs() << "Analyzed " << Loops.size() << " loop(s).\n";
    // Day 21: only printed for a non-default policy, so every -policy=gated
    // (and every pre-Day-21 invocation, which had no -policy at all) run's
    // report is byte-identical to yesterday's -- see NOTES.md's Day 21
    // entry and the regression sweep it's checked against.
    if (PolicyMode == p05::Policy::Naive)
      llvm::outs() << "policy: naive (offload every SAFE loop; cost model "
                      "computed but not consulted)\n";
    if (!Machine.Source.empty())
      llvm::outs() << "machine: " << Machine.Name << " (" << Machine.Source
                   << ")\n";
    llvm::outs() << "\n";
    // Every loop's accesses and safety first: whether a loop can collapse
    // with its child depends on the child's verdict (Phase 2.1).
    std::vector<p05::LoopAccesses> AllAccesses;
    std::vector<p05::LoopSafety> AllSafety;
    for (const p05::LoopInfo &LI : Loops) {
      p05::LoopAccesses Accesses = Affine.analyzeLoop(&LI - Loops.data());
      p05::CallEffects CallFx;
      if (UseSummaries)
        CallFx = Summaries.composeCalls(Accesses);
      if (p05::sizeBoundsFromFlags())
        Alias.assignExtents(Alias.enclosingFunction(LI.Loop), Accesses);
      AllSafety.push_back(Safety.analyzeLoop(LI, Accesses,
                                             Scalars.analyzeLoop(LI),
                                             UseSummaries ? &CallFx : nullptr));
      AllAccesses.push_back(std::move(Accesses));
    }
    if (RangeMaps)
      p05::markCollapsible(Loops, AllAccesses, AllSafety);

    // Then every loop's sections and verdict: hoisting (Phase 2.2) decides
    // across loops, and may change the verdicts of the loops it keeps data
    // on the device for.
    std::vector<std::optional<p05::RegionPlan>> AllPlans;
    std::vector<p05::ProfitabilityVerdict> AllVerdicts;
    for (const p05::LoopInfo &LI : Loops) {
      const size_t K = &LI - Loops.data();
      std::optional<p05::RegionPlan> Plan;
      if (RangeMaps)
        Plan = Regions.plan(LI, AllAccesses[K],
                            Alias.enclosingFunction(LI.Loop));
      AllVerdicts.push_back(Profitability.analyzeLoop(LI, AllSafety[K],
                                                      Plan ? &*Plan : nullptr));
      AllPlans.push_back(std::move(Plan));
    }
    // Phase 2.3: once-per-process startup costs, split across the
    // constructs that pay them (before hoisting, which prices with them).
    p05::declineNestedConstructs(Profitability, Context, Resolver, Loops,
                                 AllSafety, AllPlans, AllVerdicts);
    const std::string Startup =
        p05::chargeStartup(Profitability, Machine, Context, Resolver, Loops,
                           AllSafety, AllPlans, AllVerdicts);
    // Startup charges can move targets: check nesting again.
    p05::declineNestedConstructs(Profitability, Context, Resolver, Loops,
                                 AllSafety, AllPlans, AllVerdicts);
    std::vector<p05::DataRegion> Hoisted;
    if (RangeMaps && p05::hoistModeFromFlags() == p05::HoistMode::On) {
      p05::HoistPlanner Hoist(
          Context, Loops, Resolver, Safety,
          p05::aliasModeFromFlags() == p05::AliasMode::Check ? &Alias
                                                             : nullptr,
          Profitability, PolicyMode);
      Hoisted = Hoist.plan(AllSafety, AllPlans, AllVerdicts);
    }

    if (!Startup.empty())
      llvm::outs() << Startup << "\n\n";
    // Phase 2.5: -reasons appends one record per loop (JSON Lines), so
    // several inputs or runs can share a file.
    std::unique_ptr<llvm::raw_fd_ostream> Records;
    if (!p05::reasonsPathFromFlags().empty()) {
      std::error_code EC;
      Records = std::make_unique<llvm::raw_fd_ostream>(
          p05::reasonsPathFromFlags(), EC, llvm::sys::fs::OF_Append);
      if (EC) {
        llvm::errs() << "p05tool: cannot open "
                     << p05::reasonsPathFromFlags() << ": " << EC.message()
                     << "\n";
        Records.reset();
      }
    }
    for (const p05::LoopInfo &LI : Loops) {
      const size_t K = &LI - Loops.data();
      p05::printLoopReport(llvm::outs(), LI, Loops);
      p05::printCallChains(llvm::outs(), LI, Resolver);
      p05::printAccessReport(llvm::outs(), AllAccesses[K]);
      p05::printSafetyReport(llvm::outs(), AllSafety[K]);
      p05::printProfitabilityReport(llvm::outs(), AllVerdicts[K]);
      if (Records) {
        // The real file and line, not a #line directive's: hint edits are
        // offsets into the real file, and check_hints.py matches the two.
        const SourceManager &SM = Context.getSourceManager();
        const SourceLocation L = SM.getExpansionLoc(LI.Loop->getBeginLoc());
        const FunctionDecl *F = Alias.enclosingFunction(LI.Loop);
        p05::LoopRecord R;
        R.File = L.isValid() ? SM.getFilename(L).str() : InputFile;
        R.Line = L.isValid() ? SM.getExpansionLineNumber(L) : 0;
        R.Col = L.isValid() ? SM.getExpansionColumnNumber(L) : 0;
        R.Function = F ? F->getNameAsString() : "";
        R.Safety = p05::toString(AllSafety[K].Verdict).str();
        R.Target = p05::toString(AllVerdicts[K].Target).str();
        R.Reasons = &AllSafety[K].Reasons;
        p05::writeRecord(*Records, R);
      }

      if (RewriteFlag)
        OmpCodegen.rewriteLoop(LI, AllVerdicts[K]);
    }

    if (!RewriteFlag)
      return;
    OmpCodegen.emitRegions(Hoisted);
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
  p05::Policy PolicyMode;
  std::string InputFile;
};

class LoopAnalysisAction : public ASTFrontendAction {
public:
  LoopAnalysisAction(p05::MachineModel Machine, p05::Policy PolicyMode)
      : Machine(Machine), PolicyMode(PolicyMode) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef File) override {
    return std::make_unique<LoopAnalysisConsumer>(Machine, PolicyMode,
                                                   File.str());
  }

private:
  p05::MachineModel Machine;
  p05::Policy PolicyMode;
};

/// Factory binding the MachineModel (read from cl::opt flags once, after
/// CommonOptionsParser has parsed argv) into every LoopAnalysisAction the
/// tool creates — one per input file.
class LoopAnalysisActionFactory : public FrontendActionFactory {
public:
  LoopAnalysisActionFactory(p05::MachineModel Machine, p05::Policy PolicyMode)
      : Machine(Machine), PolicyMode(PolicyMode) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<LoopAnalysisAction>(Machine, PolicyMode);
  }

private:
  p05::MachineModel Machine;
  p05::Policy PolicyMode;
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

  llvm::Expected<p05::MachineModel> Machine = p05::machineModelFromFlags();
  if (!Machine) {
    llvm::errs() << "p05tool: " << llvm::toString(Machine.takeError()) << "\n";
    return 1;
  }
  LoopAnalysisActionFactory Factory(*Machine, p05::policyFromFlags());
  return Tool.run(&Factory);
}
