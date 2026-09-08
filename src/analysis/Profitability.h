// Profitability — the Days 11-13 pass: for a loop the safety analysis has
// already cleared, decide *which target it should run on*, and say why in
// numbers rather than adjectives.
//
// This is the one place the project claims judgement rather than analysis.
// "Safe to parallelize" and "worth parallelizing" are different questions, and
// the brief's success criterion is explicitly the second one — a tool that
// offloads everything it is allowed to offload has not made a decision.
//
// ---------------------------------------------------------------------------
// The rule set
// ---------------------------------------------------------------------------
//
// Features, all derived from facts earlier passes already established plus one
// walk of the loop body:
//
//   trip count T  — LoopInfo::TripCount, or symbolic (LoopInfo::BoundText)
//   stride s      — LoopInfo::Step
//   data volume   — the ArrayRegions below: distinct array bases, whether each
//                   is read/written, element size from the AST type
//   ops/iteration — arithmetic in the loop body *plus* arithmetic in every
//                   function transitively reachable from its callees
//
// That last one is the genuinely interprocedural half, and it matters for
// exactly the same reason the safety pass's transitive walk did. In
// benchmarks/example.c the loop body is `data[i] = scale(data[i], factor)`:
// zero arithmetic operations are visible intraprocedurally. A cost model that
// stopped at the call boundary would price this loop at 0 flops and decline
// it for the wrong reason. Crossing the boundary is what makes the number
// real.
//
// The decision itself is a cost model, not a score. Each target pays the
// greater of its compute time and its memory time (roofline reasoning, kept
// deliberately coarse), and the cheapest wins:
//
//   t_seq = max(flops / CpuGFs,           touched / (DramBW * SingleCoreShare))
//   t_cpu = max(flops / (CpuGFs * Cores), touched / DramBW) + ThreadStartUs
//   t_gpu = KernelLaunchUs + transfer / PcieBW
//                          + max(flops / GpuGFs, touched / GpuBW)
//
// `transfer` (host<->device, over PCIe) is deliberately a separate quantity
// from `touched` (DRAM traffic). Conflating them is precisely what makes naive
// models wrong about this class of loop: SAXPY touches 24 bytes per iteration
// either way, but only the GPU has to ship them across a bus first.
//
// The reported gate cascade is a rendering of this computation, not a second
// mechanism layered over it. Every bound printed in a gate line is computed
// from MachineModel — there are no hand-set thresholds to defend separately
// from the machine parameters themselves.
//
// Why the single-core DRAM share exists: one core cannot saturate a modern
// memory system (it runs out of outstanding misses long before bandwidth).
// Modelling DRAM as one flat number regardless of core count would make every
// memory-bound loop look like it gains nothing from threading, which is both
// wrong and wrong in the unhelpful direction.
//
// ---------------------------------------------------------------------------
// Stride needs no gate of its own
// ---------------------------------------------------------------------------
//
// For step s, a loop touches T elements but *spans* T*s of them, and the
// mapped region has to cover the span. So `transfer` is charged on the span
// while useful work stays on T, and poor coalescing shows up as transfer cost
// the model already prices. This is worth stating explicitly because "what
// about coalescing?" is an obvious question, and "it is already paid for in
// the transfer term" is a much better answer than a fudge factor.
//
// Scattered access needs no gate either, for a different reason: it cannot
// reach this pass at all. SafetyAnalyzer requires every array access to be
// indexed by exactly the induction variable, so `data[idx[i]]` is already
// UNSAFE. The access-pattern feature the roadmap asks for is therefore mostly
// pre-filtered by the previous stage, and what is left of it is the stride.
//
// ---------------------------------------------------------------------------
// Known limitations (documented, not papered over — see NOTES.md)
// ---------------------------------------------------------------------------
//
//   - Operation counts are *static*: a loop or a branch inside a callee is
//     counted once, not trip-count times. The estimate is therefore a lower
//     bound for control-flow-heavy callees.
//   - All operations are weighted equally; a divide or a sqrt costs the same
//     as an add.
//   - A function called from several places in one body is counted once per
//     call site, but its own internal call sites are folded flat.
//   - Cache reuse across iterations is not modelled; `touched` assumes cold
//     traffic every iteration.
//   - Mapped regions always start at index 0 rather than at the loop's start
//     value, so a loop over [1000, 2000) maps 2000 elements, not 1000.
//   - MachineModel's defaults are order-of-magnitude estimates for a discrete
//     PCIe-attached GPU, not measurements of any particular machine. The
//     command-line overrides are the honest mitigation: the claim this pass
//     makes is "the decision follows from these stated parameters", never
//     "these parameters describe your hardware".

#ifndef P05_ANALYSIS_PROFITABILITY_H
#define P05_ANALYSIS_PROFITABILITY_H

#include "analysis/CallResolver.h"
#include "analysis/LoopInfo.h"
#include "analysis/SafetyAnalysis.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace p05 {

/// Where a loop should run. Three-valued rather than the binary
/// profitable/not-profitable the roadmap sketched, for the same reason
/// SafetyVerdict is tri-state: the third case is real and collapsing it loses
/// information. A loop of 8 iterations is not "safe but not GPU-profitable,
/// so thread it" — starting a thread team costs more than the loop does, and
/// saying CpuParallel there would be a decision the model itself contradicts.
enum class OffloadTarget {
  GpuOffload,
  CpuParallel,
  Sequential,
};

llvm::StringRef toString(OffloadTarget T);

/// Command-line category owning both the tool's own options and the machine
/// model overrides. It lives here rather than in main.cpp because the options
/// it owns do; the driver only needs it to hand to CommonOptionsParser, which
/// keeps main.cpp wiring-only in the same way printSafetyReport does.
llvm::cl::OptionCategory &optionCategory();

/// The hardware the cost model prices against. Every threshold this pass
/// reports is derived from these six numbers, so this struct is the entire
/// surface that has to be justified — and every field is overridable from the
/// command line, so "is this just tuned to your benchmarks?" is answered by
/// re-running with different hardware rather than by argument.
struct MachineModel {
  /// Fixed cost of getting a kernel onto the device at all, paid once per
  /// loop regardless of size. This is what makes small loops unprofitable no
  /// matter how favourable everything else looks.
  double KernelLaunchUs = 10.0;

  /// Host<->device link. PCIe 3.0 x16 delivers ~12 GB/s of the nominal 16;
  /// raise it to ~64 to model an NVLink-class attach.
  double PcieBandwidthGBs = 12.0;

  /// Device double-precision throughput.
  double GpuThroughputGFs = 500.0;

  /// Device memory bandwidth (HBM/GDDR), used for the device-side roofline.
  double GpuBandwidthGBs = 700.0;

  /// Host double-precision throughput, per core.
  double CpuThroughputGFs = 4.0;

  unsigned CpuCores = 8;

  /// Aggregate host memory bandwidth, saturated only by all cores together.
  double DramBandwidthGBs = 20.0;

  /// Fraction of DramBandwidthGBs a single core can actually reach. See the
  /// header comment: one core runs out of outstanding misses long before it
  /// runs out of bus.
  double SingleCoreDramShare = 0.25;

  /// Cost of spinning up an OpenMP thread team.
  double ThreadStartUs = 5.0;
};

/// Reads the command-line overrides into a MachineModel. Call after
/// CommonOptionsParser has parsed argv.
MachineModel machineModelFromFlags();

/// One array or pointer the loop touches, with everything codegen needs to
/// emit a map clause for it.
///
/// Built here rather than in Week 3's codegen because the profitability model
/// has to know the data volume anyway, and the volume and the map clause are
/// the same fact asked two ways: Read && Written is map(tofrom:), read-only is
/// map(to:), write-only is map(from:), and ExtentText is the array-section
/// length. Days 14-16 inherit their map clauses from this instead of walking
/// the body a second time to rediscover them.
struct ArrayRegion {
  const clang::ValueDecl *Base = nullptr;
  bool Read = false;
  bool Written = false;

  /// Size of one element, from the AST type (pointee for a pointer, element
  /// type for an array).
  uint64_t ElemBytes = 0;

  /// Length of the mapped section, as source text: "65536", "n". Always
  /// starting from index 0 — see the limitations note in the header.
  std::string ExtentText;

  /// The OpenMP map kind this region implies: "to", "from", or "tofrom".
  llvm::StringRef mapKind() const {
    if (Read && Written)
      return "tofrom";
    return Written ? "from" : "to";
  }
};

/// Modelled time on each target, in microseconds, plus the intermediate
/// quantities the report needs to explain where those times came from.
struct CostEstimate {
  double SeqUs = 0.0;
  double CpuUs = 0.0;
  double GpuUs = 0.0;

  // GPU breakdown, so the report can say *which* term dominated rather than
  // just quoting a total.
  double GpuLaunchUs = 0.0;
  double GpuTransferUs = 0.0;
  double GpuComputeUs = 0.0;

  double Flops = 0.0;
  double TransferBytes = 0.0;
  double TouchedBytes = 0.0;

  /// Flops per byte moved across the bus. The single number that decides
  /// most of these loops.
  double Intensity = 0.0;

  /// Intensity at which offload starts to pay, derived from the machine
  /// model alone (independent of this loop). Reported alongside Intensity so
  /// the comparison is visible rather than implied.
  double BreakEvenIntensity = 0.0;
};

struct ProfitabilityVerdict {
  OffloadTarget Target = OffloadTarget::Sequential;

  /// False when the loop never reached the cost model — an unsafe or
  /// unverifiable loop is not "modelled and found unprofitable", it is not
  /// modelled at all, and the report must not imply otherwise.
  bool Evaluated = false;

  /// Set when the target is GpuOffload but the trip count is only known at
  /// run time: the source-level condition under which offload wins, e.g.
  /// "n >= 16384". Week 3 renders this as OpenMP's own if() clause. The
  /// threshold is solved from the cost model's crossover point, never picked.
  std::optional<std::string> GuardExpr;

  /// Whether the model shows threading the host beating running the loop
  /// sequentially. Computed for every Evaluated loop regardless of which
  /// Target was ultimately chosen — codegen consults this on its own (e.g.
  /// to decide whether a GPU loop declined for another reason may still
  /// degrade to a CPU-threaded pragma), and must never emit `parallel for`
  /// when this is false: that would be a pragma the cost model itself
  /// contradicts.
  bool CpuBeatsSeq = false;

  /// The CPU-vs-sequential analogue of GuardExpr: set only when the trip
  /// count is symbolic and threading beats sequential above some threshold
  /// (CpuBeatsSeq is true but not unconditionally). Same closed form and
  /// power-of-two rounding as GuardExpr, solved from where thread-start
  /// overhead stops dominating. Empty when the trip count is statically
  /// known — no runtime test is needed then, since t_cpu vs t_seq was
  /// already decided numerically — or when CpuBeatsSeq holds for every n.
  std::optional<std::string> CpuGuardExpr;

  std::vector<ArrayRegion> Regions;

  /// Per-iteration operation count, split so the report can show how much of
  /// it was invisible without crossing a call boundary.
  double OpsInBody = 0.0;
  double OpsInCallees = 0.0;

  /// |LoopInfo::Step|.
  int64_t Stride = 1;

  /// Trip count used for the model, when statically known.
  std::optional<int64_t> TripCount;

  CostEstimate Costs;

  /// Plain-English findings, in the order they were reached.
  std::vector<std::string> Reasons;
};

/// Prices loops against a MachineModel. Memoizes per-function operation
/// counts, mirroring SafetyAnalyzer::getEffects — several loops can reach the
/// same helper, and the count is a fact about the function, not the loop.
class ProfitabilityAnalyzer {
public:
  ProfitabilityAnalyzer(CallResolver &Resolver, clang::ASTContext &Ctx,
                        MachineModel Machine)
      : Resolver(Resolver), Ctx(Ctx), Machine(Machine) {}

  /// Static arithmetic operation count of FD's own body, excluding anything
  /// it calls. Memoized.
  double getOpCount(const clang::FunctionDecl *FD);

  /// Full verdict for one loop. Safety is a gate, not an input to the model:
  /// anything other than SafetyVerdict::Safe returns Sequential with
  /// Evaluated == false, without pricing the loop at all.
  ProfitabilityVerdict analyzeLoop(const LoopInfo &LI, const LoopSafety &Safety);

  /// Intensity above which offload beats a fully threaded host, for this
  /// machine model, in the large-loop limit where launch cost vanishes.
  double breakEvenIntensity() const;

private:
  CallResolver &Resolver;
  clang::ASTContext &Ctx;
  MachineModel Machine;
  llvm::DenseMap<const clang::FunctionDecl *, double> OpCache;
};

/// Human-readable report: the gate cascade, then the decision. Lives here
/// rather than in the driver, matching printLoopReport / printCallChains /
/// printSafetyReport.
void printProfitabilityReport(llvm::raw_ostream &OS,
                              const ProfitabilityVerdict &V);

} // namespace p05

#endif // P05_ANALYSIS_PROFITABILITY_H
