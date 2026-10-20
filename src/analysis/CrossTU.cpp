#include "analysis/CrossTU.h"
#include "analysis/AliasAnalysis.h"
#include "analysis/CallResolver.h"  // isKnownFunction
#include "analysis/Profitability.h" // optionCategory()

#include "clang/AST/ASTImporter.h"
#include "clang/AST/ASTImporterSharedState.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace clang;

namespace p05 {

namespace {
enum class OnOff { On, Off };
} // namespace

static llvm::cl::opt<OnOff> FlagCrossTU(
    "cross-tu",
    llvm::cl::desc("With several input files, calls into the others "
                   "(Phase 2.5)"),
    llvm::cl::values(
        clEnumValN(OnOff::On, "on",
                   "import the definitions another input has (default)"),
        clEnumValN(OnOff::Off, "off",
                   "such calls are opaque (ablation baseline)")),
    llvm::cl::init(OnOff::On), llvm::cl::cat(optionCategory()));

bool crossTUFromFlags() { return FlagCrossTU == OnOff::On; }

namespace {

/// The translation unit being analysed, and the first source-manager entry
/// importing created in it: a declaration located in that entry or a later
/// one came from another file (the importer gives every file it copies
/// from a new entry, even a header this file includes too).
struct ForeignState {
  const ASTContext *Ctx = nullptr;
  unsigned First = ~0u;
  llvm::DenseSet<const VarDecl *> NonEscaping;
  llvm::StringSet<> NamedElsewhere;
};
ForeignState &foreignState() {
  static ForeignState S;
  return S;
}

std::string realPath(StringRef P) {
  llvm::SmallString<256> Out;
  if (llvm::sys::fs::real_path(P, Out))
    return P.str();
  return std::string(Out);
}

std::string fileName(const ASTUnit &U) {
  return llvm::sys::path::filename(U.getMainFileName()).str();
}

bool sameKind(const NamedDecl *A, const NamedDecl *B) {
  return isa<VarDecl>(A) == isa<VarDecl>(B) &&
         isa<FunctionDecl>(A) == isa<FunctionDecl>(B) &&
         isa<TagDecl>(A) == isa<TagDecl>(B) &&
         isa<TypedefNameDecl>(A) == isa<TypedefNameDecl>(B);
}

/// Every imported declaration that must be one with this file's declaration
/// of the same name is; else why not.
std::string validate(ASTContext &Ctx) {
  // External objects and functions, at any scope (a block-scope `extern` is
  // not in the translation unit's lookup), by symbol name (an asm label
  // gives one symbol two C names): one canonical declaration each.
  struct Externals : RecursiveASTVisitor<Externals> {
    std::vector<const NamedDecl *> Out;
    bool VisitVarDecl(VarDecl *VD) {
      if (VD->hasGlobalStorage() && VD->isExternallyVisible())
        Out.push_back(VD);
      return true;
    }
    bool VisitFunctionDecl(FunctionDecl *FD) {
      if (FD->isExternallyVisible())
        Out.push_back(FD);
      return true;
    }
  } Ext;
  Ext.TraverseDecl(Ctx.getTranslationUnitDecl());
  ASTNameGenerator Gen(Ctx);
  struct Symbol {
    llvm::SmallPtrSet<const Decl *, 2> Canon;
    bool Foreign = false;
    const NamedDecl *Named = nullptr;
  };
  llvm::StringMap<Symbol> Symbols;
  std::vector<std::string> Order;
  for (const NamedDecl *D : Ext.Out) {
    const std::string Name = Gen.getName(D);
    if (Name.empty())
      continue;
    auto [It, New] = Symbols.try_emplace(Name);
    if (New)
      Order.push_back(Name);
    It->second.Canon.insert(D->getCanonicalDecl());
    It->second.Foreign |= isForeign(D);
    It->second.Named = D;
  }
  for (const std::string &Name : Order) {
    const Symbol &S = Symbols[Name];
    if (S.Foreign && S.Canon.size() > 1)
      return "'" + S.Named->getNameAsString() +
             "' did not merge with this file's declaration of it";
  }

  // Named types: an imported one must be this file's of the same name.
  TranslationUnitDecl *TU = Ctx.getTranslationUnitDecl();
  for (const Decl *D : TU->decls()) {
    const auto *ND = dyn_cast<NamedDecl>(D);
    if (!ND || !ND->getDeclName() || !isForeign(ND) ||
        !(isa<TagDecl>(ND) || isa<TypedefNameDecl>(ND)))
      continue;
    for (const NamedDecl *Own : TU->lookup(ND->getDeclName())) {
      if (isForeign(Own) || !sameKind(Own, ND))
        continue;
      bool Same = Own->getCanonicalDecl() == ND->getCanonicalDecl();
      // A typedef is only a name: the same type under it is enough.
      if (const auto *TA = dyn_cast<TypedefNameDecl>(ND))
        Same = Ctx.hasSameType(TA->getUnderlyingType(),
                               cast<TypedefNameDecl>(Own)->getUnderlyingType());
      if (!Same)
        return "'" + ND->getNameAsString() +
               "' did not merge with this file's declaration of it";
    }
  }
  return "";
}

} // namespace

void beginTranslationUnit(const ASTContext &Ctx) {
  foreignState() = {&Ctx, ~0u, {}, {}};
}

std::string foreignLabel(const FunctionDecl *F) {
  const FunctionDecl *Def = F->getDefinition() ? F->getDefinition() : F;
  const SourceManager &SM = F->getASTContext().getSourceManager();
  return "'" + F->getNameAsString() + "' (" +
         llvm::sys::path::filename(
             SM.getFilename(SM.getFileLoc(Def->getLocation())))
             .str() +
         ")";
}

bool referencedInAnotherInput(const FunctionDecl *F) {
  return F && F->getIdentifier() &&
         foreignState().NamedElsewhere.count(F->getName());
}

bool isForeignLocation(const ASTContext &Ctx, SourceLocation L) {
  const ForeignState &S = foreignState();
  if (&Ctx != S.Ctx || L.isInvalid())
    return false;
  const SourceManager &SM = Ctx.getSourceManager();
  const FileID F = SM.getFileID(SM.getFileLoc(L));
  const int ID = static_cast<int>(F.getHashValue());
  return ID > 0 && static_cast<unsigned>(ID) >= S.First;
}

bool isForeign(const Decl *D) {
  return D && isForeignLocation(D->getASTContext(), D->getLocation());
}

bool isNonEscapingForeignStatic(const VarDecl *VD) {
  return VD && foreignState().NonEscaping.count(VD->getCanonicalDecl());
}

void CrossTUIndex::build(tooling::ClangTool &Tool) {
  Tool.setDiagnosticConsumer(&Quiet);
  Tool.buildASTs(ASTs);
  Tool.setDiagnosticConsumer(nullptr);
  for (const std::unique_ptr<ASTUnit> &U : ASTs) {
    if (!U)
      continue;
    // Every external function it names, anywhere (callers for
    // referencedInAnotherInput).
    struct Names : RecursiveASTVisitor<Names> {
      llvm::StringSet<> &Out;
      explicit Names(llvm::StringSet<> &Out) : Out(Out) {}
      bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (const auto *FD = dyn_cast<FunctionDecl>(E->getDecl()))
          if (FD->isExternallyVisible() && FD->getIdentifier())
            Out.insert(FD->getName());
        return true;
      }
    } N(Named[U.get()]);
    N.TraverseDecl(U->getASTContext().getTranslationUnitDecl());
    const SourceManager &SM = U->getSourceManager();
    const bool Errors = U->getDiagnostics().hasErrorOccurred();
    for (const Decl *D : U->getASTContext().getTranslationUnitDecl()->decls()) {
      if (Errors)
        break;
      const auto *FD = dyn_cast<FunctionDecl>(D);
      // Defined in the input itself: one in a header would be defined in
      // every file including it.
      if (!FD || !FD->doesThisDeclarationHaveABody() ||
          !FD->isExternallyVisible() ||
          !SM.isWrittenInMainFile(FD->getLocation()))
        continue;
      auto [It, New] = Defs.try_emplace(FD->getName(), Def{U.get(), FD});
      if (!New && It->second.FD != FD)
        It->second.FD = nullptr; // defined twice: ambiguous
    }
    AliasAnalyzer Alias(U->getASTContext(), /*WholeProgram=*/false);
    for (const Decl *D : U->getASTContext().getTranslationUnitDecl()->decls())
      if (const auto *VD = dyn_cast<VarDecl>(D);
          VD && VD->hasGlobalStorage() && !VD->isExternallyVisible() &&
          !Alias.addressEscapes(VD))
        NonEscaping[U.get()].push_back(VD->getCanonicalDecl());
  }
}

void CrossTUIndex::beginFile(const ASTContext &Ctx, StringRef MainFile) {
  ForeignState &State = foreignState();
  if (State.Ctx != &Ctx)
    beginTranslationUnit(Ctx);
  const std::string Main = realPath(MainFile);
  for (const std::unique_ptr<ASTUnit> &U : ASTs)
    if (U && realPath(U->getMainFileName()) != Main)
      for (const auto &E : Named[U.get()])
        State.NamedElsewhere.insert(E.getKey());
}

ImportResult CrossTUIndex::importInto(ASTContext &Ctx, FileManager &FM,
                                      StringRef MainFile) {
  ImportResult R;
  if (!crossTUFromFlags() || Defs.empty())
    return R;
  ForeignState &State = foreignState();
  State.First = Ctx.getSourceManager().local_sloc_entry_size();
  const std::string Main = realPath(MainFile);
  auto Shared =
      std::make_shared<ASTImporterSharedState>(*Ctx.getTranslationUnitDecl());
  llvm::DenseMap<ASTUnit *, std::unique_ptr<ASTImporter>> Importers;

  struct Opaque : RecursiveASTVisitor<Opaque> {
    std::vector<const FunctionDecl *> Out;
    bool VisitCallExpr(CallExpr *CE) {
      if (const FunctionDecl *FD = CE->getDirectCallee())
        if (!FD->hasBody() && FD->isExternallyVisible() &&
            !isKnownFunction(FD))
          Out.push_back(FD);
      return true;
    }
  };
  llvm::StringSet<> Tried;
  for (bool Progress = true; Progress;) {
    Progress = false;
    Opaque V;
    V.TraverseDecl(Ctx.getTranslationUnitDecl());
    for (const FunctionDecl *FD : V.Out) {
      if (FD->hasBody() || !Tried.insert(FD->getName()).second)
        continue;
      auto It = Defs.find(FD->getName());
      if (It == Defs.end() || !It->second.FD ||
          realPath(It->second.Unit->getMainFileName()) == Main)
        continue;
      ASTUnit *U = It->second.Unit;
      std::unique_ptr<ASTImporter> &Imp = Importers[U];
      if (!Imp)
        Imp = std::make_unique<ASTImporter>(Ctx, FM, U->getASTContext(),
                                            U->getFileManager(),
                                            /*MinimalImport=*/false, Shared);
      llvm::Expected<Decl *> To =
          Imp->Import(const_cast<FunctionDecl *>(It->second.FD));
      if (!To) {
        llvm::consumeError(To.takeError());
        continue; // stays opaque
      }
      if (!FD->hasBody())
        continue; // not linked to this file's declaration: stays opaque
      R.Imported.push_back(FD->getNameAsString() + " (" + fileName(*U) + ")");
      Progress = true;
    }
  }
  for (auto &[U, Imp] : Importers)
    for (const VarDecl *VD : NonEscaping[U])
      if (Decl *To = Imp->GetAlreadyImportedOrNull(const_cast<VarDecl *>(VD)))
        if (const auto *ToVD = dyn_cast<VarDecl>(To))
          State.NonEscaping.insert(ToVD->getCanonicalDecl());
  // Also when nothing was listed: an import that failed part-way may still
  // have brought other declarations, even bodies, along.
  if (Ctx.getSourceManager().local_sloc_entry_size() > State.First)
    R.Conflict = validate(Ctx);
  return R;
}

} // namespace p05
