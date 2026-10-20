#include "analysis/Profitability.h"
#include "analysis/CrossTU.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>

using namespace clang;
using namespace llvm;

namespace p05 {

llvm::StringRef toString(OffloadTarget T) {
  switch (T) {
  case OffloadTarget::GpuOffload:
    return "GPU_OFFLOAD";
  case OffloadTarget::CpuParallel:
    return "CPU_PARALLEL";
  case OffloadTarget::Sequential:
    return "SEQUENTIAL";
  }
  return "SEQUENTIAL";
}

llvm::cl::OptionCategory &optionCategory() {
  // Shared by every p05tool flag, not just the MachineModel overrides this
  // file owns — main.cpp registers -rewrite/-o here too, and passes this
  // same category to CommonOptionsParser, so nothing the tool defines ends
  // up hidden from --help.
  static llvm::cl::OptionCategory Cat("p05tool options");
  return Cat;
}

namespace {

// --- MachineModel command-line overrides ------------------------------------
//
// One cl::opt per MachineModel field, named to match the field's meaning
// rather than its C++ identifier. Defined here (not main.cpp) for the same
// reason printSafetyReport lives in SafetyAnalysis.cpp: the driver stays
// wiring-only, and the options belong next to the model they configure.

cl::opt<double> FlagKernelLaunchUs(
    "kernel-launch-us",
    cl::desc("Fixed cost of launching a GPU kernel, in microseconds"),
    cl::init(10.0), cl::cat(optionCategory()));

cl::opt<double> FlagPcieBandwidthGBs(
    "pcie-bandwidth",
    cl::desc("Host<->device link bandwidth, in GB/s"),
    cl::init(12.0), cl::cat(optionCategory()));

cl::opt<double>
    FlagGpuThroughputGFs("gpu-throughput",
                         cl::desc("Device double-precision throughput, in GFLOP/s"),
                         cl::init(500.0), cl::cat(optionCategory()));

cl::opt<double> FlagGpuBandwidthGBs(
    "gpu-bandwidth", cl::desc("Device memory bandwidth, in GB/s"),
    cl::init(700.0), cl::cat(optionCategory()));

cl::opt<double>
    FlagCpuThroughputGFs("cpu-throughput",
                         cl::desc("Host double-precision throughput per core, in GFLOP/s"),
                         cl::init(4.0), cl::cat(optionCategory()));

cl::opt<unsigned> FlagCpuCores("cpu-cores", cl::desc("Host core count"),
                               cl::init(8), cl::cat(optionCategory()));

cl::opt<double>
    FlagDramBandwidthGBs("dram-bandwidth",
                         cl::desc("Aggregate host memory bandwidth, in GB/s"),
                         cl::init(20.0), cl::cat(optionCategory()));

cl::opt<double> FlagSingleCoreDramShare(
    "single-core-dram-share",
    cl::desc("Fraction of dram-bandwidth one core alone can reach"),
    cl::init(0.25), cl::cat(optionCategory()));

cl::opt<double>
    FlagThreadStartUs("thread-start-us",
                      cl::desc("Cost of starting an OpenMP thread team, in microseconds"),
                      cl::init(5.0), cl::cat(optionCategory()));

// Phase 2.3: measured machine parameters (eval/calibrate/).
cl::opt<std::string> FlagMachine(
    "machine",
    cl::desc("JSON file of measured machine parameters (eval/calibrate/); "
             "explicit machine flags override it"),
    cl::value_desc("file"), cl::cat(optionCategory()));

// Day 21: which decision procedure ProfitabilityAnalyzer runs. See the
// Policy enum's comment in Profitability.h for what Naive means. Defaults to
// Gated so every run without -policy is unchanged from before this flag
// existed.
cl::opt<Policy> FlagPolicy(
    "policy",
    cl::desc("Which loops get offloaded: the cost-model-gated policy (default) "
             "or the naive 'offload everything safe' strawman"),
    cl::values(clEnumValN(Policy::Gated, "gated",
                          "Cost-model argmin over seq/cpu/gpu (default)"),
               clEnumValN(Policy::Naive, "naive",
                          "Offload every SAFE loop unconditionally, guards "
                          "dropped, cost model computed but not consulted"),
               clEnumValN(Policy::AllCpu, "all-cpu",
                          "Testing: CPU parallel for on every SAFE loop, "
                          "unconditionally")),
    cl::init(Policy::Gated), cl::cat(optionCategory()));

// --- Unit helpers ------------------------------------------------------------
//
// A bandwidth/throughput given in G(something)/s converts to (something) per
// microsecond by dividing by 1000 (1 G.../s = 1e9 .../s = 1000 .../us). Every
// cost term below is built from these two conversions so the model has
// exactly one place that could get units wrong.

double usPerByte(double BandwidthGBs) {
  return BandwidthGBs > 0.0 ? 1.0 / (BandwidthGBs * 1000.0)
                            : std::numeric_limits<double>::infinity();
}

double usPerFlop(double ThroughputGFs) {
  return ThroughputGFs > 0.0 ? 1.0 / (ThroughputGFs * 1000.0)
                             : std::numeric_limits<double>::infinity();
}

/// Smallest power of two >= X, capped so a pathological crossover solution
/// cannot spin this loop for a long time.
int64_t nextPow2(double X) {
  if (X <= 1.0)
    return 1;
  int64_t V = 1;
  while (static_cast<double>(V) < X && V < (int64_t(1) << 52))
    V <<= 1;
  return V;
}

std::string fmtUs(double Us) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << llvm::format("%.3f", Us) << " us";
  return OS.str();
}

std::string fmtNum(double X) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  // Guard the integer cast with isfinite() and a magnitude bound *before*
  // ever performing it: casting an out-of-range or non-finite double to
  // int64_t is undefined behaviour, not a saturating truncation, so the
  // check has to happen without touching the cast first. Reachable in
  // practice whenever a SAFE loop touches no array at all (Intensity then
  // divides by zero bytes) — e.g. a loop whose whole body is scalar
  // arithmetic into a body-local temporary.
  if (!std::isfinite(X))
    OS << (X > 0 ? "inf" : (X < 0 ? "-inf" : "nan"));
  else if (std::abs(X) < 1e15 && X == std::floor(X))
    OS << static_cast<int64_t>(X);
  else
    OS << llvm::format("%.3f", X);
  return OS.str();
}

// --- Half 1: interprocedural — static arithmetic op count of one function's
// own body. ------------------------------------------------------------

class OpCountVisitor : public RecursiveASTVisitor<OpCountVisitor> {
public:
  explicit OpCountVisitor(double &Count) : Count(Count) {}

  bool VisitBinaryOperator(BinaryOperator *BO) {
    switch (BO->getOpcode()) {
    case BO_Add:
    case BO_Sub:
    case BO_Mul:
    case BO_Div:
    case BO_Rem:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign:
      Count += 1.0;
      break;
    default:
      break;
    }
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->getOpcode() == UO_Minus || UO->isIncrementDecrementOp())
      Count += 1.0;
    return true;
  }

private:
  double &Count;
};

// --- Half 2: intraprocedural — which arrays the loop body touches, and in
// which direction. -------------------------------------------------------
//
// Only meaningful for loops the safety pass has already verified SAFE. Since
// Phase 1.3 that no longer implies every access is `base[i]` (the dependence
// tests accept `b[i + 1]`, `m[i][j]`, ...), so the regions recorded here are
// exact only when LoopSafety::GpuMappable holds; analyzeLoop declines GPU
// offload otherwise. For the cost model they remain a per-iteration byte
// estimate (which still ignores inner loops' trip counts — Phase 2).

class RegionVisitor : public RecursiveASTVisitor<RegionVisitor> {
public:
  RegionVisitor(ASTContext &Ctx, std::vector<ArrayRegion> &Regions)
      : Ctx(Ctx), Regions(Regions) {}

  // Pre-order: this fires before the visitor descends into BO's own
  // children, so a plain assignment's LHS subscript is recorded as a pure
  // write (and marked in PureWriteTargets) before VisitArraySubscriptExpr
  // sees the same node moments later.
  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!BO->isAssignmentOp())
      return true;
    const bool WholeElement =
        isa<ArraySubscriptExpr>(BO->getLHS()->IgnoreParenCasts());
    if (auto *ASE = elementOf(BO->getLHS())) {
      // A compound assignment (`a[i] += x`) reads the old value as well as
      // writing the new one; a plain `=` only writes — unless it writes only
      // a member (`a[i].x = v`), which leaves the rest of the element as it
      // was, so the element is read (copied in) too.
      const bool Reads = BO->isCompoundAssignmentOp() || !WholeElement;
      markAccess(ASE, /*Write=*/true, /*Read=*/Reads);
      if (!Reads)
        PureWriteTargets.insert(ASE);
    }
    return true;
  }

  // `a[i]++` / `--a[i]`: read and written.
  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      if (auto *ASE = elementOf(UO->getSubExpr()))
        markAccess(ASE, /*Write=*/true, /*Read=*/true);
    return true;
  }

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    if (PureWriteTargets.count(ASE))
      return true; // already recorded as write-only above
    markAccess(ASE, /*Write=*/false, /*Read=*/true);
    return true;
  }

  /// The array element an lvalue writes into: `a[i]`, or `a[i].f` (a member
  /// store writes part of the element, so the element must be copied both
  /// ways).
  static ArraySubscriptExpr *elementOf(Expr *E) {
    E = E->IgnoreParenCasts();
    while (auto *ME = dyn_cast<MemberExpr>(E)) {
      if (ME->isArrow())
        return nullptr;
      E = ME->getBase()->IgnoreParenCasts();
    }
    return dyn_cast<ArraySubscriptExpr>(E);
  }

private:
  void markAccess(ArraySubscriptExpr *ASE, bool Write, bool Read) {
    const auto *DRE =
        dyn_cast<DeclRefExpr>(ASE->getBase()->IgnoreParenImpCasts());
    // Phase 2.5: a field path (`s->r[i]`) is a base of its own.
    const ValueDecl *Base =
        DRE ? cast<ValueDecl>(DRE->getDecl()->getCanonicalDecl())
            : knownFieldPath(ASE->getBase()->IgnoreParenImpCasts());
    if (!Base)
      return;
    ArrayRegion &R = regionFor(Base);
    R.Read = R.Read || Read;
    R.Written = R.Written || Write;
  }

  ArrayRegion &regionFor(const ValueDecl *Base) {
    for (ArrayRegion &R : Regions)
      if (R.Base == Base)
        return R;
    Regions.push_back(ArrayRegion{});
    ArrayRegion &R = Regions.back();
    R.Base = Base;
    R.ElemBytes = elemBytesOf(Base);
    return R;
  }

  uint64_t elemBytesOf(const ValueDecl *Base) const {
    QualType T = Base->getType();
    if (T->isPointerType())
      return Ctx.getTypeSizeInChars(T->getPointeeType()).getQuantity();
    if (const clang::ArrayType *AT = Ctx.getAsArrayType(T))
      return Ctx.getTypeSizeInChars(AT->getElementType()).getQuantity();
    return Ctx.getTypeSizeInChars(T).getQuantity();
  }

  ASTContext &Ctx;
  std::vector<ArrayRegion> &Regions;
  llvm::DenseSet<const ArraySubscriptExpr *> PureWriteTargets;
};

} // namespace

namespace {

/// One machine-file field: where it goes, and the flag that overrides it.
struct Field {
  const char *Key;
  double MachineModel::*Dbl = nullptr;
  unsigned MachineModel::*Uns = nullptr;
  cl::Option *Flag = nullptr;
};

std::vector<Field> cpuFields() {
  return {{"throughput_gflops", &MachineModel::CpuThroughputGFs, nullptr,
           &FlagCpuThroughputGFs},
          {"cores", nullptr, &MachineModel::CpuCores, &FlagCpuCores},
          {"dram_bandwidth_gbs", &MachineModel::DramBandwidthGBs, nullptr,
           &FlagDramBandwidthGBs},
          {"single_core_dram_share", &MachineModel::SingleCoreDramShare,
           nullptr, &FlagSingleCoreDramShare},
          {"thread_start_us", &MachineModel::ThreadStartUs, nullptr,
           &FlagThreadStartUs},
          {"runtime_init_us", &MachineModel::CpuInitUs, nullptr, nullptr}};
}

std::vector<Field> gpuFields() {
  return {{"kernel_launch_us", &MachineModel::KernelLaunchUs, nullptr,
           &FlagKernelLaunchUs},
          {"pcie_bandwidth_gbs", &MachineModel::PcieBandwidthGBs, nullptr,
           &FlagPcieBandwidthGBs},
          {"throughput_gflops", &MachineModel::GpuThroughputGFs, nullptr,
           &FlagGpuThroughputGFs},
          {"bandwidth_gbs", &MachineModel::GpuBandwidthGBs, nullptr,
           &FlagGpuBandwidthGBs},
          {"init_us", &MachineModel::GpuInitUs, nullptr, nullptr}};
}

llvm::Error machineError(const llvm::Twine &Msg) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "-machine=" + FlagMachine + ": " + Msg);
}

/// Every field of one section, each a non-negative number (cores a
/// positive integer); no other keys.
llvm::Error readSection(const json::Object &O, const char *Name,
                        const std::vector<Field> &Fields, MachineModel &M) {
  for (const auto &KV : O)
    if (llvm::none_of(Fields, [&](const Field &F) { return KV.first == F.Key; }))
      return machineError("unknown key \"" + KV.first.str() + "\" in \"" +
                          Name + "\"");
  for (const Field &F : Fields) {
    std::optional<double> V = O.getNumber(F.Key);
    if (!V)
      return machineError("\"" + std::string(Name) + "\" needs a number \"" +
                          F.Key + "\"");
    if (!std::isfinite(*V) || *V < 0.0)
      return machineError("\"" + std::string(Name) + "." + F.Key +
                          "\" must be a non-negative number");
    // A rate of 0 would price everything on that side as infinitely slow.
    const llvm::StringRef Key(F.Key);
    if ((Key.ends_with("_gflops") || Key.ends_with("_gbs")) && *V <= 0.0)
      return machineError("\"" + std::string(Name) + "." + F.Key +
                          "\" must be positive");
    if (Key == "single_core_dram_share" && (*V <= 0.0 || *V > 1.0))
      return machineError("\"" + std::string(Name) + "." + F.Key +
                          "\" must be in (0, 1]");
    if (F.Uns) {
      if (*V < 1.0 || *V != std::floor(*V) || *V > 65536.0)
        return machineError("\"" + std::string(Name) + "." + F.Key +
                            "\" must be a positive integer");
      M.*F.Uns = static_cast<unsigned>(*V);
    } else {
      M.*F.Dbl = *V;
    }
  }
  return llvm::Error::success();
}

} // namespace

llvm::Expected<MachineModel> machineModelFromFlags() {
  MachineModel M;
  if (!FlagMachine.empty()) {
    auto Buf = llvm::MemoryBuffer::getFile(FlagMachine);
    if (!Buf)
      return machineError(Buf.getError().message());
    llvm::Expected<json::Value> J = json::parse((*Buf)->getBuffer());
    if (!J)
      return machineError(llvm::toString(J.takeError()));
    const json::Object *O = J->getAsObject();
    if (!O)
      return machineError("not a JSON object");
    // Metadata the calibration writes for people; not read.
    static const char *const Meta[] = {"name",     "measured", "host",
                                       "device",   "compiler", "notes",
                                       "raw"};
    for (const auto &KV : *O)
      if (KV.first != "cpu" && KV.first != "gpu" &&
          llvm::none_of(Meta, [&](const char *K) { return KV.first == K; }))
        return machineError("unknown key \"" + KV.first.str() + "\"");
    const json::Object *Cpu = O->getObject("cpu");
    if (!Cpu)
      return machineError("needs a \"cpu\" section");
    if (llvm::Error E = readSection(*Cpu, "cpu", cpuFields(), M))
      return std::move(E);
    if (const json::Value *G = O->get("gpu")) {
      const json::Object *Gpu = G->getAsObject();
      if (!Gpu)
        return machineError("\"gpu\" must be an object");
      if (llvm::Error E = readSection(*Gpu, "gpu", gpuFields(), M))
        return std::move(E);
    } else {
      M.HasGpu = false;
    }
    if (std::optional<llvm::StringRef> N = O->getString("name"))
      M.Name = N->str();
    M.Source = FlagMachine;
  }
  // A flag given on the command line beats the file.
  for (const std::vector<Field> &Fs : {cpuFields(), gpuFields()})
    for (const Field &F : Fs) {
      if (!F.Flag || !F.Flag->getNumOccurrences())
        continue;
      if (F.Uns)
        M.*F.Uns = static_cast<cl::opt<unsigned> *>(F.Flag)->getValue();
      else
        M.*F.Dbl = static_cast<cl::opt<double> *>(F.Flag)->getValue();
    }
  return M;
}

Policy policyFromFlags() { return FlagPolicy; }

/// Phase 2.5: a known library function (sqrt, ...) is priced as one
/// operation; compiler hints (__builtin_expect) and assert's failure path,
/// which never runs, as none.
static double knownFunctionOps(const FunctionDecl *FD) {
  const unsigned ID = isKnownFunction(FD) ? FD->getBuiltinID() : 0;
  const Builtin::Context &BI = FD->getASTContext().BuiltinInfo;
  return ID && (BI.isLibFunction(ID) || BI.isPredefinedLibFunction(ID)) ? 1.0
                                                                          : 0.0;
}

double ProfitabilityAnalyzer::getOpCount(const FunctionDecl *FD) {
  FD = FD->getCanonicalDecl();
  if (auto It = OpCache.find(FD); It != OpCache.end())
    return It->second;

  double Count = knownFunctionOps(FD);
  if (const FunctionDecl *Def = FD->getDefinition()) {
    OpCountVisitor Visitor(Count);
    Visitor.TraverseStmt(Def->getBody());
  }
  return OpCache.try_emplace(FD, Count).first->second;
}

double ProfitabilityAnalyzer::breakEvenIntensity() const {
  // Asymptotic (compute-bound-on-both-sides) crossover: the flop/byte ratio
  // at which GPU compute time equals aggregate-CPU compute time for the same
  // per-element flop count, purely as a function of the machine model — no
  // loop enters this calculation. If aggregate CPU compute is already at
  // least as fast as the GPU, offload can never pay on compute alone, no
  // matter how arithmetic-heavy the kernel is.
  double CpuAggGFs = Machine.CpuThroughputGFs * Machine.CpuCores;
  double Denom = (1.0 / CpuAggGFs) - (1.0 / Machine.GpuThroughputGFs);
  if (Denom <= 0.0)
    return std::numeric_limits<double>::infinity();
  return (1.0 / Machine.PcieBandwidthGBs) / Denom;
}

void ProfitabilityAnalyzer::setLoops(const std::vector<LoopInfo> &All) {
  for (const LoopInfo &L : All)
    LoopOf[L.Loop] = &L;
}

namespace {

/// Phase 2.1 op counter: OpCountVisitor's operations, a constant-trip loop's
/// body (with its condition and increment) times its trip count, and each
/// call's callee counted where it is called.
class ScaledOpCounter : public RecursiveASTVisitor<ScaledOpCounter> {
public:
  using FnOps = std::function<double(const FunctionDecl *)>;
  ScaledOpCounter(
      const llvm::DenseMap<const ForStmt *, const LoopInfo *> &LoopOf,
      FnOps Ops, double &Own, double &Calls)
      : LoopOf(LoopOf), Ops(std::move(Ops)), Own(Own), Calls(Calls) {}

  bool TraverseForStmt(ForStmt *FS) {
    auto It = LoopOf.find(FS);
    if (It == LoopOf.end() || !It->second->TripCount ||
        *It->second->TripCount <= 0)
      return RecursiveASTVisitor::TraverseForStmt(FS);
    const double Trip = static_cast<double>(*It->second->TripCount);
    TraverseStmt(FS->getInit());
    double O = 0.0, C = 0.0;
    ScaledOpCounter Inner(LoopOf, Ops, O, C);
    Inner.TraverseStmt(FS->getCond());
    Inner.TraverseStmt(FS->getInc());
    Inner.TraverseStmt(FS->getBody());
    Own += O * Trip;
    Calls += C * Trip;
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *BO) {
    double N = 0.0;
    OpCountVisitor(N).VisitBinaryOperator(BO);
    Own += N;
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *UO) {
    double N = 0.0;
    OpCountVisitor(N).VisitUnaryOperator(UO);
    Own += N;
    return true;
  }
  bool VisitCallExpr(CallExpr *CE) {
    if (const FunctionDecl *F = CE->getDirectCallee())
      Calls += Ops(F);
    return true;
  }

private:
  const llvm::DenseMap<const ForStmt *, const LoopInfo *> &LoopOf;
  FnOps Ops;
  double &Own, &Calls;
};

} // namespace

void ProfitabilityAnalyzer::countOps(const Stmt *S, double &Own,
                                     double &Calls) {
  ScaledOpCounter C(
      LoopOf, [this](const FunctionDecl *F) { return functionOps(F); }, Own,
      Calls);
  C.TraverseStmt(const_cast<Stmt *>(S));
}

double ProfitabilityAnalyzer::functionOps(const FunctionDecl *FD) {
  FD = FD->getCanonicalDecl();
  if (isKnownFunction(FD))
    return knownFunctionOps(FD);
  if (auto It = FnOps.find(FD); It != FnOps.end())
    return It->second;
  const FunctionDecl *Def = FD->getDefinition();
  // Recursion makes a loop UNKNOWN before it is priced; count it once.
  if (!Def || !Def->getBody() || !Counting.insert(FD).second)
    return 0.0;
  double Own = 0.0, Calls = 0.0;
  countOps(Def->getBody(), Own, Calls);
  Counting.erase(FD);
  return FnOps.try_emplace(FD, Own + Calls).first->second;
}

ProfitabilityVerdict
ProfitabilityAnalyzer::priceLoop(const LoopInfo &LI, const LoopSafety &Safety,
                                 const RegionPlan *Plan) {
  ProfitabilityVerdict V;

  // Profitability never overrides safety: an UNSAFE or UNKNOWN loop is not
  // "priced and found unprofitable", it was never priced at all, and the
  // report must say so rather than imply a judgement that wasn't made.
  if (Safety.Verdict != SafetyVerdict::Safe) {
    V.Target = OffloadTarget::Sequential;
    V.Evaluated = false;
    V.Reasons.push_back("not evaluated: safety verdict is " +
                        toString(Safety.Verdict).str() +
                        "; profitability requires SAFE");
    return V;
  }
  V.Evaluated = true;
  V.Stride = LI.Step ? std::abs(*LI.Step) : 1;
  V.TripCount = LI.TripCount;

  // --- Ops: body + every direct and transitive callee -----------------------
  if (Plan) {
    countOps(LI.Loop->getBody(), V.OpsInBody, V.OpsInCallees);
  } else {
  double BodyOps = 0.0;
  OpCountVisitor BodyVisitor(BodyOps);
  BodyVisitor.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
  V.OpsInBody = BodyOps;

  for (const FunctionDecl *Callee : LI.Callees) {
    if (!Callee->hasBody())
      continue; // would have been caught as UNKNOWN by the safety gate above
    V.OpsInCallees += getOpCount(Callee);
    CallChainSummary Summary = Resolver.getReachable(Callee);
    for (const ResolvedCallee &RC : Summary.Reachable)
      if (RC.HasVisibleBody)
        V.OpsInCallees += getOpCount(RC.Callee);
  }
  }
  double OpsPerIter = V.OpsInBody + V.OpsInCallees;

  // --- Data: which arrays, how big, which direction --------------------------
  // Phase 2.5: a plan that stopped at a field path has no regions to count.
  const bool FromPlan = Plan && !Plan->HasPath;
  if (FromPlan) {
    V.Regions = Plan->Regions;
    V.RangeMaps = true;
  } else {
    RegionVisitor RV(Ctx, V.Regions);
    RV.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
  }

  // Mapped-region extent, per element type in map(kind: base[0:extent]) —
  // always the loop's own bound text, never the trip count: the map always
  // starts at index 0 regardless of the loop's start value (a documented
  // limitation, not an oversight — see the header comment).
  //
  // The bound text alone is the element count only for `<`; `for (i = 0; i
  // <= N; i++)` touches N+1 elements, so a bare `N` would under-map by one
  // and leave the last iteration's access outside the mapped region on a
  // real device. Bumped by one for LE, on both the constant- and
  // parameter-bound paths, so codegen's map() clause always matches this
  // loop's actual index range.
  bool Inclusive = LI.CmpOp == BO_LE;
  std::string Extent = (LI.Kind == LoopKind::ConstantBound && LI.ConstBound)
                            ? std::to_string(*LI.ConstBound + (Inclusive ? 1 : 0))
                            : (Inclusive ? "(" + LI.BoundText + ") + 1"
                                        : LI.BoundText);
  if (!FromPlan)
    for (ArrayRegion &R : V.Regions)
      R.ExtentText = Extent;

  // Bytes per iteration: one unit per region and direction, or (Phase 2.1)
  // the section's units spread over a known trip count.
  double BytesPerIter = 0.0;
  for (const ArrayRegion &R : V.Regions) {
    double Units = 1.0;
    if (FromPlan && R.ConstUnits && LI.TripCount && *LI.TripCount > 0)
      // Not floored at one unit: an inner loop touches one element of its
      // row (one unit) per run, not per iteration.
      Units = static_cast<double>(*R.ConstUnits) /
              static_cast<double>(*LI.TripCount);
    BytesPerIter += ((R.Read ? R.ElemBytes : 0) + (R.Written ? R.ElemBytes : 0)) *
                    Units;
  }

  V.Costs.BreakEvenIntensity = breakEvenIntensity();
  V.Costs.Intensity = BytesPerIter > 0.0 ? OpsPerIter / BytesPerIter
                      : OpsPerIter > 0.0 ? std::numeric_limits<double>::infinity()
                                         : 0.0;

  V.Reasons.push_back("ops/iteration: " + fmtNum(OpsPerIter) + " (body " +
                       fmtNum(V.OpsInBody) + ", callees " +
                       fmtNum(V.OpsInCallees) + ")");
  {
    std::string RegionsText;
    for (size_t I = 0; I < V.Regions.size(); ++I) {
      if (I)
        RegionsText += ", ";
      const ArrayRegion &R = V.Regions[I];
      RegionsText += R.Base->getNameAsString() + " (map(" +
                     R.mapKind().str() + ": " + R.listItem() + "), " +
                     std::to_string(R.ElemBytes) + " B/elem)";
    }
    V.Reasons.push_back("data/iteration: " + fmtNum(BytesPerIter) +
                         " B across " + std::to_string(V.Regions.size()) +
                         " region(s)" +
                         (RegionsText.empty() ? "" : ": " + RegionsText));
  }
  V.Reasons.push_back(
      "intensity: " + fmtNum(V.Costs.Intensity) +
      " flop/byte  (this machine's break-even: " +
      (std::isinf(V.Costs.BreakEvenIntensity)
           ? std::string("unreachable — aggregate CPU already >= GPU compute")
           : fmtNum(V.Costs.BreakEvenIntensity)) +
      " flop/byte)");

  double CpuAggGFs = Machine.CpuThroughputGFs * Machine.CpuCores;
  // Phase 2.3: startup costs charged to this loop's runs, if any.
  StartupCharge Charge;
  if (auto It = Startup.find(LI.Loop); It != Startup.end())
    Charge = It->second;
  const double ThreadFixedUs = Machine.ThreadStartUs + Charge.CpuUs;
  const double LaunchFixedUs = Machine.KernelLaunchUs + Charge.GpuUs;
  if (!Charge.Note.empty())
    V.Reasons.push_back(Charge.Note);
  if (!Machine.HasGpu)
    V.Reasons.push_back("no GPU on this machine (" + Machine.Name +
                        "): offload is not a choice");

  // A loop can lack a known TripCount for two different reasons (LoopInfo.h:
  // Kind and TripCount are independent facts), and only one of them leaves
  // anything safe to write into a runtime guard. VariableBound means the
  // *bound* itself is symbolic and LI.BoundText holds its printed source
  // text ("n") — that text is exactly the loop's trip count, since Start is
  // required to be known for this Kind. ConstantBound-with-no-TripCount
  // means the opposite: the bound folded to a compile-time constant, but
  // Start didn't (e.g. `for (int i = start; i < 8192; i++)` with `start` a
  // parameter) — LoopAnalysis.cpp never populates BoundText for this Kind
  // (there is nothing symbolic about the bound to print), so it reads as an
  // empty string here, not as "8192". Building `LI.BoundText + " >= " + T`
  // in that case does not fail to fold — it silently emits an if() clause
  // testing the empty left operand against T, which Clang correctly rejects
  // at compile time. Caught by an adversarial review of the Days 17-18
  // commit that first added a *second* path reachable through this same
  // defect (CpuGuardExpr, and the descending-loop CPU degrade, both built
  // on this same unchecked BoundText).
  //
  // There is no printed expression for "trip count" in the second case —
  // the actual count is `constant_bound - start`, and `start`'s source text
  // was never captured (LoopInfo only stores its folded *value*, when it
  // has one). Rather than reconstruct that expression textually (a real
  // fix, but a bigger one, deferred — see NOTES.md), this loop is priced no
  // further and declined here, the same way SafetyAnalyzer returns Unknown
  // rather than guess when it "cannot see this far": the ops/byte facts
  // already computed above stay in the report (they don't depend on Start),
  // but no Target needing a runtime guard is chosen, and neither GuardExpr
  // nor CpuGuardExpr is ever set from this path.
  if (!V.TripCount && LI.Kind != LoopKind::VariableBound) {
    V.Target = OffloadTarget::Sequential;
    V.Reasons.push_back(
        "-> SEQUENTIAL: trip count not statically known, and this loop's "
        "bound is a compile-time constant while its start value is not "
        "(e.g. a parameter) — there is no printable trip-count expression "
        "to guard a pragma on, so this loop is declined rather than one "
        "being fabricated");
    return V;
  }

  if (V.TripCount) {
    // --- Concrete trip count: evaluate all three targets numerically -------
    int64_t N = *V.TripCount;
    // Span covers the mapped region [0, bound); equals the trip count when
    // stride is 1, wider than it otherwise — this is where stride overhead
    // is priced, with no separate gate needed (see header comment).
    int64_t Span = (LI.Kind == LoopKind::ConstantBound && LI.ConstBound)
                       ? *LI.ConstBound
                       : N;

    V.Costs.Flops = OpsPerIter * static_cast<double>(N);
    V.Costs.TouchedBytes = BytesPerIter * static_cast<double>(N);
    V.Costs.TransferBytes = BytesPerIter * static_cast<double>(Span);
    // Phase 2.1: the sections' own sizes, when constant (a descending loop's
    // bound is not its span, so this replaces Span there too).
    if (FromPlan) {
      bool AllKnown = true;
      double Bytes = 0.0;
      for (const ArrayRegion &R : V.Regions) {
        if (!R.ConstUnits) {
          AllKnown = false;
          break;
        }
        const llvm::StringRef K = R.mapKind();
        Bytes += static_cast<double>(*R.ConstUnits) * R.ElemBytes *
                 (K == "tofrom" ? 2 : 1);
      }
      V.Costs.TransferBytes =
          AllKnown ? Bytes : BytesPerIter * static_cast<double>(N);
    }

    V.Costs.SeqUs =
        std::max(V.Costs.Flops * usPerFlop(Machine.CpuThroughputGFs),
                 V.Costs.TouchedBytes *
                     usPerByte(Machine.DramBandwidthGBs *
                               Machine.SingleCoreDramShare));

    double CpuCompute =
        std::max(V.Costs.Flops * usPerFlop(CpuAggGFs),
                 V.Costs.TouchedBytes * usPerByte(Machine.DramBandwidthGBs));
    V.Costs.CpuUs = CpuCompute + ThreadFixedUs;
    // Known unconditionally at this trip count: no CpuGuardExpr is needed
    // (or possible) — t_cpu vs t_seq was already decided numerically, not
    // symbolically, for this specific N.
    V.CpuBeatsSeq = V.Costs.CpuUs <= V.Costs.SeqUs;

    V.Costs.GpuLaunchUs = LaunchFixedUs;
    V.Costs.GpuTransferUs = V.Costs.TransferBytes * TransferScale *
                            usPerByte(Machine.PcieBandwidthGBs);
    V.Costs.GpuComputeUs =
        std::max(V.Costs.Flops * usPerFlop(Machine.GpuThroughputGFs),
                 V.Costs.TouchedBytes * usPerByte(Machine.GpuBandwidthGBs));
    V.Costs.GpuUs =
        V.Costs.GpuLaunchUs + V.Costs.GpuTransferUs + V.Costs.GpuComputeUs;
    if (!Machine.HasGpu)
      V.Costs.GpuUs = std::numeric_limits<double>::infinity();

    V.Reasons.push_back("trip count " + std::to_string(N) +
                         " (known): t_seq=" + fmtUs(V.Costs.SeqUs) +
                         ", t_cpu=" + fmtUs(V.Costs.CpuUs) +
                         ", t_gpu=" + fmtUs(V.Costs.GpuUs) + " (launch " +
                         fmtUs(V.Costs.GpuLaunchUs) + " + transfer " +
                         fmtUs(V.Costs.GpuTransferUs) + " + compute " +
                         fmtUs(V.Costs.GpuComputeUs) + ")");

    if (Machine.HasGpu && V.Costs.GpuUs <= V.Costs.CpuUs &&
        V.Costs.GpuUs <= V.Costs.SeqUs) {
      V.Target = OffloadTarget::GpuOffload;
      V.Reasons.push_back("-> GPU_OFFLOAD: cheapest of the three modelled times");
    } else if (V.Costs.CpuUs <= V.Costs.SeqUs) {
      V.Target = OffloadTarget::CpuParallel;
      std::string Why = !Machine.HasGpu ? std::string("threading beats sequential")
                        : V.Costs.GpuTransferUs > V.Costs.GpuComputeUs
                             ? "GPU is transfer-bound (" + fmtUs(V.Costs.GpuTransferUs) +
                                   " of " + fmtUs(V.Costs.GpuUs) + " is data movement)"
                             : "GPU's compute/launch cost doesn't beat threading on the host";
      V.Reasons.push_back("-> CPU_PARALLEL: " + Why);
    } else {
      V.Target = OffloadTarget::Sequential;
      V.Reasons.push_back(
          "-> SEQUENTIAL: trip count too small to recover thread-start overhead (" +
          fmtUs(ThreadFixedUs) + ") or kernel-launch overhead (" +
          fmtUs(LaunchFixedUs) + ")");
    }
  } else {
    // --- Symbolic trip count: compare per-element rates ------------------
    //
    // Flops(n) and bytes(n) are both proportional to n with a fixed ratio
    // (the intensity above), so whichever of the two candidate terms inside
    // each max() dominates is the same for every n > 0 — each side reduces
    // to one linear function of n, and the GPU-vs-CPU crossover has a closed
    // form rather than needing a numeric search.
    double GpuTransferRate =
        BytesPerIter * TransferScale * usPerByte(Machine.PcieBandwidthGBs);
    double GpuComputeRate =
        std::max(OpsPerIter * usPerFlop(Machine.GpuThroughputGFs),
                 BytesPerIter * usPerByte(Machine.GpuBandwidthGBs));
    double RateGpu = GpuTransferRate + GpuComputeRate;
    double RateCpu =
        std::max(OpsPerIter * usPerFlop(CpuAggGFs),
                 BytesPerIter * usPerByte(Machine.DramBandwidthGBs));
    // Single-core rate, for the same reason SeqUs uses SingleCoreDramShare
    // in the concrete-trip branch above: one core cannot saturate DRAM.
    double RateSeq =
        std::max(OpsPerIter * usPerFlop(Machine.CpuThroughputGFs),
                 BytesPerIter * usPerByte(Machine.DramBandwidthGBs *
                                          Machine.SingleCoreDramShare));

    V.Reasons.push_back(
        "trip count symbolic (bound: " + LI.BoundText +
        "); per-element rate gpu=" + fmtUs(RateGpu) + "/iter, cpu=" +
        fmtUs(RateCpu) + "/iter, seq=" + fmtUs(RateSeq) +
        "/iter, fixed overhead launch=" + fmtUs(LaunchFixedUs) +
        " vs thread-start=" + fmtUs(ThreadFixedUs));

    // --- CPU-vs-sequential crossover, independent of the GPU decision below.
    //
    // t_seq(n) = RateSeq * n (no fixed overhead: nothing to start up)
    // t_cpu(n) = RateCpu * n + ThreadStartUs
    // Solving RateSeq*n = RateCpu*n + ThreadStartUs gives the same kind of
    // closed-form crossover as the GPU guard below, just against a different
    // pair of rates. When RateSeq <= RateCpu, threading is cheaper per
    // element *and* pays no fixed cost advantage to offset — sequential
    // already loses at every n, so CpuBeatsSeq holds unconditionally in that
    // case too; it is only when RateSeq is strictly cheaper than RateCpu
    // that ThreadStartUs can ever dominate, which is the case guarded below.
    if (RateSeq > RateCpu) {
      double NStarCpu = ThreadFixedUs / (RateSeq - RateCpu);
      V.CpuBeatsSeq = true;
      if (NStarCpu > 0.0) {
        int64_t TCpu = nextPow2(NStarCpu);
        V.CpuGuardExpr = LI.BoundText + " >= " + std::to_string(TCpu);
      }
      V.Reasons.push_back(
          "cpu-vs-seq: threading" +
          (V.CpuGuardExpr ? (" pays off when (" + *V.CpuGuardExpr + ")")
                          : std::string(" pays off unconditionally")) +
          "; crossover n* = " + fmtNum(NStarCpu) +
          (V.CpuGuardExpr ? ", rounded up to a power of two" : ""));
    } else {
      // RateSeq <= RateCpu: aggregate threading is not even cheaper per
      // element than one core alone on this machine model, so no fixed-cost
      // amortization at any n can make it pay. Distinct from the RateGpu <
      // RateCpu case below, and checked independently — a machine model
      // where GPU also loses to CPU (e.g. --cpu-cores=1) must not silently
      // fall through to a CPU_PARALLEL verdict the numbers themselves
      // contradict; that was a real gap this pass previously had, and
      // dividing by (RateCpu - RateSeq) here would additionally be a
      // divide-by-zero when they are equal.
      V.CpuBeatsSeq = false;
      V.Reasons.push_back(
          "cpu-vs-seq: threading never beats sequential on this machine "
          "model (rate_seq <= rate_cpu)");
    }

    if (Machine.HasGpu && RateGpu < RateCpu) {
      double NStar = (LaunchFixedUs - ThreadFixedUs) / (RateCpu - RateGpu);
      if (NStar <= 0.0) {
        V.Target = OffloadTarget::GpuOffload;
        V.Reasons.push_back(
            "-> GPU_OFFLOAD unconditionally: GPU's per-element cost is lower "
            "and its fixed overhead is already no worse");
      } else {
        int64_t T = nextPow2(NStar);
        V.Target = OffloadTarget::GpuOffload;
        V.GuardExpr = LI.BoundText + " >= " + std::to_string(T);
        V.Reasons.push_back(
            "-> GPU_OFFLOAD if (" + *V.GuardExpr +
            "): crossover solved from launch_us + rate_gpu*n = "
            "thread_start_us + rate_cpu*n, n* = " +
            fmtNum(NStar) + ", rounded up to a power of two");
      }
    } else if (V.CpuBeatsSeq) {
      V.Target = OffloadTarget::CpuParallel;
      V.Reasons.push_back(
          "-> CPU_PARALLEL" +
          (V.CpuGuardExpr ? (" if (" + *V.CpuGuardExpr + ")")
                          : std::string(" unconditionally")) +
          (Machine.HasGpu
               ? ": GPU's per-element cost never falls below the host's, so "
                 "no n makes offload pay off, and threading beats sequential "
               : ": threading beats sequential ") +
          (V.CpuGuardExpr ? "above that guard" : "at every n"));
    } else {
      // Neither target's per-element rate beats the alternative that would
      // otherwise catch it: GPU loses to CPU threading, and CPU threading
      // itself never recovers its own fixed cost against running the loop
      // sequentially on this machine model. Previously this branch was
      // unreachable — CPU_PARALLEL was chosen unconditionally whenever GPU
      // lost, without ever checking CpuBeatsSeq — so a machine model where
      // aggregate CPU threading doesn't even beat one core (e.g.
      // --cpu-cores=1 --single-core-dram-share=1.0) produced a verdict the
      // model's own numbers contradicted.
      V.Target = OffloadTarget::Sequential;
      V.Reasons.push_back(
          "-> SEQUENTIAL: neither GPU offload nor host threading beats "
          "running this loop sequentially on this machine model");
    }
  }

  return V;
}

ProfitabilityVerdict
ProfitabilityAnalyzer::priceResident(const LoopInfo &LI,
                                     const LoopSafety &Safety,
                                     const RegionPlan &Plan, double Scale) {
  TransferScale = Scale;
  ProfitabilityVerdict V = priceLoop(LI, Safety, &Plan);
  TransferScale = 1.0;
  return V;
}

void ProfitabilityAnalyzer::setStartupCharge(const ForStmt *Loop,
                                             StartupCharge C) {
  Startup[Loop] = std::move(C);
}

void ProfitabilityAnalyzer::declineNested(const ForStmt *Loop,
                                          std::string Why) {
  Nested[Loop] = std::move(Why);
}

ProfitabilityVerdict
ProfitabilityAnalyzer::analyzeLoop(const LoopInfo &LI, const LoopSafety &Safety,
                                   const RegionPlan *Plan) {
  ProfitabilityVerdict V = priceLoop(LI, Safety, Plan);
  applyPolicy(V);
  V.ScalarClauses = Safety.ScalarClauses;
  V.AliasGuard = Safety.AliasGuard;
  if (Plan) {
    V.Collapse2 = Safety.Collapse2;
    V.MapGuard = Plan->MapGuard;
    for (const clang::VarDecl *S : Safety.HostScalars)
      V.ScalarMaps += " map(tofrom: " + S->getNameAsString() + ")";
    // Reported next to the sections it protects.
    if (!V.MapGuard.empty() && Safety.Verdict == SafetyVerdict::Safe) {
      auto It = llvm::find_if(V.Reasons, [](const std::string &R) {
        return llvm::StringRef(R).starts_with("data/iteration: ");
      });
      V.Reasons.insert(It == V.Reasons.end() ? It : It + 1,
                       "map guard: " + V.MapGuard);
    }
  }

  // Phase 1.6: a false if(target:) still runs teams/parallel on the host,
  // and overlapping arrays can't be mapped as separate regions, so a loop
  // whose pointers are only checked apart at run time stays on the host.
  if (V.Target == OffloadTarget::GpuOffload && !V.AliasGuard.empty()) {
    V.GuardExpr.reset();
    if (V.CpuBeatsSeq) {
      V.Target = OffloadTarget::CpuParallel;
      V.Reasons.push_back("GPU offload declined: pointer disjointness is only "
                          "checked at run time -> CPU_PARALLEL, guarded by "
                          "the check");
    } else {
      V.Target = OffloadTarget::Sequential;
      V.CpuGuardExpr.reset();
      V.Reasons.push_back("GPU offload declined: pointer disjointness is only "
                          "checked at run time, and threading does not beat "
                          "sequential -> SEQUENTIAL");
    }
  }

  // Phase 2.5: code imported from another input runs on the device only
  // with `declare target` there, in a file this tool does not rewrite.
  if (V.Target == OffloadTarget::GpuOffload && !Safety.ForeignCallee.empty()) {
    const std::string Why = "GPU offload declined: it calls " +
                            Safety.ForeignCallee +
                            ", defined in another file, which would need "
                            "`declare target` there";
    V.GuardExpr.reset();
    if (V.CpuBeatsSeq) {
      V.Target = OffloadTarget::CpuParallel;
      V.Reasons.push_back(Why + " -> CPU_PARALLEL");
    } else {
      V.Target = OffloadTarget::Sequential;
      V.CpuGuardExpr.reset();
      V.Reasons.push_back(Why + ", and threading does not beat sequential "
                                "-> SEQUENTIAL");
    }
  }

  // Interlock (Phase 1.3): every map() clause is `base[0:bound]`, which is
  // only the right region when each access is exactly `base[i]`. The
  // dependence tests now prove loops SAFE whose accesses are not
  // (`a[i] = b[i + 1]`, `m[i][j]`); offloading those would leave part of the
  // touched memory unmapped on the device. Decline GPU offload for them —
  // under either policy — until Phase 2.1 sizes regions from the affine
  // ranges. Host threading needs no map, so it stays available whenever the
  // cost model says threading beats sequential.
  // Phase 2.1 (Plan): every base needs a section RegionPlanner could prove,
  // and no callee may reach memory a map() can't name.
  const bool Mappable =
      Plan ? Plan->Mappable && Safety.CalleesMappable : Safety.GpuMappable;
  if (V.Target == OffloadTarget::GpuOffload && !Mappable && Plan) {
    std::string Why = Plan->Why.empty()
                          ? std::string("a callee reaches a global, or "
                                        "memory through a pointer that "
                                        "wasn't composed into the loop")
                          : Plan->Why.front();
    V.GuardExpr.reset();
    if (V.CpuBeatsSeq) {
      V.Target = OffloadTarget::CpuParallel;
      V.Reasons.push_back("GPU offload declined: no map() section: " + Why +
                          " -> CPU_PARALLEL, which needs no map");
    } else {
      V.Target = OffloadTarget::Sequential;
      V.CpuGuardExpr.reset();
      V.Reasons.push_back("GPU offload declined: no map() section: " + Why +
                          ", and threading does not beat sequential -> "
                          "SEQUENTIAL");
    }
  }
  if (V.Target == OffloadTarget::GpuOffload && !Mappable) {
    V.GuardExpr.reset();
    if (V.CpuBeatsSeq) {
      V.Target = OffloadTarget::CpuParallel;
      V.Reasons.push_back(
          "GPU offload declined: map() regions are only sized correctly when "
          "every access is exactly base[i] (range-based maps are Phase 2.1) "
          "-> CPU_PARALLEL, which needs no map");
    } else {
      V.Target = OffloadTarget::Sequential;
      V.CpuGuardExpr.reset();
      V.Reasons.push_back(
          "GPU offload declined: map() regions are only sized correctly when "
          "every access is exactly base[i] (range-based maps are Phase 2.1), "
          "and threading does not beat sequential -> SEQUENTIAL");
    }
  }
  if (auto It = Nested.find(LI.Loop);
      It != Nested.end() && V.Evaluated &&
      V.Target != OffloadTarget::Sequential) {
    V.Target = OffloadTarget::Sequential;
    V.GuardExpr.reset();
    V.CpuGuardExpr.reset();
    V.Reasons.push_back(It->second);
  }
  return V;
}

void ProfitabilityAnalyzer::applyPolicy(ProfitabilityVerdict &V) {
  if (PolicyMode == Policy::AllCpu) {
    if (!V.Evaluated) // never overrides the safety gate
      return;
    V.Reasons.push_back("all-cpu policy (testing): overriding " +
                        toString(V.Target).str() +
                        " -- CPU parallel for on every SAFE loop");
    V.Target = OffloadTarget::CpuParallel;
    V.GuardExpr.reset();
    V.CpuGuardExpr.reset();
    V.CpuBeatsSeq = true;
    return;
  }
  if (PolicyMode != Policy::Naive)
    return;

  // Naive never overrides the safety gate -- an unevaluated loop (UNSAFE,
  // UNKNOWN, or ConstantBound-with-unknown-start, see the "no printable
  // trip-count expression" branch above) stays unevaluated under every
  // policy. That gate is a correctness fact, not a profitability opinion.
  if (!V.Evaluated)
    return;

  // Every region needs a real map() extent to offload onto; a loop that
  // priceLoop declined before ever computing one (the ConstantBound path
  // with an unknown Start, see the "-> SEQUENTIAL: trip count not
  // statically known" branch) leaves V.Target == Sequential and V.Regions
  // populated with empty ExtentText -- nothing here to overwrite that with
  // a valid pragma, so leave it declined rather than emit map(kind: base[0:])
  // with a blank extent.
  for (const ArrayRegion &R : V.Regions) {
    if (R.ExtentText.empty() && R.SectionText.empty()) {
      V.Reasons.push_back(
          "naive policy: declined -- no printable map() extent for this "
          "loop (see the SEQUENTIAL reason above), same as the gated policy");
      return;
    }
  }

  if (V.Target == OffloadTarget::GpuOffload && !V.GuardExpr) {
    // Already unconditional GPU offload -- naive and gated agree here, and
    // there's nothing to override. Still worth a line: it says the
    // agreement was checked, not assumed.
    V.Reasons.push_back(
        "naive policy: agrees with the gated policy (unconditional "
        "GPU_OFFLOAD already)");
    return;
  }

  V.Reasons.push_back(
      "naive policy: overriding gated verdict " + toString(V.Target).str() +
      (V.GuardExpr ? (" if (" + *V.GuardExpr + ")") : std::string()) +
      " -- offloading unconditionally because this loop is SAFE, without "
      "consulting the cost model's decision");
  V.Target = OffloadTarget::GpuOffload;
  V.GuardExpr.reset();
  V.CpuGuardExpr.reset();
  // V.CpuBeatsSeq is deliberately left as priceLoop computed it: OmpRewriter
  // consults it independently (e.g. to decide whether a descending GPU loop
  // may degrade to a CPU pragma instead of being declined outright), and
  // forcing it true here would let this pass emit a pragma the cost model
  // itself contradicts -- exactly the invariant OmpRewriter.h documents and
  // enforces for the gated policy already.
}

const FunctionDecl *functionOf(ASTContext &Ctx, const Stmt *S) {
  DynTypedNodeList Ps = Ctx.getParents(*S);
  while (!Ps.empty()) {
    if (const auto *F = Ps[0].get<FunctionDecl>())
      return F->getCanonicalDecl();
    Ps = Ctx.getParents(Ps[0]);
  }
  return nullptr;
}

llvm::DenseSet<const FunctionDecl *> reachableFrom(CallResolver &Resolver,
                                                   const LoopInfo &Loop) {
  llvm::DenseSet<const FunctionDecl *> Out;
  for (const FunctionDecl *C : Loop.Callees) {
    Out.insert(C->getCanonicalDecl());
    for (const ResolvedCallee &RC : Resolver.getReachable(C).Reachable)
      Out.insert(RC.Callee->getCanonicalDecl());
  }
  return Out;
}

std::vector<int> constructKinds(const SourceManager &SM,
                                const std::vector<LoopInfo> &Loops,
                                const std::vector<ProfitabilityVerdict> &Vs) {
  std::vector<int> Out(Loops.size(), 0);
  for (size_t K = 0; K < Loops.size(); ++K) {
    const ProfitabilityVerdict &V = Vs[K];
    bool Under = false;
    for (int A = Loops[K].Parent; A >= 0; A = Loops[A].Parent)
      Under |= Out[A] != 0;
    if (Under || !V.Evaluated ||
        !SM.isWrittenInMainFile(Loops[K].Loop->getBeginLoc()))
      continue;
    if (V.Target == OffloadTarget::GpuOffload)
      Out[K] = 2;
    else if (V.Target == OffloadTarget::CpuParallel && V.CpuBeatsSeq)
      Out[K] = 1;
  }
  return Out;
}

void declineNestedConstructs(
    ProfitabilityAnalyzer &Profit, ASTContext &Ctx, CallResolver &Resolver,
    const std::vector<LoopInfo> &Loops, const std::vector<LoopSafety> &Safety,
    const std::vector<std::optional<RegionPlan>> &Plans,
    std::vector<ProfitabilityVerdict> &Verdicts) {
  const size_t N = Loops.size();
  std::vector<const FunctionDecl *> Fn(N);
  std::vector<llvm::DenseSet<const FunctionDecl *>> Reach(N);
  for (size_t K = 0; K < N; ++K) {
    Fn[K] = functionOf(Ctx, Loops[K].Loop);
    Reach[K] = reachableFrom(Resolver, Loops[K]);
  }
  std::vector<bool> Declined(N, false);
  for (bool Changed = true; Changed;) {
    Changed = false;
    const std::vector<int> Kind =
        constructKinds(Ctx.getSourceManager(), Loops, Verdicts);
    for (size_t K = 0; K < N; ++K) {
      if (!Kind[K] || Declined[K])
        continue;
      for (size_t J = 0; J < N; ++J) {
        // Phase 2.5: another input's loops are decided in its own run, so
        // any of them may get a target region there.
        if (!(Kind[J] == 2 || Loops[J].Foreign) || !Fn[J] ||
            !Reach[K].count(Fn[J]))
          continue;
        Declined[K] = Changed = true;
        Profit.declineNested(
            Loops[K].Loop,
            Loops[J].Foreign
                ? "declined: it calls " + foreignLabel(Fn[J]) +
                      ", defined in another file, whose loops may get a "
                      "target region there, and one can't run inside "
                      "another construct -> SEQUENTIAL"
                : "declined: it calls '" + Fn[J]->getNameAsString() +
                      "', which has a target region of its own, and one "
                      "can't run inside another construct -> SEQUENTIAL");
        Verdicts[K] = Profit.analyzeLoop(Loops[K], Safety[K],
                                         Plans[K] ? &*Plans[K] : nullptr);
        break;
      }
    }
  }
}

std::string chargeStartup(ProfitabilityAnalyzer &Profit,
                          const MachineModel &Machine, ASTContext &Ctx,
                          CallResolver &Resolver,
                          const std::vector<LoopInfo> &Loops,
                          const std::vector<LoopSafety> &Safety,
                          const std::vector<std::optional<RegionPlan>> &Plans,
                          std::vector<ProfitabilityVerdict> &Verdicts) {
  if (Machine.CpuInitUs <= 0.0 && Machine.GpuInitUs <= 0.0)
    return "";
  std::string Costs = "runtime " + fmtUs(Machine.CpuInitUs);
  if (Machine.HasGpu)
    Costs += ", device " + fmtUs(Machine.GpuInitUs);
  const FunctionDecl *Main = nullptr;
  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls())
    if (const auto *F = dyn_cast<FunctionDecl>(D))
      if (F->isMain() && F->doesThisDeclarationHaveABody())
        Main = F;
  if (!Main)
    return "startup costs (" + Costs + ") not priced: no main() in this "
           "file, so how many constructs share them can't be seen";

  const SourceManager &SM = Ctx.getSourceManager();
  const size_t N = Loops.size();
  // Runs per entry of the function: constant trips of the loops around.
  std::vector<double> Runs(N, 1.0);
  for (size_t K = 0; K < N; ++K)
    for (int A = Loops[K].Parent; A >= 0; A = Loops[A].Parent)
      if (Loops[A].TripCount && *Loops[A].TripCount > 0)
        Runs[K] *= static_cast<double>(*Loops[A].TripCount);

  // Only constructs main() can reach run, so only they share the costs.
  llvm::DenseSet<const FunctionDecl *> Live{Main->getCanonicalDecl()};
  for (const ResolvedCallee &RC : Resolver.getReachable(Main).Reachable)
    Live.insert(RC.Callee->getCanonicalDecl());
  std::vector<bool> Runs1(N);
  for (size_t K = 0; K < N; ++K)
    Runs1[K] = Live.count(functionOf(Ctx, Loops[K].Loop)) > 0;
  auto Kind = [&](const std::vector<ProfitabilityVerdict> &Vs) {
    std::vector<int> Out = constructKinds(SM, Loops, Vs);
    for (size_t K = 0; K < N; ++K)
      if (!Runs1[K])
        Out[K] = 0;
    return Out;
  };

  std::vector<int> Cur = Kind(Verdicts);
  for (size_t Round = 0; Round <= N + 1; ++Round) {
    unsigned NCpu = 0, NGpu = 0;
    for (int C : Cur) {
      NCpu += C == 1;
      NGpu += C == 2;
    }
    std::vector<ProfitabilityVerdict> Next;
    for (size_t K = 0; K < N; ++K) {
      // The loop itself counted in, whichever target it ends up on.
      const unsigned ShCpu = NCpu - (Cur[K] == 1) + 1;
      const unsigned ShGpu = NGpu - (Cur[K] == 2) + 1;
      ProfitabilityAnalyzer::StartupCharge C;
      C.CpuUs = Machine.CpuInitUs / ShCpu / Runs[K];
      C.GpuUs = Machine.HasGpu ? Machine.GpuInitUs / ShGpu / Runs[K] : 0.0;
      C.Note = "startup (main() in this file): runtime " +
               fmtUs(Machine.CpuInitUs) + " / " + std::to_string(ShCpu) +
               " construct(s)";
      if (Machine.HasGpu)
        C.Note += ", device " + fmtUs(Machine.GpuInitUs) + " / " +
                  std::to_string(ShGpu);
      C.Note += ", over " + fmtNum(Runs[K]) + " run(s): +" +
                fmtUs(C.CpuUs) + " on t_cpu" +
                (Machine.HasGpu ? ", +" + fmtUs(C.GpuUs) + " on t_gpu" : "");
      Profit.setStartupCharge(Loops[K].Loop, std::move(C));
      const RegionPlan *P = Plans[K] ? &*Plans[K] : nullptr;
      Next.push_back(Profit.analyzeLoop(Loops[K], Safety[K], P));
    }
    std::vector<int> NextKind = Kind(Next);
    Verdicts = std::move(Next);
    if (NextKind == Cur)
      break;
    Cur = std::move(NextKind);
  }
  return "startup costs (" + Costs + ") priced: main() is in this file";
}

void printProfitabilityReport(llvm::raw_ostream &OS,
                              const ProfitabilityVerdict &V) {
  OS << "  profitability : " << toString(V.Target);
  if (V.GuardExpr)
    OS << " if (" << *V.GuardExpr << ")";
  else if (V.Target == OffloadTarget::CpuParallel && V.CpuGuardExpr)
    OS << " if (" << *V.CpuGuardExpr << ")";
  OS << "\n";
  for (const std::string &Reason : V.Reasons)
    OS << "    - " << Reason << "\n";
  OS << "\n";
}

} // namespace p05
