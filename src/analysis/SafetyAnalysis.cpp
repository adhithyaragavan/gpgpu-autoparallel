#include "analysis/SafetyAnalysis.h"

#include "analysis/DependenceTest.h"

#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/ADT/DenseSet.h"

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
      unknown("calls through a function pointer");
    return true;
  }
  bool VisitAtomicExpr(AtomicExpr *) {
    unknown("uses an atomic builtin");
    return true;
  }
  bool VisitAsmStmt(AsmStmt *) {
    unknown("contains inline asm");
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
  void unknown(const std::string &What) {
    Effects.HasUnknownWrite = true;
    Effects.AccessesThroughPointer = true;
    if (!llvm::is_contained(Effects.Reasons, What))
      Effects.Reasons.push_back(What);
  }
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
        Effects.Reasons.push_back("writes through local '" +
                                   VD->getNameAsString() +
                                   "', which may point to the caller's memory");
        return;
      }
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
      flag("writes to a target this analysis cannot resolve to a variable",
           SafetyVerdict::Unknown);
      return;
    }
    // Indirected writes (`*p =`, `a[i] =`, `p->f =`) are array/pointer
    // accesses: the dependence tests (DependenceTest.h) decide them, over
    // AffineAccess's access list. Nothing further to say about them here.
    if (CrossedIndirection)
      return;

    if (refersToVar(Target, InductionVar)) {
      flag("loop body reassigns the induction variable '" +
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
        flag("loop-carried dependence: writes to part of '" +
             VD->getNameAsString() +
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
  std::vector<std::string> Found;
  unsigned BreakTargets = 0;
  void note(const std::string &S) {
    if (!llvm::is_contained(Found, S))
      Found.push_back(S);
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
    note("loop body uses an atomic builtin");
    return true;
  }
  bool VisitAsmStmt(AsmStmt *) {
    note("loop body contains inline asm");
    return true;
  }
};

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
                                       const LoopScalars &Scalars) {
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
        // A global pointer can point anywhere; a global array is itself.
        const bool Hazard = G->getType()->isPointerType()
                                ? !WrittenBases.empty() || WritesMayAlias
                                : WrittenBases.count(G) || WritesMayAlias;
        if (Hazard) {
          escalate(Safety.Verdict, SafetyVerdict::Unknown);
          Safety.Reasons.push_back("'" + F->getNameAsString() +
                                   "' uses global '" + G->getNameAsString() +
                                   "', which the loop writes");
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
      if (!F || !Through)
        continue;
      const VarDecl *Root = pointerRoot(Arg);
      const bool Named = isNamedObject(Root);
      const bool Hazard = Named ? WrittenBases.count(Root) || WritesMayAlias
                                : !WrittenBases.empty() || WritesMayAlias;
      if (Hazard) {
        escalate(Safety.Verdict, SafetyVerdict::Unknown);
        Safety.Reasons.push_back(
            "'" + F->getNameAsString() + "' accesses memory through an "
            "argument" +
            (Named ? " into '" + Root->getNameAsString() + "'"
                   : std::string(" that may point anywhere")) +
            ", and the loop writes " +
            (Named ? std::string("it") : std::string("memory")));
      }
    }
  }

  // Early exits and invisible memory effects in the body itself.
  BodyHazardVisitor BH;
  BH.TraverseStmt(const_cast<Stmt *>(LI.Loop->getBody()));
  for (const std::string &R : BH.Found) {
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
    Safety.Reasons.push_back(R);
  }

  // Array/pointer accesses: can two iterations touch the same element?
  LoopDependence Dep = testLoopDependences(Accesses, LI.Step ? *LI.Step : 1);
  if (Dep.Outcome == DepOutcome::Dependent)
    escalate(Safety.Verdict, SafetyVerdict::Unsafe);
  else if (Dep.Outcome == DepOutcome::Unknown)
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
  for (std::string &R : Dep.Reasons)
    Safety.Reasons.push_back(std::move(R));
  Safety.DependenceSummary = dependenceSummary(Dep);

  // Scalars the loop writes by name (ScalarAnalysis.h).
  for (const std::string &R : Scalars.UnsafeReasons) {
    escalate(Safety.Verdict, SafetyVerdict::Unsafe);
    Safety.Reasons.push_back(R);
  }
  for (const std::string &R : Scalars.UnknownReasons) {
    escalate(Safety.Verdict, SafetyVerdict::Unknown);
    Safety.Reasons.push_back(R);
  }
  Safety.ScalarClauses = Scalars.clauseText();
  // Memory a callee reaches through a pointer argument or a global is not in
  // any map() clause this pass can size.
  // A lastprivate or reduction scalar needs its own map() on a device.
  Safety.GpuMappable = Dep.GpuMappable && !PassesPointer &&
                       !CalleeUsesGlobals && !CalleeThroughPointer &&
                       !Scalars.needsHost();

  return Safety;
}

void printSafetyReport(llvm::raw_ostream &OS, const LoopSafety &Safety) {
  OS << "  safety     : " << toString(Safety.Verdict) << "\n";
  for (const std::string &Reason : Safety.Reasons)
    OS << "    - " << Reason << "\n";
  if (!Safety.DependenceSummary.empty())
    OS << "  deps       : " << Safety.DependenceSummary << "\n";
  if (Safety.Verdict == SafetyVerdict::Safe && !Safety.ScalarClauses.empty())
    OS << "  scalars    :" << Safety.ScalarClauses << "\n";
  OS << "\n";
}

} // namespace p05
