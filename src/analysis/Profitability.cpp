#include "analysis/Profitability.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Format.h"

#include <algorithm>
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
  static llvm::cl::OptionCategory Cat("p05tool profitability options");
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
// Only meaningful for loops the safety pass has already verified SAFE, which
// guarantees every array access is indexed by exactly the induction variable
// (SafetyAnalysis.h's body-dependence rule) — so this visitor does not need
// to re-check indexing, only record which base is touched and how.

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
    if (auto *ASE =
            dyn_cast<ArraySubscriptExpr>(BO->getLHS()->IgnoreParenCasts())) {
      // A compound assignment (`a[i] += x`) reads the old value as well as
      // writing the new one; a plain `=` only writes.
      markAccess(ASE, /*Write=*/true, /*Read=*/BO->isCompoundAssignmentOp());
      if (!BO->isCompoundAssignmentOp())
        PureWriteTargets.insert(ASE);
    }
    return true;
  }

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    if (PureWriteTargets.count(ASE))
      return true; // already recorded as write-only above
    markAccess(ASE, /*Write=*/false, /*Read=*/true);
    return true;
  }

private:
  void markAccess(ArraySubscriptExpr *ASE, bool Write, bool Read) {
    const auto *DRE =
        dyn_cast<DeclRefExpr>(ASE->getBase()->IgnoreParenImpCasts());
    if (!DRE)
      return;
    const auto *Base = cast<ValueDecl>(DRE->getDecl()->getCanonicalDecl());
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
    if (const ArrayType *AT = Ctx.getAsArrayType(T))
      return Ctx.getTypeSizeInChars(AT->getElementType()).getQuantity();
    return Ctx.getTypeSizeInChars(T).getQuantity();
  }

  ASTContext &Ctx;
  std::vector<ArrayRegion> &Regions;
  llvm::DenseSet<const ArraySubscriptExpr *> PureWriteTargets;
};

} // namespace

MachineModel machineModelFromFlags() {
  MachineModel M;
  M.KernelLaunchUs = FlagKernelLaunchUs;
  M.PcieBandwidthGBs = FlagPcieBandwidthGBs;
  M.GpuThroughputGFs = FlagGpuThroughputGFs;
  M.GpuBandwidthGBs = FlagGpuBandwidthGBs;
  M.CpuThroughputGFs = FlagCpuThroughputGFs;
  M.CpuCores = FlagCpuCores;
  M.DramBandwidthGBs = FlagDramBandwidthGBs;
  M.SingleCoreDramShare = FlagSingleCoreDramShare;
  M.ThreadStartUs = FlagThreadStartUs;
  return M;
}

double ProfitabilityAnalyzer::getOpCount(const FunctionDecl *FD) {
  FD = FD->getCanonicalDecl();
  if (auto It = OpCache.find(FD); It != OpCache.end())
    return It->second;

  double Count = 0.0;
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

ProfitabilityVerdict
ProfitabilityAnalyzer::analyzeLoop(const LoopInfo &LI, const LoopSafety &Safety) {
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
  double OpsPerIter = V.OpsInBody + V.OpsInCallees;

  // --- Data: which arrays, how big, which direction --------------------------
  RegionVisitor RV(Ctx, V.Regions);
  RV.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));

  // Mapped-region extent, per element type in map(kind: base[0:extent]) —
  // always the loop's own bound text, never the trip count: the map always
  // starts at index 0 regardless of the loop's start value (a documented
  // limitation, not an oversight — see the header comment).
  std::string Extent = (LI.Kind == LoopKind::ConstantBound && LI.ConstBound)
                            ? std::to_string(*LI.ConstBound)
                            : LI.BoundText;
  for (ArrayRegion &R : V.Regions)
    R.ExtentText = Extent;

  double BytesPerIter = 0.0;
  for (const ArrayRegion &R : V.Regions)
    BytesPerIter += (R.Read ? R.ElemBytes : 0) + (R.Written ? R.ElemBytes : 0);

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
                     R.mapKind().str() + ": " + R.Base->getNameAsString() +
                     "[0:" + R.ExtentText + "]), " +
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

    V.Costs.SeqUs =
        std::max(V.Costs.Flops * usPerFlop(Machine.CpuThroughputGFs),
                 V.Costs.TouchedBytes *
                     usPerByte(Machine.DramBandwidthGBs *
                               Machine.SingleCoreDramShare));

    double CpuCompute =
        std::max(V.Costs.Flops * usPerFlop(CpuAggGFs),
                 V.Costs.TouchedBytes * usPerByte(Machine.DramBandwidthGBs));
    V.Costs.CpuUs = CpuCompute + Machine.ThreadStartUs;

    V.Costs.GpuLaunchUs = Machine.KernelLaunchUs;
    V.Costs.GpuTransferUs =
        V.Costs.TransferBytes * usPerByte(Machine.PcieBandwidthGBs);
    V.Costs.GpuComputeUs =
        std::max(V.Costs.Flops * usPerFlop(Machine.GpuThroughputGFs),
                 V.Costs.TouchedBytes * usPerByte(Machine.GpuBandwidthGBs));
    V.Costs.GpuUs =
        V.Costs.GpuLaunchUs + V.Costs.GpuTransferUs + V.Costs.GpuComputeUs;

    V.Reasons.push_back("trip count " + std::to_string(N) +
                         " (known): t_seq=" + fmtUs(V.Costs.SeqUs) +
                         ", t_cpu=" + fmtUs(V.Costs.CpuUs) +
                         ", t_gpu=" + fmtUs(V.Costs.GpuUs) + " (launch " +
                         fmtUs(V.Costs.GpuLaunchUs) + " + transfer " +
                         fmtUs(V.Costs.GpuTransferUs) + " + compute " +
                         fmtUs(V.Costs.GpuComputeUs) + ")");

    if (V.Costs.GpuUs <= V.Costs.CpuUs && V.Costs.GpuUs <= V.Costs.SeqUs) {
      V.Target = OffloadTarget::GpuOffload;
      V.Reasons.push_back("-> GPU_OFFLOAD: cheapest of the three modelled times");
    } else if (V.Costs.CpuUs <= V.Costs.SeqUs) {
      V.Target = OffloadTarget::CpuParallel;
      std::string Why = V.Costs.GpuTransferUs > V.Costs.GpuComputeUs
                             ? "GPU is transfer-bound (" + fmtUs(V.Costs.GpuTransferUs) +
                                   " of " + fmtUs(V.Costs.GpuUs) + " is data movement)"
                             : "GPU's compute/launch cost doesn't beat threading on the host";
      V.Reasons.push_back("-> CPU_PARALLEL: " + Why);
    } else {
      V.Target = OffloadTarget::Sequential;
      V.Reasons.push_back(
          "-> SEQUENTIAL: trip count too small to recover thread-start overhead (" +
          fmtUs(Machine.ThreadStartUs) + ") or kernel-launch overhead (" +
          fmtUs(Machine.KernelLaunchUs) + ")");
    }
  } else {
    // --- Symbolic trip count: compare per-element rates ------------------
    //
    // Flops(n) and bytes(n) are both proportional to n with a fixed ratio
    // (the intensity above), so whichever of the two candidate terms inside
    // each max() dominates is the same for every n > 0 — each side reduces
    // to one linear function of n, and the GPU-vs-CPU crossover has a closed
    // form rather than needing a numeric search.
    double GpuTransferRate = BytesPerIter * usPerByte(Machine.PcieBandwidthGBs);
    double GpuComputeRate =
        std::max(OpsPerIter * usPerFlop(Machine.GpuThroughputGFs),
                 BytesPerIter * usPerByte(Machine.GpuBandwidthGBs));
    double RateGpu = GpuTransferRate + GpuComputeRate;
    double RateCpu =
        std::max(OpsPerIter * usPerFlop(CpuAggGFs),
                 BytesPerIter * usPerByte(Machine.DramBandwidthGBs));

    V.Reasons.push_back(
        "trip count symbolic (bound: " + LI.BoundText +
        "); per-element rate gpu=" + fmtUs(RateGpu) + "/iter, cpu=" +
        fmtUs(RateCpu) + "/iter, fixed overhead launch=" +
        fmtUs(Machine.KernelLaunchUs) +
        " vs thread-start=" + fmtUs(Machine.ThreadStartUs));

    if (RateGpu < RateCpu) {
      double NStar = (Machine.KernelLaunchUs - Machine.ThreadStartUs) /
                      (RateCpu - RateGpu);
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
    } else {
      V.Target = OffloadTarget::CpuParallel;
      V.Reasons.push_back(
          "-> CPU_PARALLEL: GPU's per-element cost never falls below the "
          "host's, so no n makes offload pay off; trip count is a runtime "
          "parameter and assumed not pathologically small");
    }
  }

  return V;
}

void printProfitabilityReport(llvm::raw_ostream &OS,
                              const ProfitabilityVerdict &V) {
  OS << "  profitability : " << toString(V.Target);
  if (V.GuardExpr)
    OS << " if (" << *V.GuardExpr << ")";
  OS << "\n";
  for (const std::string &Reason : V.Reasons)
    OS << "    - " << Reason << "\n";
  OS << "\n";
}

} // namespace p05
