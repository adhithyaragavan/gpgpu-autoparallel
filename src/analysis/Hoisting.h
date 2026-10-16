// Hoisting — Phase 2.2: keep arrays on the device across offloaded loops.
//
// Until now every GPU loop copied its sections in and out each time it ran:
// an offloaded loop inside a time-step loop paid the transfer every step,
// and two offloaded loops in a row copied the same array back and forth. A
// region R (an enclosing loop the tool leaves sequential, or a run of
// statements in one block) now gets one `#pragma omp target data` holding
// the hull of every section its GPU loops map; the loops keep their own
// map() clauses, which find the data present and copy nothing.
//
// One condition C guards everything: the region's `target data if(C)`, and
// `if(target: C)` on every GPU loop in it. C is the conjunction of the
// loops' map guards and their cost guards priced with their share of the
// region's transfer (ProfitabilityAnalyzer::priceResident, below). C names only variables R never
// changes, so it has one value for the whole region: when true every loop
// runs on the device with resident data; when false nothing is mapped and
// every loop runs on the host, on host data. No loop can run on the host
// while the device holds the current copy.
//
// R is hoisted only when (each failure is reported on the loops in it):
//   - every GPU loop runs on each entry: on the way to R only blocks and
//     loop bodies, and for pointer bases each for loop around adds its first
//     test to C (a while loop refuses); a hull's ends are owned memory only
//     because loops touch them;
//   - every name in the sections and C is unchanged in R: not assigned,
//     incremented or address-taken in the function, not a global whose
//     address is taken anywhere or a call in R may write, not volatile, and
//     (a local) declared before R in a block or for header around it; bases
//     included, so pointer swaps are refused;
//   - mapped bases are parameters or arrays, and every pair is proven apart
//     statically (`restrict` only for a pair with a write);
//   - every loop in R that touches mapped data and can be offloaded is (a
//     host loop on resident data would see stale values);
//   - host code in R that touches mapped data only subscripts a mapped base
//     directly or through a pointer variable read by name, sits in a { }
//     block and doesn't jump out; it gets `target update from(...)` before
//     and `to(...)` after, with if(C). Host code may not use other local or
//     global pointers, pointers loaded from memory, functions that offload
//     loops of their own or reach memory through pointers, or functions
//     without a body outside system headers; other pointer parameters must
//     be proven apart from the mapped bases. Conditions around the GPU loops
//     may not touch mapped data;
//   - R has no return or goto and no call that may not return (exit,
//     longjmp, noreturn); a run of statements has no break/continue leaving
//     it and declares only variables, none used after it (it gets { });
//   - the GPU loops run at least twice per entry. Written bases map tofrom,
//     never `from`.
//
// Then (Phase 2.3, not under -policy naive) whether the region pays. It
// copies, per entry, each base once in and (written) once out, plus each
// `target update` section each time it runs; its loops run so many times.
// Run counts multiply the constant trip counts of the loops between a
// statement and R; a symbolic one is a t, and a ratio of two such sums is
// taken at whichever of t = 1 and t -> infinity gives more. Sections are
// taken to be alike in size, so loop K is priced with its own transfer
// times (copies per kernel run) / (its own map sections). R is refused when
// its copies per entry exceed the loops' own maps', when a loop that chose
// the GPU on its own no longer does at its share, or when no loop chooses
// the GPU at its share. Candidates are found with no transfer at all (the
// optimistic bound), so no region is missed.
//
// The largest candidate is tried first: each function's top-level run of
// statements, then each enclosing loop, recursing inward on failure.
// -hoist=off keeps per-loop maps (ablation baseline).

#ifndef P05_ANALYSIS_HOISTING_H
#define P05_ANALYSIS_HOISTING_H

#include "analysis/AffineAccess.h"
#include "analysis/AliasAnalysis.h"
#include "analysis/CallResolver.h"
#include "analysis/DataRegions.h"
#include "analysis/LoopInfo.h"
#include "analysis/Profitability.h"
#include "analysis/SafetyAnalysis.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"

#include <optional>
#include <string>
#include <vector>

namespace p05 {

enum class HoistMode { On, Off };
HoistMode hoistModeFromFlags();

/// A `target update` around one host statement inside a region.
struct HostUpdate {
  const clang::Stmt *S = nullptr;
  std::string Before; ///< pragma line, or empty
  std::string After;  ///< pragma line, or empty
};

/// One hoisted region: statements First..Last (the same statement for an
/// enclosing loop), all children of one block when Braces.
struct DataRegion {
  const clang::Stmt *First = nullptr, *Last = nullptr;
  bool Braces = false;
  std::string Pragma; ///< "#pragma omp target data map(...) [if(C)]"
  /// Declarations of section ends the pragma uses, when a section has a
  /// `?:` (Clang 22 miscompiles one inside a `target data` section): the
  /// region is then wrapped in { } that holds them.
  std::string Decls;
  std::vector<HostUpdate> Updates;
  std::vector<int> Sites; ///< indices of its GPU loops
  unsigned Line = 0;
};

class HoistPlanner {
public:
  HoistPlanner(clang::ASTContext &Ctx, const std::vector<LoopInfo> &Loops,
               CallResolver &Resolver, SafetyAnalyzer &Safety,
               AliasAnalyzer *Alias, ProfitabilityAnalyzer &Profit,
               Policy PolicyMode);

  /// Finds the regions and rewrites the verdicts of their GPU loops
  /// (target, if(target: C), a report line); loops in a candidate that
  /// failed get a report line saying why.
  std::vector<DataRegion>
  plan(const std::vector<LoopSafety> &Safety,
       const std::vector<std::optional<RegionPlan>> &Plans,
       std::vector<ProfitabilityVerdict> &Verdicts);

private:
  struct Impl;
  clang::ASTContext &Ctx;
  const std::vector<LoopInfo> &Loops;
  CallResolver &Resolver;
  SafetyAnalyzer &SafetyA;
  AliasAnalyzer *Alias;
  ProfitabilityAnalyzer &Profit;
  Policy PolicyMode;
};

} // namespace p05

#endif // P05_ANALYSIS_HOISTING_H
