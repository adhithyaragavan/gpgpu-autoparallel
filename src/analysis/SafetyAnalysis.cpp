#include "analysis/SafetyAnalysis.h"
#include "analysis/CrossTU.h"

#include "analysis/DependenceTest.h"

#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"

#include <functional>

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
    if (UO->getOpcode() == UO_Deref)
      Effects.AccessesThroughPointer = true;
    return true;
  }
  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    // Indexing a pointer (not a named array) reaches memory chosen by
    // whoever computed the pointer.
    if (ASE->getBase()->IgnoreParenImpCasts()->getType()->isPointerType())
      Effects.AccessesThroughPointer = true;
    return true;
  }
  bool VisitMemberExpr(MemberExpr *ME) {
    if (ME->isArrow())
      Effects.AccessesThroughPointer = true;
    return true;
  }
  // A call through a function pointer could run anything; atomic builtins
  // and inline asm write memory without an assignment this pass sees.
  bool VisitCallExpr(CallExpr *CE) {
    if (!CE->getDirectCallee())
      unknown("calls through a function pointer", RejectCategory::IndirectCall);
    return true;
  }
  bool VisitAtomicExpr(AtomicExpr *) {
    unknown("uses an atomic builtin", RejectCategory::AtomicOrAsm);
    return true;
  }
  bool VisitAsmStmt(AsmStmt *) {
    unknown("contains inline asm", RejectCategory::AtomicOrAsm);
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      if (VD->hasGlobalStorage() && !VD->isStaticLocal()) {
        const VarDecl *C = VD->getCanonicalDecl();
        if (!llvm::is_contained(Effects.GlobalsUsed, C))
          Effects.GlobalsUsed.push_back(C);
      }
    return true;
  }

private:
  void unknown(const std::string &What, RejectCategory Cat) {
    Effects.HasUnknownWrite = true;
    Effects.AccessesThroughPointer = true;
    if (llvm::none_of(Effects.Reasons,
                      [&](const Reason &R) { return R.Text == What; }))
      Effects.Reasons.push_back({Cat, What});
  }
  void recordWrite(const Expr *Target) {
    bool CrossedIndirection = false;
    const ValueDecl *Root = unwrapWriteTarget(Target, CrossedIndirection);
    if (!Root) {
      Effects.HasUnknownWrite = true;
      Effects.Reasons.push_back(
          {RejectCategory::CalleeSideEffect,
           "writes to a target this analysis cannot resolve to a variable"});
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
        Effects.Reasons.push_back({RejectCategory::CalleeSideEffect,
                                   "writes through pointer parameter '" +
                                       PVD->getNameAsString() + "'"});
      }
      // `p = other;` with no indirection: rebinds this function's own copy
      // of the pointer, invisible to the caller. Not a side effect.
      return;
    }

    if (const auto *VD = dyn_cast<VarDecl>(Root)) {
      // Written through a local pointer (`double *q = p + 1; *q = ...`): it
      // may point into the caller's memory, which this coarse pass can't
      // follow. "Cannot see", so UNKNOWN rather than UNSAFE.
      // Written through a local that holds a pointer — a pointer, a struct
      // or array of pointers, or an integer cast to one (`s.p[i] = `,
      // `ps[0][i] = `): it may point into the caller's memory, which this
      // coarse pass can't follow. "Cannot see", so UNKNOWN. Only a plain
      // local array is known to be the function's own storage.
      if (CrossedIndirection && VD->hasLocalStorage() &&
          !(VD->getType()->isArrayType() &&
            !VD->getType()->getPointeeOrArrayElementType()->isPointerType() &&
            !VD->getType()->getPointeeOrArrayElementType()->isRecordType())) {
        Effects.HasUnknownWrite = true;
        Effects.Reasons.push_back(
            {RejectCategory::CalleeSideEffect,
             "writes through local '" + VD->getNameAsString() +
                 "', which may point to the caller's memory"});
        return;
      }
      if (VD->hasGlobalStorage()) {
        // Covers file-scope globals and function-local `static`s alike: both
        // persist state across calls/iterations the same way, so both are a
        // real hazard for the loop that (transitively) calls this function —
        // not just "globals" in the narrow syntactic sense.
        Effects.WritesGlobal = true;
        Effects.Reasons.push_back({RejectCategory::CalleeSideEffect,
                                   "writes global '" + VD->getNameAsString() +
                                       "'"});
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
  // Only automatic variables are fresh per iteration; a `static` declared in
  // the body is one object shared by every iteration.
  bool VisitDeclStmt(DeclStmt *DS) {
    for (Decl *D : DS->decls())
      if (auto *VD = dyn_cast<VarDecl>(D))
        if (VD->hasLocalStorage())
          LocalVars.insert(VD);
    return true;
  }

  // Array and pointer accesses (`a[...]`, `*p`, `p->f`) are not checked
  // here any more: Phase 1.3's dependence tests (DependenceTest.h) decide
  // whether two iterations can touch the same element. This visitor keeps
  // the scalar rules: the induction variable must not be reassigned, and a
  // scalar declared outside the body must not be written.
  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      recordWrite(UO->getSubExpr());
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
      flag(RejectCategory::Other,
           "writes to a target this analysis cannot resolve to a variable",
           SafetyVerdict::Unknown);
      return;
    }
    // Indirected writes (`*p =`, `a[i] =`, `p->f =`) are array/pointer
    // accesses: the dependence tests (DependenceTest.h) decide them, over
    // AffineAccess's access list. Nothing further to say about them here.
    if (CrossedIndirection)
      return;

    if (refersToVar(Target, InductionVar)) {
      flag(RejectCategory::LoopShape,
           "loop body reassigns the induction variable '" +
           InductionVar->getNameAsString() + "'");
      return;
    }

    // A write to a variable by name (`x = `, `x += `) is classified by
    // ScalarAnalysis (Phase 1.4): private, lastprivate, reduction, or a
    // genuine loop-carried scalar. What remains here is writing *part* of a
    // variable declared outside the body (`s.x = ` on an outer struct), which
    // no clause makes safe.
    if (isa<DeclRefExpr>(Target->IgnoreParenCasts()))
      return;
    if (const auto *VD = dyn_cast<VarDecl>(Root)) {
      if (!LocalVars.count(VD)) {
        flag(RejectCategory::ScalarDependence,
             "loop-carried dependence: writes to part of '" +
             VD->getNameAsString() +
             "', which is not declared inside the loop body");
      }
    }
  }

  void flag(RejectCategory Cat, std::string Text,
            SafetyVerdict Severity = SafetyVerdict::Unsafe) {
    escalate(Safety.Verdict, Severity);
    Safety.Reasons.push_back({Cat, std::move(Text)});
  }

  const VarDecl *InductionVar;
  llvm::DenseSet<const VarDecl *> LocalVars;
  LoopSafety &Safety;
};

/// The named variable a pointer-valued argument is computed from: `a`,
/// `&a[i]`, `a + k`, `&s->f` (s). Crosses a subscript or member only under
/// `&` (address arithmetic); `qs[0]` or `s.p` is a pointer LOADED from
/// memory and could point anywhere, so it has no root. Explicit casts stop
/// it too (`(long)a`). Null = can't be named.
const VarDecl *pointerRoot(const Expr *E, bool UnderAddrOf = false) {
  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    return VD ? VD->getCanonicalDecl() : nullptr;
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_AddrOf)
    return pointerRoot(UO->getSubExpr(), true);
  if (UnderAddrOf) {
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
      return pointerRoot(ASE->getBase(), false);
    if (const auto *ME = dyn_cast<MemberExpr>(E))
      return ME->isArrow() ? pointerRoot(ME->getBase(), false)
                           : pointerRoot(ME->getBase(), true);
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(E);
      BO && (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub)) {
    if (BO->getLHS()->getType()->isPointerType())
      return pointerRoot(BO->getLHS(), false);
    if (BO->getRHS()->getType()->isPointerType())
      return pointerRoot(BO->getRHS(), false);
  }
  return nullptr;
}

/// Does a function's body assign to, or take the address of, VD?
bool writtenOrEscapedIn(const VarDecl *VD, const FunctionDecl *FD) {
  struct V : RecursiveASTVisitor<V> {
    const VarDecl *VD;
    bool Found = false;
    bool is(const Expr *E) {
      const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
      return DRE && DRE->getDecl()->getCanonicalDecl() == VD;
    }
    bool VisitBinaryOperator(BinaryOperator *BO) {
      Found |= BO->isAssignmentOp() && is(BO->getLHS());
      return true;
    }
    bool VisitUnaryOperator(UnaryOperator *UO) {
      Found |= (UO->isIncrementDecrementOp() || UO->getOpcode() == UO_AddrOf) &&
               is(UO->getSubExpr());
      return true;
    }
  } Vis;
  Vis.VD = VD;
  if (FD && FD->getBody())
    Vis.TraverseStmt(FD->getBody());
  return Vis.Found;
}

/// A root the analysis can name as one distinct object: a named array, or a
/// pointer parameter its function never reassigns (the documented no-alias
/// assumption between parameters). Local and global pointers can point
/// anywhere.
bool isNamedObject(const VarDecl *VD) {
  if (!VD)
    return false;
  if (VD->getType()->isArrayType())
    return true;
  if (const auto *PVD = dyn_cast<ParmVarDecl>(VD))
    return PVD->getType()->isPointerType() &&
           !writtenOrEscapedIn(
               PVD, dyn_cast_or_null<FunctionDecl>(PVD->getDeclContext()));
  return false;
}

/// An argument that carries no pointer: arithmetic-typed, and no
/// pointer-to-integer conversion inside it (`(long)a` carries one; `a[i]`
/// is just a loaded value).
bool carriesNoPointer(const Expr *E) {
  if (!E->getType()->isArithmeticType())
    return false;
  std::function<bool(const Stmt *)> PtrToInt = [&](const Stmt *S) {
    if (const auto *CE = dyn_cast<CastExpr>(S))
      if (CE->getCastKind() == CK_PointerToIntegral)
        return true;
    for (const Stmt *C : S->children())
      if (C && PtrToInt(C))
        return true;
    return false;
  };
  return !PtrToInt(E);
}

/// Loop-body constructs that end the iteration space early or touch memory
/// invisibly: `break` (not inside a nested loop/switch), `return`, `goto`,
/// atomic builtins, inline asm.
class BodyHazardVisitor : public RecursiveASTVisitor<BodyHazardVisitor> {
  using Base = RecursiveASTVisitor<BodyHazardVisitor>;

public:
  std::vector<Reason> Found;
  unsigned BreakTargets = 0;
  void note(const std::string &S,
            RejectCategory Cat = RejectCategory::EarlyExit) {
    if (llvm::none_of(Found, [&](const Reason &R) { return R.Text == S; }))
      Found.push_back({Cat, S});
  }
  bool TraverseForStmt(ForStmt *S, DataRecursionQueue * = nullptr) {
    ++BreakTargets;
    bool Ok = Base::TraverseForStmt(S, nullptr);
    --BreakTargets;
    return Ok;
  }
  bool TraverseWhileStmt(WhileStmt *S, DataRecursionQueue * = nullptr) {
    ++BreakTargets;
    bool Ok = Base::TraverseWhileStmt(S, nullptr);
    --BreakTargets;
    return Ok;
  }
  bool TraverseDoStmt(DoStmt *S, DataRecursionQueue * = nullptr) {
    ++BreakTargets;
    bool Ok = Base::TraverseDoStmt(S, nullptr);
    --BreakTargets;
    return Ok;
  }
  bool TraverseSwitchStmt(SwitchStmt *S, DataRecursionQueue * = nullptr) {
    ++BreakTargets;
    bool Ok = Base::TraverseSwitchStmt(S, nullptr);
    --BreakTargets;
    return Ok;
  }
  bool VisitBreakStmt(BreakStmt *) {
    if (BreakTargets == 0)
      note("loop body can 'break' out of the loop");
    return true;
  }
  bool VisitReturnStmt(ReturnStmt *) {
    note("loop body can 'return' from inside the loop");
    return true;
  }
  bool VisitGotoStmt(GotoStmt *) {
    note("loop body contains 'goto'");
    return true;
  }
  bool VisitIndirectGotoStmt(IndirectGotoStmt *) {
    note("loop body contains 'goto'");
    return true;
  }
  bool VisitAtomicExpr(AtomicExpr *) {
    note("loop body uses an atomic builtin", RejectCategory::AtomicOrAsm);
    return true;
  }
  bool VisitAsmStmt(AsmStmt *) {
    note("loop body contains inline asm", RejectCategory::AtomicOrAsm);
    return true;
  }
};

/// Reason for a callee without a visible body. Making the body visible only
/// lets the analysis see what the callee does, so the hint enables analysis
/// rather than fixing the loop.
Reason opaque(const FunctionDecl *F, std::string Text) {
  Reason R(RejectCategory::OpaqueCallee, std::move(Text));
  R.Fix = Hint{"make the definition of '" + F->getNameAsString() +
                   "' visible in this translation unit so its effects can be "
                   "analyzed",
               false,
               {}};
  return R;
}

/// Every call expression in a statement (inner loops included).
class CallCollector : public RecursiveASTVisitor<CallCollector> {
public:
  std::vector<const CallExpr *> Calls;
  bool VisitCallExpr(CallExpr *CE) {
    Calls.push_back(CE);
    return true;
  }
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

LoopSafety SafetyAnalyzer::analyzeLoop(const LoopInfo &LI,
                                       const LoopAccesses &Accesses,
                                       const LoopScalars &Scalars,
                                       const CallEffects *Calls) {
  LoopSafety Safety;
  // Phase 1.5: callee effects come composed into Accesses (plus Calls'
  // reasons) instead of from the coarse per-function flags.
  const bool Precise = Calls != nullptr;

  // An unrecognized loop shape means nothing downstream (LI.InductionVar
  // included) is trustworthy enough to run the body-dependence walk against.
  // Such loops were never headed for codegen anyway; UNKNOWN says so
  // explicitly instead of producing a verdict built on facts we don't have.
  if (LI.Kind == LoopKind::Unrecognized) {
    Safety.Verdict = SafetyVerdict::Unknown;
    Safety.Reasons.push_back({RejectCategory::LoopShape,
                              "loop shape not recognized (" +
                                  LI.UnrecognizedReason +
                                  "); safety not evaluated"});
    return Safety;
  }

  if (LI.HasIndirectCall) {
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
    Safety.Reasons.push_back(
        {RejectCategory::IndirectCall,
         "loop body contains an indirect call that cannot be resolved"});
  }

  for (const FunctionDecl *Callee : LI.Callees) {
    if (!Callee->hasBody()) {
      escalate(Safety.Verdict, SafetyVerdict::Unknown);
      Safety.Reasons.push_back(opaque(Callee, "'" + Callee->getNameAsString() +
                                                  "' has no visible body in "
                                                  "this TU"));
      continue;
    }

    if (!Precise) {
      const FunctionEffects &Effects = getEffects(Callee);
      if (Effects.WritesGlobal || Effects.WritesThroughPointerParam)
        escalate(Safety.Verdict, SafetyVerdict::Unsafe);
      if (Effects.HasUnknownWrite)
        escalate(Safety.Verdict, SafetyVerdict::Unknown);
      for (const Reason &R : Effects.Reasons)
        Safety.Reasons.push_back(
            {R.Cat, "'" + Callee->getNameAsString() + "' " + R.Text});
    }

    CallChainSummary Summary = Resolver.getReachable(Callee);
    if (Summary.HasRecursion) {
      escalate(Safety.Verdict, SafetyVerdict::Unknown);
      Safety.Reasons.push_back({RejectCategory::Recursion,
                                "'" + Callee->getNameAsString() +
                                    "' recurses; call chain cannot be "
                                    "bounded"});
    }
    for (const ResolvedCallee &RC : Summary.Reachable) {
      if (!RC.HasVisibleBody) {
        escalate(Safety.Verdict, SafetyVerdict::Unknown);
        Safety.Reasons.push_back(opaque(
            RC.Callee, "'" + Callee->getNameAsString() + "' reaches '" +
                           RC.Callee->getNameAsString() +
                           "', which has no visible body"));
        continue;
      }
      if (Precise)
        continue;
      const FunctionEffects &ReachEffects = getEffects(RC.Callee);
      if (ReachEffects.WritesGlobal || ReachEffects.WritesThroughPointerParam)
        escalate(Safety.Verdict, SafetyVerdict::Unsafe);
      if (ReachEffects.HasUnknownWrite)
        escalate(Safety.Verdict, SafetyVerdict::Unknown);
      for (const Reason &R : ReachEffects.Reasons)
        Safety.Reasons.push_back({R.Cat, "'" + Callee->getNameAsString() +
                                             "' transitively reaches '" +
                                             RC.Callee->getNameAsString() +
                                             "', which " + R.Text});
    }
  }

  if (Precise) {
    for (const Reason &R : Calls->UnsafeReasons) {
      escalate(Safety.Verdict, SafetyVerdict::Unsafe);
      Safety.Reasons.push_back(R);
    }
    for (const Reason &R : Calls->UnknownReasons) {
      escalate(Safety.Verdict, SafetyVerdict::Unknown);
      Safety.Reasons.push_back(R);
    }
    // An incomplete summary is a callee reaching an invisible body or
    // recursing, which the walk above reports; say so if it somehow didn't.
    for (const FunctionDecl *F : Calls->Incomplete) {
      escalate(Safety.Verdict, SafetyVerdict::Unknown);
      const CallChainSummary CS = Resolver.getReachable(F);
      if (!CS.HasOpaqueCallee && !CS.HasRecursion)
        Safety.Reasons.push_back({RejectCategory::OpaqueCallee,
                                  "'" + F->getNameAsString() +
                                      "' could not be fully summarized"});
    }
  }

  if (LI.InductionVar) {
    BodyDependenceVisitor Visitor(LI.InductionVar, Safety);
    Visitor.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
  }

  // What callees touch that the loop body doesn't show. The loop's own
  // array writes, by base:
  llvm::DenseSet<const VarDecl *> WrittenBases;
  bool WritesMayAlias = false;
  for (const ArrayAccess &X : Accesses.Accesses) {
    if (X.Kind != AccessKind::Write && X.Kind != AccessKind::ReadWrite)
      continue;
    if (const auto *VD = dyn_cast_or_null<VarDecl>(X.Base))
      WrittenBases.insert(VD->getCanonicalDecl());
    WritesMayAlias |= !X.Base || X.BaseMayAlias;
  }
  // Effects of a callee and everything it reaches (visible bodies only;
  // invisible ones already made the loop UNKNOWN above).
  auto ForEachReachable = [&](const FunctionDecl *F, auto Fn) {
    if (F->hasBody())
      Fn(F, getEffects(F));
    for (const ResolvedCallee &RC : Resolver.getReachable(F).Reachable)
      if (RC.HasVisibleBody)
        Fn(RC.Callee, getEffects(RC.Callee));
  };
  bool CalleeUsesGlobals = false;
  for (const FunctionDecl *Callee : LI.Callees)
    ForEachReachable(Callee, [&](const FunctionDecl *F,
                                 const FunctionEffects &E) {
      for (const VarDecl *G : E.GlobalsUsed) {
        CalleeUsesGlobals = true;
        // Elements of a global array a callee touches are composed into
        // the loop's accesses (Precise), and tested there.
        if (Precise && G->getType()->isArrayType())
          continue;
        // A global pointer can point anywhere; a global array is itself.
        const bool Hazard = G->getType()->isPointerType()
                                ? !WrittenBases.empty() || WritesMayAlias
                                : WrittenBases.count(G) || WritesMayAlias;
        if (Hazard) {
          escalate(Safety.Verdict, SafetyVerdict::Unknown);
          Safety.Reasons.push_back({RejectCategory::CalleeSideEffect,
                                    "'" + F->getNameAsString() +
                                        "' uses global '" +
                                        G->getNameAsString() +
                                        "', which the loop writes"});
        }
      }
    });
  // Arguments: a callee that reads or writes through a pointer it is handed
  // (or computes) touches whatever the argument points into. Any argument
  // that isn't a plain number may carry a pointer (a pointer, a struct or
  // array holding one, `(long)a`). It is checked against the loop's writes
  // when it can be named as one object; otherwise it could reach anything
  // the loop writes.
  CallCollector CC;
  CC.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
  bool PassesPointer = false, CalleeThroughPointer = false;
  for (const CallExpr *Call : CC.Calls) {
    const FunctionDecl *F = Call->getDirectCallee();
    bool Through = false;
    if (F)
      ForEachReachable(F, [&](const FunctionDecl *, const FunctionEffects &E) {
        Through |= E.AccessesThroughPointer || E.HasUnknownWrite;
      });
    CalleeThroughPointer |= Through;
    for (const Expr *Arg : Call->arguments()) {
      if (carriesNoPointer(Arg))
        continue;
      PassesPointer = true;
      // Precise: the callee's accesses through Arg are composed into the
      // loop's own and tested there.
      if (!F || !Through || Precise)
        continue;
      const VarDecl *Root = pointerRoot(Arg);
      const bool Named = isNamedObject(Root);
      const bool Hazard = Named ? WrittenBases.count(Root) || WritesMayAlias
                                : !WrittenBases.empty() || WritesMayAlias;
      if (Hazard) {
        escalate(Safety.Verdict, SafetyVerdict::Unknown);
        Safety.Reasons.push_back(
            {RejectCategory::CalleeSideEffect,
             "'" + F->getNameAsString() + "' accesses memory through an "
             "argument" +
                 (Named ? " into '" + Root->getNameAsString() + "'"
                        : std::string(" that may point anywhere")) +
                 ", and the loop writes " +
                 (Named ? std::string("it") : std::string("memory"))});
      }
    }
  }

  // Early exits and invisible memory effects in the body itself.
  BodyHazardVisitor BH;
  BH.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
  for (const Reason &R : BH.Found) {
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
    Safety.Reasons.push_back(R);
  }

  // Phase 1.6: a pointer parameter may point at a global the loop (or a
  // callee) reads by name, so each such global joins the accesses as a read
  // of its one element. Const globals can't be written through a pointer.
  LoopAccesses WithGlobals;
  const LoopAccesses *All = &Accesses;
  const FunctionDecl *Fn = Alias ? Alias->enclosingFunction(LI.Loop) : nullptr;
  if (Fn) {
    WithGlobals = Accesses;
    llvm::SetVector<const VarDecl *> Named;
    auto Consider = [&](const VarDecl *G) {
      if (G && G->hasGlobalStorage() && !G->getType()->isArrayType() &&
          !G->getType().isConstQualified())
        Named.insert(G->getCanonicalDecl());
    };
    struct NameVisitor : RecursiveASTVisitor<NameVisitor> {
      std::function<void(const VarDecl *)> F;
      bool VisitDeclRefExpr(DeclRefExpr *DRE) {
        F(dyn_cast<VarDecl>(DRE->getDecl()));
        return true;
      }
    } NV;
    NV.F = Consider;
    NV.TraverseStmt(const_cast<Expr *>(LI.Loop->getCond()));
    NV.TraverseStmt(const_cast<Expr *>(LI.Loop->getInc()));
    NV.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
    for (const FunctionDecl *Callee : LI.Callees)
      ForEachReachable(Callee,
                       [&](const FunctionDecl *, const FunctionEffects &E) {
                         for (const VarDecl *G : E.GlobalsUsed)
                           Consider(G);
                       });
    for (const VarDecl *G : Named) {
      ArrayAccess X;
      X.Text = G->getNameAsString() + " (by name)";
      X.Kind = AccessKind::Read;
      X.Base = G;
      X.Affine = true;
      X.Subscripts.push_back(AffineExpr());
      X.DimExtents.push_back(std::string());
      WithGlobals.Accesses.push_back(std::move(X));
    }
    All = &WithGlobals;
  }

  // Array/pointer accesses: can two iterations touch the same element?
  AliasOracle Oracle = [&](const ValueDecl *A, const ValueDecl *B) {
    return Alias->proof(Fn, A, B);
  };
  LoopDependence Dep = testLoopDependences(*All, LI.Step ? *LI.Step : 1,
                                           Fn ? &Oracle : nullptr);
  if (Dep.Outcome == DepOutcome::Dependent)
    escalate(Safety.Verdict, SafetyVerdict::Unsafe);
  else if (Dep.Outcome == DepOutcome::Unknown)
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
  for (Reason &R : Dep.Reasons)
    Safety.Reasons.push_back(std::move(R));
  Safety.DependenceSummary = dependenceSummary(Dep);

  // Bases that might overlap: proven apart, or checked at run time.
  std::vector<std::string> Checks;
  for (const AliasPair &P : Dep.AliasPairs) {
    const std::string Pair = "'" + P.A->getNameAsString() + "' and '" +
                             P.B->getNameAsString() + "'";
    if (P.Proof != AliasProof::Unproven) {
      Safety.AliasNotes.push_back(Pair + " can't overlap (" +
                                  toString(P.Proof).str() + ")");
      continue;
    }
    std::string Why;
    if (std::optional<std::string> C =
            Alias->runtimeCheck(Fn, LI.Loop->getBeginLoc(), P.A, P.B, *All,
                                Why)) {
      Checks.push_back(*C);
      Safety.AliasNotes.push_back(Pair + " may overlap: checked at run time");
      continue;
    }
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
    Reason R(RejectCategory::PointerOverlap,
             Pair + " may overlap (not restrict, and not every call is known "
                    "to pass disjoint objects), and no run-time check is "
                    "possible: " +
                 Why);
    R.Fix = Alias->restrictHint(Fn, P.A, P.B);
    Safety.Reasons.push_back(std::move(R));
  }
  for (const std::string &C : Checks)
    Safety.AliasGuard += (Safety.AliasGuard.empty() ? "" : " && ") + C;

  // Scalars the loop writes by name (ScalarAnalysis.h).
  for (const Reason &R : Scalars.UnsafeReasons) {
    escalate(Safety.Verdict, SafetyVerdict::Unsafe);
    Safety.Reasons.push_back(R);
  }
  for (const Reason &R : Scalars.UnknownReasons) {
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
    Safety.Reasons.push_back(R);
  }
  Safety.ScalarClauses = Scalars.clauseText();
  for (const FunctionDecl *Callee : LI.Callees) {
    std::vector<const FunctionDecl *> Fns{Callee};
    for (const ResolvedCallee &RC : Resolver.getReachable(Callee).Reachable)
      Fns.push_back(RC.Callee);
    for (const FunctionDecl *F : Fns)
      if (Safety.ForeignCallee.empty() && isForeign(F->getDefinition()))
        Safety.ForeignCallee = foreignLabel(F);
  }
  // Memory a callee reaches through a global, or through a pointer whose
  // accesses were not composed into the loop's, is not in any map() clause
  // this pass can size.
  Safety.CalleesMappable =
      !CalleeUsesGlobals &&
      (Precise || (!PassesPointer && !CalleeThroughPointer));
  // A lastprivate or reduction scalar needs its own map() on a device.
  for (const ScalarClause &C : Scalars.Clauses)
    if (C.K != ScalarClause::Private)
      Safety.HostScalars.push_back(C.Var);
  Safety.GpuMappable = Dep.GpuMappable && !PassesPointer &&
                       !CalleeUsesGlobals && !CalleeThroughPointer &&
                       !Scalars.needsHost();

  return Safety;
}

void markCollapsible(const std::vector<LoopInfo> &Loops,
                     const std::vector<LoopAccesses> &Accesses,
                     std::vector<LoopSafety> &Safety) {
  for (size_t K = 0; K < Loops.size(); ++K) {
    const LoopInfo &Outer = Loops[K];
    LoopSafety &S = Safety[K];
    if (S.Verdict != SafetyVerdict::Safe || !Outer.PerfectlyNested ||
        Outer.Children.size() != 1)
      continue;
    const LoopInfo &Inner = Loops[Outer.Children.front()];
    const LoopSafety &IS = Safety[Outer.Children.front()];
    auto No = [&](const std::string &Why) { S.CollapseNote = "no, " + Why; };
    if (Outer.Kind == LoopKind::Unrecognized ||
        Inner.Kind == LoopKind::Unrecognized || !Inner.InductionVar) {
      No("the inner loop is not a counted loop");
      continue;
    }
    const VarDecl *J = Inner.InductionVar->getCanonicalDecl();
    if (J == Outer.InductionVar->getCanonicalDecl()) {
      No("both loops use the same variable");
      continue;
    }
    if (IS.Verdict != SafetyVerdict::Safe) {
      No("the inner loop is not SAFE on its own");
      continue;
    }
    if (!IS.AliasGuard.empty()) {
      No("the inner loop needs its own run-time alias check");
      continue;
    }
    auto R = Accesses[K].Ranges.find(J);
    if (Inner.BoundsDependOnOuterIV || R == Accesses[K].Ranges.end() ||
        !R->second.Exact || !R->second.Lo->IVTerms.empty() ||
        !R->second.Hi->IVTerms.empty()) {
      No("the inner loop's range is not a fixed, exact rectangle");
      continue;
    }
    if (llvm::any_of(S.HostScalars, [&](const VarDecl *V) {
          return V->getCanonicalDecl() == J;
        })) {
      No("'" + J->getNameAsString() + "' is read after the loop");
      continue;
    }
    S.Collapse2 = true;
    S.CollapseNote = "2 (with '" + J->getNameAsString() + "')";
  }
}

void printSafetyReport(llvm::raw_ostream &OS, const LoopSafety &Safety) {
  OS << "  safety     : " << toString(Safety.Verdict) << "\n";
  for (const Reason &R : Safety.Reasons)
    OS << "    - " << R.Text << "\n";
  printRejection(OS, Safety.Reasons);
  if (!Safety.DependenceSummary.empty())
    OS << "  deps       : " << Safety.DependenceSummary << "\n";
  if (Safety.Verdict == SafetyVerdict::Safe && !Safety.ScalarClauses.empty())
    OS << "  scalars    :" << Safety.ScalarClauses << "\n";
  for (const std::string &N : Safety.AliasNotes)
    OS << "  alias      : " << N << "\n";
  if (Safety.Verdict == SafetyVerdict::Safe && !Safety.AliasGuard.empty())
    OS << "  alias guard: " << Safety.AliasGuard << "\n";
  if (!Safety.CollapseNote.empty())
    OS << "  collapse   : " << Safety.CollapseNote << "\n";
  OS << "\n";
}

} // namespace p05
