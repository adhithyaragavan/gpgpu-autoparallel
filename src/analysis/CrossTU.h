// CrossTU — Phase 2.5: calls into other files of the same program.
//
// `p05tool a.c b.c c.c --` analyses each file as before, but first gives it
// the bodies of the external functions it calls that are defined in one of
// the other files: every input is parsed once up front (CrossTUIndex), and
// before a file is analysed each such callee's definition is copied into its
// AST with clang::ASTImporter (what the static analyzer's CTU mode does),
// repeated until nothing new is called. The callee then simply has a body:
// Summaries, safety, alias and pricing need no change.
//
// Imported declarations are "foreign" (isForeign): their loops are not
// analysed in this file (they are, in their own), and proofs that need every
// caller or every use of a declaration to be visible never hold for them
// (their callers and uses in the other file were not imported). GPU offload
// is declined for a loop that reaches foreign code: its device build would
// need `declare target` in a file this tool does not rewrite.
//
// Identity: C has no overloading, so an external function is found by name;
// a name defined in two inputs (or in a header) is ambiguous and never
// imported. Imported globals and named types must merge with this file's
// declarations of the same name (the importer's structural equivalence); if
// one doesn't, two declarations would stand for one object or one type, so
// validate() reports it and the driver analyses the file without imports.

#ifndef P05_ANALYSIS_CROSSTU_H
#define P05_ANALYSIS_CROSSTU_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <memory>
#include <string>
#include <vector>

namespace p05 {

/// -cross-tu (default on; only matters with more than one input).
bool crossTUFromFlags();

/// Starts the analysis of Ctx: nothing in it is foreign until importInto.
void beginTranslationUnit(const clang::ASTContext &Ctx);

/// Was D (a function, variable or type) imported from another file into the
/// translation unit being analysed?
bool isForeign(const clang::Decl *D);
/// Is L in code imported from another file?
bool isForeignLocation(const clang::ASTContext &Ctx, clang::SourceLocation L);

/// A static variable imported from another file whose address that file
/// never lets out (AliasAnalyzer's escape scan, run on that file): as good
/// as one of this file's own non-escaping statics.
bool isNonEscapingForeignStatic(const clang::VarDecl *VD);

/// "'f' (b.c)": a foreign function and the file it came from.
std::string foreignLabel(const clang::FunctionDecl *F);

/// -whole-program with several inputs: is F named (called or its address
/// taken) in another input? Its callers there are not in this file.
bool referencedInAnotherInput(const clang::FunctionDecl *F);

/// What importInto did, for the report.
struct ImportResult {
  /// "f (b.c)" per imported function, in import order.
  std::vector<std::string> Imported;
  /// Why the imports can't be trusted (validate()); empty when they can.
  std::string Conflict;
};

class CrossTUIndex {
public:
  /// Parses every source of Tool (diagnostics suppressed: the analysis run
  /// reports them) and indexes each externally visible function defined in
  /// a source file itself (not in one with errors: error recovery may have
  /// dropped statements from its bodies).
  void build(clang::tooling::ClangTool &Tool);

  /// Starts the analysis of Ctx, the AST of input MainFile: records which
  /// functions the other inputs name (referencedInAnotherInput).
  void beginFile(const clang::ASTContext &Ctx, llvm::StringRef MainFile);

  /// Imports into Ctx, to a fixed point, the definition of every external
  /// function Ctx calls without a body that exactly one other input defines.
  /// MainFile is Ctx's own input (never imported from).
  ImportResult importInto(clang::ASTContext &Ctx, clang::FileManager &FM,
                          llvm::StringRef MainFile);

private:
  struct Def {
    clang::ASTUnit *Unit = nullptr;
    const clang::FunctionDecl *FD = nullptr; ///< null: ambiguous
  };
  /// Per input, its internal-linkage globals whose address doesn't escape.
  llvm::DenseMap<clang::ASTUnit *,
                 std::vector<const clang::VarDecl *>> NonEscaping;
  /// The parsed inputs' diagnostics go here, also while importing from them
  /// (the importer reports a mismatch on both sides).
  clang::IgnoringDiagConsumer Quiet;
  std::vector<std::unique_ptr<clang::ASTUnit>> ASTs;
  llvm::StringMap<Def> Defs;
  /// Per input, the external functions it names.
  llvm::DenseMap<clang::ASTUnit *, llvm::StringSet<>> Named;
};

} // namespace p05

#endif // P05_ANALYSIS_CROSSTU_H
