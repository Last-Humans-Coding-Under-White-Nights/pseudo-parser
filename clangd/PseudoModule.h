//===--- PseudoModule.h - Pseudo-parser feature module -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDOMODULE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDOMODULE_H

#include "CodeComplete.h"
#include "DraftStore.h"
#include "FeatureModule.h"
#include "GlobalCompilationDatabase.h"
#include "LSPBinder.h"
#include "Protocol.h"
#include "SemanticHighlighting.h"
#include "XRefs.h"
#include "pseudo/AST.h"
#include "pseudo/Diagnostics.h"
#include "pseudo/Headers.h"
#include "pseudo/Parse.h"
#include "pseudo/Scopes.h"
#include "pseudo/Types.h"
#include "support/ThreadsafeFS.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

/// Feature module that uses clang-pseudo (GLR C++ pseudo-parser) to provide
/// language features (such as document symbols, go-to-definition, hover,
/// completions, and diagnostics) without requiring valid compilation commands,
/// headers, or complete types.
///
/// When --use-pseudo-parser is enabled, clangd operates in pure pseudo-parser
/// mode. When disabled, clangd uses the regular Clang AST-based parser.
class PseudoModule final : public FeatureModule {
public:
  PseudoModule();
  ~PseudoModule() override;

  void initializeLSP(LSPBinder &Bind,
                     const llvm::json::Object &ClientCaps,
                     llvm::json::Object &ServerCaps) override;

  std::unique_ptr<ASTListener> astListeners() override;

  void onDocumentDidOpen(const DidOpenTextDocumentParams &Params);
  void onDocumentDidChange(const DidChangeTextDocumentParams &Params);
  void onDocumentDidClose(const DidCloseTextDocumentParams &Params);

  void updateDraft(PathRef File, llvm::StringRef Contents,
                   llvm::StringRef Version = "");
  void removeDraft(PathRef File);
  void publishDiagnosticsFor(PathRef File, llvm::StringRef Contents,
                             llvm::StringRef Version = "");

  void setEnabled(bool E) { Enabled = E; }
  bool isEnabled() const { return Enabled; }

  void setPseudoOnly(bool P) {
    PseudoOnly = P;
    if (P)
      Enabled = true;
  }
  bool isPseudoOnly() const { return PseudoOnly; }

  bool hasFacilities() const { return HasFacilities; }
  void setFacilitiesInitialized(bool V = true) { HasFacilities = V; }

  /// Parse C++ code using clang-pseudo and extract hierarchical DocumentSymbols.
  llvm::Expected<std::vector<DocumentSymbol>>
  getDocumentSymbols(llvm::StringRef Code);

  /// Parse C++ code using clang-pseudo and compute semantic SelectionRanges.
  llvm::Expected<std::vector<SelectionRange>>
  getSemanticRanges(llvm::StringRef Code,
                    llvm::ArrayRef<Position> Positions);

  /// Parse C++ code using clang-pseudo and compute folding ranges.
  llvm::Expected<std::vector<FoldingRange>>
  getFoldingRanges(llvm::StringRef Code, bool LineFoldingOnly);

  /// Locate the definition / declaration of the symbol at \p Pos within \p Code.
  llvm::Expected<std::vector<LocatedSymbol>>
  locateSymbolAt(PathRef File, llvm::StringRef Code, Position Pos);

  /// Find all references to the symbol at \p Pos within \p Code.
  llvm::Expected<ReferencesResult>
  findReferences(PathRef File, llvm::StringRef Code, Position Pos,
                 uint32_t Limit = 0);

  std::string getDocument(PathRef File);

  /// Parse C++ code using clang-pseudo and compute Hover information.
  llvm::Expected<std::optional<Hover>>
  getHover(PathRef File, llvm::StringRef Code, Position Pos);

  /// Parse C++ code using clang-pseudo and extract HighlightingTokens.
  llvm::Expected<std::vector<HighlightingToken>>
  getSemanticHighlightings(llvm::StringRef Code);

  /// Parse C++ code using clang-pseudo and compute SemanticTokens.
  llvm::Expected<SemanticTokens>
  getSemanticTokens(llvm::StringRef Code);

  /// Parse C++ code using clang-pseudo and compute code completions.
  llvm::Expected<CompletionList>
  getCompletions(PathRef File, llvm::StringRef Code, Position Pos);

  /// Parse C++ code using clang-pseudo and compute AST.
  llvm::Expected<std::optional<ASTNode>>
  getAST(PathRef File, llvm::StringRef Code,
         std::optional<Range> R = std::nullopt);

  /// Parse C++ code using clang-pseudo and generate diagnostics.
  llvm::Expected<std::vector<Diagnostic>>
  getDiagnostics(PathRef File, llvm::StringRef Code);

  llvm::Expected<std::vector<Diagnostic>>
  getDiagnostics(llvm::StringRef Code);

  void onGoToDefinition(const TextDocumentPositionParams &Params,
                        Callback<std::vector<Location>> Reply);
  void onGoToDeclaration(const TextDocumentPositionParams &Params,
                         Callback<std::vector<Location>> Reply);
  void onReference(const ReferenceParams &Params,
                   Callback<std::vector<ReferenceLocation>> Reply);
  void onDocumentHighlight(const TextDocumentPositionParams &Params,
                           Callback<std::vector<DocumentHighlight>> Reply);
  void onDocumentSymbol(const DocumentSymbolParams &Params,
                        Callback<llvm::json::Value> Reply);
  void onSelectionRange(const SelectionRangeParams &Params,
                        Callback<std::vector<SelectionRange>> Reply);
  void onFoldingRange(const FoldingRangeParams &Params,
                      Callback<std::vector<FoldingRange>> Reply);
  void onHover(const TextDocumentPositionParams &Params,
               Callback<std::optional<Hover>> Reply);
  void onSemanticTokens(const SemanticTokensParams &Params,
                        Callback<SemanticTokens> Reply);
  void onSemanticTokensDelta(const SemanticTokensDeltaParams &Params,
                             Callback<SemanticTokensOrDelta> Reply);
  void onCompletion(const CompletionParams &Params,
                    Callback<CompletionList> Reply);
  void onAST(const ASTParams &Params, Callback<std::optional<ASTNode>> Reply);

  using IncludeDirective = clangd::IncludeDirective;
  using DeclKind = clangd::DeclKind;
  using HeaderDecl = clangd::HeaderDecl;
  using HeaderInfo = clangd::HeaderInfo;

  static inline bool isTypeDecl(DeclKind K) { return clangd::isTypeDecl(K); }
  static inline bool isValueDecl(DeclKind K) { return clangd::isValueDecl(K); }
  static inline bool isFunctionDecl(DeclKind K) {
    return clangd::isFunctionDecl(K);
  }

  void setCompilationDatabaseForTesting(const GlobalCompilationDatabase *CDB) {
    TestCDB = CDB;
  }
  void setFSForTesting(const ThreadsafeFS *FS) { TestFS = FS; }

  std::vector<std::string> getIncludeDirectories(PathRef File) const {
    return clangd::getIncludeDirectories(File, getCDB());
  }

  static std::vector<IncludeDirective> extractIncludes(llvm::StringRef Code) {
    return clangd::extractIncludes(Code);
  }

  std::string resolveHeader(const IncludeDirective &Inc,
                            llvm::StringRef CurrentDir,
                            llvm::ArrayRef<std::string> IncludeDirs,
                            llvm::vfs::FileSystem &FS) const {
    return clangd::resolveHeader(Inc, CurrentDir, IncludeDirs, FS);
  }

  std::shared_ptr<const HeaderInfo>
  getHeaderInfo(llvm::StringRef HeaderPath, llvm::vfs::FileSystem &FS) {
    return clangd::getHeaderInfo(HeaderPath, FS, HeaderCache, HeaderCacheMutex);
  }

  const GlobalCompilationDatabase *getCDB() const;
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> getFS() const;

private:
  void onCustomPseudoSymbols(const DocumentSymbolParams &Params,
                             Callback<std::vector<DocumentSymbol>> Reply);

  bool shouldRunCompletion(const CompletionParams &Params) const;

  bool Enabled = true;
  bool PseudoOnly = false;
  bool HasFacilities = false;
  bool SupportsHierarchicalDocumentSymbol = true;
  SymbolKindBitset SupportedSymbolKinds;
  MarkupKind HoverContentFormat = MarkupKind::PlainText;
  bool SupportsReferenceContainer = false;
  bool SupportsCompletionLabelDetails = false;
  CompletionItemKindBitset SupportedCompletionItemKinds;
  CodeCompleteOptions BaseCodeCompleteOpts;

  const GlobalCompilationDatabase *TestCDB = nullptr;
  const ThreadsafeFS *TestFS = nullptr;
  mutable std::mutex HeaderCacheMutex;
  llvm::StringMap<std::shared_ptr<const HeaderInfo>> HeaderCache;
  mutable std::mutex SemanticTokensMutex;
  llvm::StringMap<SemanticTokens> LastSemanticTokens;

  DraftStore DraftMgr;
  LSPBinder::OutgoingNotification<PublishDiagnosticsParams> PublishDiagnostics;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDOMODULE_H
