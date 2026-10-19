// Rejection — Phase 2.5 item 1: why a loop was not parallelized, as a fixed
// category, and what source change (if any) would remove the reason.
//
// Every reason a non-SAFE loop records carries one RejectCategory, set where
// the reason is produced (never recovered from the text afterwards). The
// categories are what Phase 3's rejection breakdown counts.
//
// A Hint is a claim, so it is only attached where the analysis itself
// guarantees the claim:
//   Fixes = true   applying the hint removes this reason and adds no other:
//                  `restrict` on a pointer parameter turns an Unproven alias
//                  pair into a Restrict proof (AliasAnalysis.h checks restrict
//                  only after the proofs that could come first, and nothing
//                  else reads the qualifier); -fp-reductions turns a refused
//                  floating-point reduction into a reduction clause. A loop
//                  whose every reason has a Fixes hint becomes SAFE once all
//                  of them are applied. scripts/check_hints.py applies the
//                  machine-readable edits and re-runs the tool to check that.
//   Fixes = false  the hint removes a reason the analysis cannot get past
//                  ("cannot see"), but what it finds behind it is unknown:
//                  making an opaque callee's definition visible.
// No hint is offered for a real hazard (a carried scalar, a proven array
// dependence): no source change preserves the program's meaning.
//
// -hints=off drops the hint lines from the report (the categories stay).
// -reasons=<file> appends one JSON object per loop (JSON Lines).

#ifndef P05_ANALYSIS_REJECTION_H
#define P05_ANALYSIS_REJECTION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <vector>

namespace p05 {

enum class RejectCategory {
  LoopShape,          ///< unrecognized loop, induction variable reassigned
  EarlyExit,          ///< break, return or goto out of the body
  IndirectCall,       ///< a call through a function pointer
  OpaqueCallee,       ///< a callee (or one it reaches) has no visible body
  Recursion,          ///< a callee recurses
  CalleeSideEffect,   ///< a callee writes a global or memory the loop uses
  AtomicOrAsm,        ///< atomic builtins or inline asm in the body
  IndirectSubscript,  ///< a subscript reads memory (`a[idx[i]]`)
  NonAffineSubscript, ///< any other subscript the tests can't handle
  UnknownBase,        ///< the accessed object can't be named
  PointerOverlap,     ///< two bases may be the same memory
  ArrayDependence,    ///< a proven loop-carried array dependence
  UnprovenDependence, ///< affine, but independence could not be proven
  ScalarDependence,   ///< a scalar carries a value across iterations
  SharedScalar,       ///< a written scalar that can't be privatized
  FpReduction,        ///< a floating-point reduction (off by default)
  LiveOutScalar,      ///< a scalar read after the loop, not always written
  Other,
};

/// Stable kebab-case name, used in the report and the JSON.
llvm::StringRef toString(RejectCategory C);

/// A source insertion (byte offset in File), or a tool flag.
struct HintEdit {
  std::string File;
  unsigned Offset = 0;
  std::string Insert;
  std::string Flag; ///< non-empty: a flag instead of an insertion
};

struct Hint {
  std::string Text;
  bool Fixes = false;
  /// What applying it means, machine-readably. A Fixes hint without edits
  /// is never made (it could not be checked).
  std::vector<HintEdit> Edits;
};

struct Reason {
  RejectCategory Cat = RejectCategory::Other;
  std::string Text;
  std::optional<Hint> Fix;

  Reason() = default;
  Reason(RejectCategory Cat, std::string Text)
      : Cat(Cat), Text(std::move(Text)) {}
};

bool hintsFromFlags();
/// Empty when -reasons was not given.
const std::string &reasonsPathFromFlags();

/// The category line and the hint lines for one loop's reasons (nothing
/// when Reasons is empty).
void printRejection(llvm::raw_ostream &OS, const std::vector<Reason> &Reasons);

/// Every reason has a hint that fixes it.
bool fixable(const std::vector<Reason> &Reasons);

/// One JSON Lines record for a loop.
struct LoopRecord {
  std::string File, Function;
  unsigned Line = 0, Col = 0;
  std::string Safety, Target;
  const std::vector<Reason> *Reasons = nullptr;
};
void writeRecord(llvm::raw_ostream &OS, const LoopRecord &R);

} // namespace p05

#endif // P05_ANALYSIS_REJECTION_H
