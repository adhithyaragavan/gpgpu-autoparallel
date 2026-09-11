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
                          "dropped, cost model computed but not consulted")),
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

Policy policyFromFlags() { return FlagPolicy; }

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
ProfitabilityAnalyzer::priceLoop(const LoopInfo &LI, const LoopSafety &Safety) {
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

    V.Costs.SeqUs =
        std::max(V.Costs.Flops * usPerFlop(Machine.CpuThroughputGFs),
                 V.Costs.TouchedBytes *
                     usPerByte(Machine.DramBandwidthGBs *
                               Machine.SingleCoreDramShare));

    double CpuCompute =
        std::max(V.Costs.Flops * usPerFlop(CpuAggGFs),
                 V.Costs.TouchedBytes * usPerByte(Machine.DramBandwidthGBs));
    V.Costs.CpuUs = CpuCompute + Machine.ThreadStartUs;
    // Known unconditionally at this trip count: no CpuGuardExpr is needed
    // (or possible) — t_cpu vs t_seq was already decided numerically, not
    // symbolically, for this specific N.
    V.CpuBeatsSeq = V.Costs.CpuUs <= V.Costs.SeqUs;

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
        "/iter, fixed overhead launch=" + fmtUs(Machine.KernelLaunchUs) +
        " vs thread-start=" + fmtUs(Machine.ThreadStartUs));

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
      double NStarCpu = Machine.ThreadStartUs / (RateSeq - RateCpu);
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
    } else if (V.CpuBeatsSeq) {
      V.Target = OffloadTarget::CpuParallel;
      V.Reasons.push_back(
          "-> CPU_PARALLEL" +
          (V.CpuGuardExpr ? (" if (" + *V.CpuGuardExpr + ")")
                          : std::string(" unconditionally")) +
          ": GPU's per-element cost never falls below the host's, so no n "
          "makes offload pay off, and threading beats sequential " +
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
ProfitabilityAnalyzer::analyzeLoop(const LoopInfo &LI, const LoopSafety &Safety) {
  ProfitabilityVerdict V = priceLoop(LI, Safety);
  applyPolicy(V);
  return V;
}

void ProfitabilityAnalyzer::applyPolicy(ProfitabilityVerdict &V) {
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
    if (R.ExtentText.empty()) {
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
