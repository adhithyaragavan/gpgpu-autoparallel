// DataRegions — Phase 2.1: map() sections from the subscripts' ranges.
//
// Until Phase 2.0 every GPU loop mapped `base[0:bound]`, right only when each
// access is exactly `base[i]` (Phase 1.3 kept every other loop off the GPU),
// and wrong for a descending loop (priced and mapped as [0:0]). A write-only
// array was mapped `from` even when the loop writes only some of its elements
// (`if (c[i]) y[i] = v`), which copies uninitialized device memory over the
// unwritten ones.
//
// Here each base the loop (or a callee, through Phase 1.5's composed
// accesses) touches gets the section its subscripts can reach: the smallest
// and largest index over the loop's and inner loops' ranges (DependenceTest's
// subscriptBounds), along the outermost dimension. Mapping copies every
// element of the section, so each must be memory the program owns:
//
//   Object   an array declared with constant extents (global, static or
//            local): the section is clamped to [0, extent).
//   Pointer  anything else (pointer parameters, pointers to rows): the ends
//            of the section must be elements the loop surely touches. Only
//            exact accesses define them: ones that run on every iteration
//            (no if, ?:, &&/||, switch or while on the way; no break,
//            continue, goto or non-final return in any loop or callee around
//            them) with every loop variable in their subscript stepping by
//            +-1 (IVRange::Exact). Any other access must lie, by a constant
//            offset, inside what the exact ones span. Both ends are reached
//            only if every loop involved runs at least once, and the lowest
//            index must not be negative: those conditions go into MapGuard,
//            which joins the GPU pragma's if(target:) (false runs the region
//            on the host, where nothing is copied).
//
// Shapes: a pointer subscripted once maps p[lo:len]; a delinearized pointer
// (`a[i*n + j]`) maps the linear range, all of its accesses exact; an array or
// a pointer to arrays (`double m[N][M]`, a parameter `double a[][M]`) maps
// whole rows, m[lo:len][0:M], relying on C's rule that each subscript stays
// inside its own dimension.
//
// Direction: `to` if only read, `from` only if never read and one exact
// write covers every element of the section (a single loop variable with
// coefficient +-1 in the outer subscript, each inner subscript running over
// its whole row), else `tofrom`.
//
// -maps=bound keeps the pre-2.1 regions and pricing (ablation baseline).

#ifndef P05_ANALYSIS_DATAREGIONS_H
#define P05_ANALYSIS_DATAREGIONS_H

#include "analysis/AffineAccess.h"
#include "analysis/LoopInfo.h"

#include "clang/AST/ASTContext.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace p05 {

enum class MapMode { Ranges, Bound };
MapMode mapModeFromFlags();

/// One array or pointer the loop touches, with everything codegen needs to
/// emit a map clause for it.
///
/// Built by the profitability pass's region walk (-maps=bound) or by
/// RegionPlanner (-maps=ranges): the model has to know the data volume
/// anyway, and the volume and the map clause are the same fact asked two
/// ways.
struct ArrayRegion {
  const clang::ValueDecl *Base = nullptr;
  bool Read = false;
  bool Written = false;

  /// Size of one unit of the mapped section's outermost dimension: an
  /// element for a 1-D or linearized section, a whole row for m[lo:len][0:M].
  uint64_t ElemBytes = 0;

  /// -maps=bound: length of the mapped section, as source text: "65536",
  /// "n". Always starting from index 0.
  std::string ExtentText;

  /// -maps=ranges: the whole list item, e.g. "a[1:n]", "m[0:n][0:512]".
  std::string SectionText;
  /// -maps=ranges: units mapped, when that is a constant.
  std::optional<int64_t> ConstUnits;
  /// -maps=ranges: `from` was proven (a write covers the whole section).
  bool WriteCovers = false;

  /// The OpenMP map kind this region implies: "to", "from", or "tofrom".
  llvm::StringRef mapKind() const {
    if (!SectionText.empty())
      return !Written ? "to" : (!Read && WriteCovers) ? "from" : "tofrom";
    if (Read && Written)
      return "tofrom";
    return Written ? "from" : "to";
  }

  /// The map() list item: SectionText, or base[0:ExtentText].
  std::string listItem() const;
};

struct RegionPlan {
  /// Every base got a section.
  bool Mappable = true;
  /// Why not, one line per base that didn't.
  std::vector<std::string> Why;
  std::vector<ArrayRegion> Regions;
  /// Conditions the sections need at run time ("n >= 1", ...), joined with
  /// &&; empty when none. A safety condition: kept under every policy.
  std::string MapGuard;
};

class RegionPlanner {
public:
  RegionPlanner(clang::ASTContext &Ctx, const std::vector<LoopInfo> &Loops);

  /// Sections for loop LI, from its accesses A (composed with callees'),
  /// with names written just before the loop in F.
  RegionPlan plan(const LoopInfo &LI, const LoopAccesses &A,
                  const clang::FunctionDecl *F);

private:
  clang::ASTContext &Ctx;
  const std::vector<LoopInfo> &Loops;
  llvm::DenseMap<const clang::ForStmt *, const LoopInfo *> LoopOf;
};

} // namespace p05

#endif // P05_ANALYSIS_DATAREGIONS_H
