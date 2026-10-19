#include "analysis/Rejection.h"

#include "analysis/Profitability.h" // optionCategory()

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"

namespace p05 {

namespace {
enum class HintMode { On, Off };
}

static llvm::cl::opt<HintMode> FlagHints(
    "hints",
    llvm::cl::desc("Hints under a rejected loop, where a source change "
                   "would remove a reason"),
    llvm::cl::values(clEnumValN(HintMode::On, "on", "print them (default)"),
                     clEnumValN(HintMode::Off, "off",
                                "only the categories")),
    llvm::cl::init(HintMode::On), llvm::cl::cat(optionCategory()));

static llvm::cl::opt<std::string> FlagReasons(
    "reasons",
    llvm::cl::desc("Append one JSON object per loop (verdict, target, "
                   "categorized reasons, hints) to this file"),
    llvm::cl::init(""), llvm::cl::cat(optionCategory()));

bool hintsFromFlags() { return FlagHints == HintMode::On; }
const std::string &reasonsPathFromFlags() { return FlagReasons; }

llvm::StringRef toString(RejectCategory C) {
  switch (C) {
  case RejectCategory::LoopShape:
    return "loop-shape";
  case RejectCategory::EarlyExit:
    return "early-exit";
  case RejectCategory::IndirectCall:
    return "indirect-call";
  case RejectCategory::OpaqueCallee:
    return "opaque-callee";
  case RejectCategory::Recursion:
    return "recursion";
  case RejectCategory::CalleeSideEffect:
    return "callee-side-effect";
  case RejectCategory::AtomicOrAsm:
    return "atomic-or-asm";
  case RejectCategory::IndirectSubscript:
    return "indirect-subscript";
  case RejectCategory::NonAffineSubscript:
    return "non-affine-subscript";
  case RejectCategory::UnknownBase:
    return "unknown-base";
  case RejectCategory::PointerOverlap:
    return "pointer-overlap";
  case RejectCategory::ArrayDependence:
    return "array-dependence";
  case RejectCategory::UnprovenDependence:
    return "unproven-dependence";
  case RejectCategory::ScalarDependence:
    return "scalar-dependence";
  case RejectCategory::SharedScalar:
    return "shared-scalar";
  case RejectCategory::FpReduction:
    return "fp-reduction";
  case RejectCategory::LiveOutScalar:
    return "live-out-scalar";
  case RejectCategory::Other:
    return "other";
  }
  return "other";
}

bool fixable(const std::vector<Reason> &Reasons) {
  return !Reasons.empty() && llvm::all_of(Reasons, [](const Reason &R) {
    return R.Fix && R.Fix->Fixes;
  });
}

void printRejection(llvm::raw_ostream &OS, const std::vector<Reason> &Reasons) {
  if (Reasons.empty())
    return;
  std::vector<RejectCategory> Cats;
  for (const Reason &R : Reasons)
    if (!llvm::is_contained(Cats, R.Cat))
      Cats.push_back(R.Cat);
  OS << "  rejected   : ";
  for (size_t K = 0; K < Cats.size(); ++K)
    OS << (K ? ", " : "") << toString(Cats[K]);
  OS << "\n";
  if (!hintsFromFlags())
    return;
  std::vector<std::string> Seen;
  for (const Reason &R : Reasons) {
    if (!R.Fix || llvm::is_contained(Seen, R.Fix->Text))
      continue;
    Seen.push_back(R.Fix->Text);
    OS << "  hint       : " << R.Fix->Text
       << (R.Fix->Fixes ? " [fixes " + toString(R.Cat).str() + "]"
                        : std::string(" [enables analysis]"))
       << "\n";
  }
  if (fixable(Reasons))
    OS << "  hints      : applying every hint above makes this loop SAFE\n";
}

void writeRecord(llvm::raw_ostream &OS, const LoopRecord &R) {
  llvm::json::Array Reasons;
  if (R.Reasons)
    for (const Reason &X : *R.Reasons) {
      llvm::json::Object O{{"category", toString(X.Cat)}, {"text", X.Text}};
      if (X.Fix) {
        llvm::json::Array Edits;
        for (const HintEdit &E : X.Fix->Edits) {
          if (!E.Flag.empty())
            Edits.push_back(llvm::json::Object{{"flag", E.Flag}});
          else
            Edits.push_back(llvm::json::Object{
                {"file", E.File}, {"offset", E.Offset}, {"insert", E.Insert}});
        }
        O["hint"] = llvm::json::Object{{"text", X.Fix->Text},
                                       {"fixes", X.Fix->Fixes},
                                       {"edits", std::move(Edits)}};
      }
      Reasons.push_back(std::move(O));
    }
  const bool Fixable = R.Reasons && fixable(*R.Reasons);
  llvm::json::Object Rec{{"file", R.File},     {"line", R.Line},
                         {"col", R.Col},       {"function", R.Function},
                         {"safety", R.Safety}, {"target", R.Target},
                         {"fixable", Fixable}, {"reasons", std::move(Reasons)}};
  OS << llvm::json::Value(std::move(Rec)) << "\n";
}

} // namespace p05
