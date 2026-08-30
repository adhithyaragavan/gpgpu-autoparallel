#include "analysis/SafetyAnalysis.h"

#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/ADT/DenseSet.h"

using namespace clang;

namespace p05 {

llvm::StringRef toString(SafetyVerdict V) {
  switch (V) {
  case SafetyVerdict::Safe:
    return "SAFE";
  case SafetyVerdict::Unknown:
    return "UNKNOWN";
  case SafetyVerdict::Unsafe:
    return "UNSAFE";
  }
  return "UNKNOWN";
}

namespace {

/// Escalates Cur to New if New is the more severe of the two, in the order
/// Safe < Unknown < Unsafe. Never downgrades — one hazard anywhere is enough
/// to condemn the whole loop, so callers just fold every finding through this
/// rather than tracking flags separately and reconciling them at the end.
void escalate(SafetyVerdict &Cur, SafetyVerdict New) {
  if (New == SafetyVerdict::Unsafe)
    Cur = SafetyVerdict::Unsafe;
  else if (New == SafetyVerdict::Unknown && Cur == SafetyVerdict::Safe)
    Cur = SafetyVerdict::Unknown;
}

/// True when E, stripped of parens/implicit casts, is a bare reference to VD.
/// Local counterpart to LoopAnalysis.cpp's refersToVar — that one lives in an
/// anonymous namespace there and isn't reachable from here.
bool refersToVar(const Expr *E, const ValueDecl *VD) {
  if (!E || !VD)
    return false;
  const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
  return DRE && DRE->getDecl()->getCanonicalDecl() == VD->getCanonicalDecl();
}

/// Unwraps one level of "written through" indirection (deref, subscript, or
/// arrow-member access) at a time and reports whether any indirection was
/// crossed at all, so callers can tell "assigned to the variable itself"
/// (`p = x`, a local rebind) apart from "wrote through it"
/// (`*p = x`, `p[i] = x`, `p->f = x`, which reach whatever the variable
/// points to). Returns the root ValueDecl of the lvalue's base, or null if
/// the shape isn't one recognized here (e.g. the target is itself a call's
/// return value) — surfaced by callers as "cannot see", not silently ignored,
/// consistent with this file's coarse-but-loud scope.
const ValueDecl *unwrapWriteTarget(const Expr *E, bool &CrossedIndirection) {
  E = E->IgnoreParenCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref) {
      CrossedIndirection = true;
      return unwrapWriteTarget(UO->getSubExpr(), CrossedIndirection);
    }
  }
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    CrossedIndirection = true;
    return unwrapWriteTarget(ASE->getBase(), CrossedIndirection);
  }
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (ME->isArrow())
      CrossedIndirection = true;
    return unwrapWriteTarget(ME->getBase(), CrossedIndirection);
  }
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  return nullptr;
}

std::string exprText(const Expr *E) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  E->printPretty(OS, nullptr, PrintingPolicy(LangOptions()));
  return OS.str();
}

/// --- Half 1: interprocedural — what does a function's own body write? -----

class EffectVisitor : public RecursiveASTVisitor<EffectVisitor> {
public:
  explicit EffectVisitor(FunctionEffects &Effects) : Effects(Effects) {}

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp())
      recordWrite(BO->getLHS());
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      recordWrite(UO->getSubExpr());
    return true;
  }

private:
  void recordWrite(const Expr *Target) {
    bool CrossedIndirection = false;
    const ValueDecl *Root = unwrapWriteTarget(Target, CrossedIndirection);
    if (!Root) {
      Effects.HasUnknownWrite = true;
      Effects.Reasons.push_back(
          "writes to a target this analysis cannot resolve to a variable");
      return;
    }

    // ParmVarDecl is-a VarDecl, so it is checked first: a parameter's own
    // storage class is always automatic, but folding it into the "plain
    // VarDecl" branch below would blur the fact this whole check hinges on
    // — whether the write reached the *caller's* memory or just rebound a
    // local copy.
    if (const auto *PVD = dyn_cast<ParmVarDecl>(Root)) {
      if (CrossedIndirection) {
        Effects.WritesThroughPointerParam = true;
        Effects.Reasons.push_back("writes through pointer parameter '" +
                                   PVD->getNameAsString() + "'");
      }
      // `p = other;` with no indirection: rebinds this function's own copy
      // of the pointer, invisible to the caller. Not a side effect.
      return;
    }

    if (const auto *VD = dyn_cast<VarDecl>(Root)) {
      if (VD->hasGlobalStorage()) {
        // Covers file-scope globals and function-local `static`s alike: both
        // persist state across calls/iterations the same way, so both are a
        // real hazard for the loop that (transitively) calls this function —
        // not just "globals" in the narrow syntactic sense.
        Effects.WritesGlobal = true;
        Effects.Reasons.push_back("writes global '" + VD->getNameAsString() +
                                   "'");
      }
      // An ordinary local: mutating it is invisible outside this call, so it
      // is not a side effect for this analysis's purposes.
    }
  }

  FunctionEffects &Effects;
};

/// --- Half 2: intraprocedural — does the loop's own body carry a dependence
/// across iterations? ---------------------------------------------------

class BodyDependenceVisitor
    : public RecursiveASTVisitor<BodyDependenceVisitor> {
public:
  BodyDependenceVisitor(const VarDecl *InductionVar, LoopSafety &Safety)
      : InductionVar(InductionVar), Safety(Safety) {}

  // Declarations inside the loop body are private per iteration — visited
  // before any use, since C requires a declaration to precede its uses, so a
  // single top-to-bottom pass is enough to have every relevant one recorded
  // by the time a write to it is checked.
  bool VisitDeclStmt(DeclStmt *DS) {
    for (Decl *D : DS->decls())
      if (auto *VD = dyn_cast<VarDecl>(D))
        LocalVars.insert(VD);
    return true;
  }

  // Every array/pointer access — read or write alike — must be indexed by
  // exactly the induction variable. See the header comment on this file for
  // why that is what makes the check sound without alias analysis.
  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    if (!refersToVar(ASE->getIdx(), InductionVar))
      flag("array access not indexed by the induction variable alone: '" +
           exprText(ASE) + "'");
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    // `p->f`: the same physical field is touched on every iteration
    // regardless of which iteration is running — a fixed location, not an
    // induction-variable-indexed one. No subscript concept to check against.
    if (ME->isArrow())
      flag("dereferences through '->' without indexing by the induction "
           "variable: '" +
           exprText(ME) + "'");
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->getOpcode() == UO_Deref) {
      // `*p` with no subscript: same address every iteration. (`*(p + i)` is
      // not recognized as induction-variable-indexed here — see the known
      // over-approximations in the header comment.)
      flag("dereferences a pointer without indexing by the induction "
           "variable: '" +
           exprText(UO) + "'");
    } else if (UO->isIncrementDecrementOp()) {
      recordWrite(UO->getSubExpr());
    }
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp())
      recordWrite(BO->getLHS());
    return true;
  }

private:
  void recordWrite(const Expr *Target) {
    bool CrossedIndirection = false;
    const ValueDecl *Root = unwrapWriteTarget(Target, CrossedIndirection);
    if (!Root) {
      flag("writes to a target this analysis cannot resolve to a variable",
           SafetyVerdict::Unknown);
      return;
    }
    // Indirected writes (`*p =`, `a[i] =`, `p->f =`) were already checked for
    // induction-variable indexing by the Visit*Expr handlers above; nothing
    // further to say about them here.
    if (CrossedIndirection)
      return;

    if (refersToVar(Target, InductionVar)) {
      flag("loop body reassigns the induction variable '" +
           InductionVar->getNameAsString() + "'");
      return;
    }

    if (const auto *VD = dyn_cast<VarDecl>(Root)) {
      if (!LocalVars.count(VD)) {
        // Not the induction variable, not declared fresh inside this loop's
        // body: a scalar that survives across iterations, whether it is a
        // global, a variable declared outside the loop, or a parameter
        // reassigned directly (all three carry state loop-to-loop the same
        // way). Covers the classic reduction shape (`sum += a[i]`), which
        // this coarse check flags UNSAFE rather than recognizing as a
        // parallelizable pattern — see the header comment.
        flag("loop-carried dependence: writes to '" + VD->getNameAsString() +
             "', which is not declared inside the loop body");
      }
    }
  }

  void flag(std::string Reason, SafetyVerdict Severity = SafetyVerdict::Unsafe) {
    escalate(Safety.Verdict, Severity);
    Safety.Reasons.push_back(std::move(Reason));
  }

  const VarDecl *InductionVar;
  llvm::DenseSet<const VarDecl *> LocalVars;
  LoopSafety &Safety;
};

} // namespace

const FunctionEffects &SafetyAnalyzer::getEffects(const FunctionDecl *FD) {
  FD = FD->getCanonicalDecl();
  if (auto It = Cache.find(FD); It != Cache.end())
    return It->second;

  FunctionEffects Effects;
  // getDefinition() (not FD itself) is the redecl that actually carries the
  // body; may differ from the canonical decl passed in.
  if (const FunctionDecl *Def = FD->getDefinition()) {
    EffectVisitor Visitor(Effects);
    Visitor.TraverseStmt(Def->getBody());
  }
  // No definition visible: Effects stays empty here. Callers must treat "no
  // visible body" as its own conservative hazard (see analyzeLoop below)
  // rather than reading an empty FunctionEffects as "no effects" — this
  // function does not conflate the two.

  return Cache.try_emplace(FD, std::move(Effects)).first->second;
}

LoopSafety SafetyAnalyzer::analyzeLoop(const LoopInfo &LI) {
  LoopSafety Safety;

  // An unrecognized loop shape means nothing downstream (LI.InductionVar
  // included) is trustworthy enough to run the body-dependence walk against.
  // Such loops were never headed for codegen anyway; UNKNOWN says so
  // explicitly instead of producing a verdict built on facts we don't have.
  if (LI.Kind == LoopKind::Unrecognized) {
    Safety.Verdict = SafetyVerdict::Unknown;
    Safety.Reasons.push_back("loop shape not recognized (" +
                              LI.UnrecognizedReason +
                              "); safety not evaluated");
    return Safety;
  }

  if (LI.HasIndirectCall) {
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
    Safety.Reasons.push_back(
        "loop body contains an indirect call that cannot be resolved");
  }

  for (const FunctionDecl *Callee : LI.Callees) {
    if (!Callee->hasBody()) {
      escalate(Safety.Verdict, SafetyVerdict::Unknown);
      Safety.Reasons.push_back("'" + Callee->getNameAsString() +
                                "' has no visible body in this TU");
      continue;
    }

    const FunctionEffects &Effects = getEffects(Callee);
    if (Effects.WritesGlobal || Effects.WritesThroughPointerParam)
      escalate(Safety.Verdict, SafetyVerdict::Unsafe);
    if (Effects.HasUnknownWrite)
      escalate(Safety.Verdict, SafetyVerdict::Unknown);
    for (const std::string &Reason : Effects.Reasons)
      Safety.Reasons.push_back("'" + Callee->getNameAsString() + "' " +
                                Reason);

    CallChainSummary Summary = Resolver.getReachable(Callee);
    if (Summary.HasRecursion) {
      escalate(Safety.Verdict, SafetyVerdict::Unknown);
      Safety.Reasons.push_back("'" + Callee->getNameAsString() +
                                "' recurses; call chain cannot be bounded");
    }
    for (const ResolvedCallee &RC : Summary.Reachable) {
      if (!RC.HasVisibleBody) {
        escalate(Safety.Verdict, SafetyVerdict::Unknown);
        Safety.Reasons.push_back(
            "'" + Callee->getNameAsString() + "' reaches '" +
            RC.Callee->getNameAsString() + "', which has no visible body");
        continue;
      }
      const FunctionEffects &ReachEffects = getEffects(RC.Callee);
      if (ReachEffects.WritesGlobal || ReachEffects.WritesThroughPointerParam)
        escalate(Safety.Verdict, SafetyVerdict::Unsafe);
      if (ReachEffects.HasUnknownWrite)
        escalate(Safety.Verdict, SafetyVerdict::Unknown);
      for (const std::string &Reason : ReachEffects.Reasons)
        Safety.Reasons.push_back("'" + Callee->getNameAsString() +
                                  "' transitively reaches '" +
                                  RC.Callee->getNameAsString() + "', which " +
                                  Reason);
    }
  }

  if (LI.InductionVar) {
    BodyDependenceVisitor Visitor(LI.InductionVar, Safety);
    Visitor.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
  }

  return Safety;
}

void printSafetyReport(llvm::raw_ostream &OS, const LoopSafety &Safety) {
  OS << "  safety     : " << toString(Safety.Verdict) << "\n";
  for (const std::string &Reason : Safety.Reasons)
    OS << "    - " << Reason << "\n";
  OS << "\n";
}

} // namespace p05
