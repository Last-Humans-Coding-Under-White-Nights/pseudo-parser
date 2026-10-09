//===--- PseudoModule.cpp - Pseudo-parser feature module -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PseudoModule.h"
#include "DraftStore.h"
#include "Hover.h"
#include "LSPBinder.h"
#include "Protocol.h"
#include "SourceCode.h"
#include "TUScheduler.h"
#include "pseudo/AST.h"
#include "pseudo/Diagnostics.h"
#include "pseudo/Headers.h"
#include "pseudo/Parse.h"
#include "pseudo/Scopes.h"
#include "pseudo/Types.h"
#include "support/Logger.h"
#include "support/Threading.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TokenKinds.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
int EnableABIBreakingChecks = 0;
#else
int DisableABIBreakingChecks = 0;
#endif
} // namespace llvm

#if !defined(_WIN32)
// Fallback symbol definition for typeinfo of llvm::ErrorInfoBase.
// This prevents dlopen failure when the host clangd was built with -fno-rtti
// or when external toolchains have mismatched RTTI settings.
extern "C" {
__attribute__((weak, visibility("default")))
extern const char _ZTSN4llvm13ErrorInfoBaseE[] = "N4llvm13ErrorInfoBaseE";

__attribute__((weak, visibility("default")))
extern const void *_ZTIN4llvm13ErrorInfoBaseE[2] = {
    (const void *)0,
    (const void *)_ZTSN4llvm13ErrorInfoBaseE
};
}
#endif

namespace clang {
namespace clangd {

#if defined(_WIN32)
// Fallback definitions for static member variables in clang::clangd that are not
// exported from clangd.exe on Windows. In LLVM's Windows export script
// (extract_symbols.py), only variables in llvm:: are exported; variables in
// clang:: are omitted. Supplying local definitions within the module DLL
// satisfies the link requirements without conflicting with the host executable.
__declspec(selectany) char LSPError::ID = 0;
__declspec(selectany) const CodeCompleteOptions::CodeCompletionRankingModel
    CodeCompleteOptions::DefaultRankingModel =
        CodeCompleteOptions::DecisionForest;
#endif

namespace {

static Location *getToggle(const TextDocumentPositionParams &Point,
                           LocatedSymbol &Sym) {
  if (!Sym.Definition || *Sym.Definition == Sym.PreferredDeclaration)
    return nullptr;
  if (Sym.Definition->uri.file() == Point.textDocument.uri.file() &&
      Sym.Definition->range.contains(Point.position))
    return &Sym.PreferredDeclaration;
  if (Sym.PreferredDeclaration.uri.file() == Point.textDocument.uri.file() &&
      Sym.PreferredDeclaration.range.contains(Point.position))
    return &*Sym.Definition;
  return nullptr;
}

static void increment(std::string &S) {
  for (char &C : llvm::reverse(S)) {
    if (C != '9') {
      ++C;
      return;
    }
    C = '0';
  }
  S.insert(S.begin(), '1');
}

static void adjustSymbolKinds(llvm::MutableArrayRef<DocumentSymbol> Syms,
                             SymbolKindBitset Kinds) {
  for (auto &S : Syms) {
    S.kind = adjustKindToCapability(S.kind, Kinds);
    adjustSymbolKinds(S.children, Kinds);
  }
}

static std::vector<SymbolInformation>
flattenSymbolHierarchy(llvm::ArrayRef<DocumentSymbol> Symbols,
                       const URIForFile &FileURI) {
  std::vector<SymbolInformation> Results;
  struct SymbolHierarchyFlattener {
    const URIForFile &FileURI;
    std::vector<SymbolInformation> &Results;

    void append(const DocumentSymbol &S,
                std::optional<llvm::StringRef> ParentName) {
      SymbolInformation SI;
      SI.containerName = std::string(!ParentName ? "" : *ParentName);
      SI.name = S.name;
      SI.kind = S.kind;
      SI.location.range = S.range;
      SI.location.uri = FileURI;

      Results.push_back(std::move(SI));
      std::string FullName =
          !ParentName ? S.name : (ParentName->str() + "::" + S.name);
      for (const DocumentSymbol &C : S.children)
        append(C, /*ParentName=*/FullName);
    }
  };
  SymbolHierarchyFlattener Flattener{FileURI, Results};
  for (const DocumentSymbol &S : Symbols)
    Flattener.append(S, /*ParentName=*/"");
  return Results;
}

static SymbolKindBitset defaultSymbolKinds() {
  SymbolKindBitset Defaults;
  for (size_t I = SymbolKindMin; I <= static_cast<size_t>(SymbolKind::Array);
       ++I)
    Defaults.set(I);
  return Defaults;
}

static CompletionItemKindBitset defaultCompletionItemKinds() {
  CompletionItemKindBitset Defaults;
  for (size_t I = CompletionItemKindMin;
       I <= static_cast<size_t>(CompletionItemKind::Reference); ++I)
    Defaults.set(I);
  return Defaults;
}

class PseudoASTListener final : public FeatureModule::ASTListener {
public:
  void sawDiagnostic(const clang::Diagnostic &D, clangd::Diag &Diag) override {}
};

} // namespace

PseudoModule::PseudoModule()
    : SupportedSymbolKinds(defaultSymbolKinds()),
      SupportedCompletionItemKinds(defaultCompletionItemKinds()) {}
PseudoModule::~PseudoModule() = default;

const GlobalCompilationDatabase *PseudoModule::getCDB() const {
  if (TestCDB)
    return TestCDB;
  return nullptr;
}

llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> PseudoModule::getFS() const {
  if (TestFS)
    return TestFS->view(std::nullopt);
  if (hasFacilities())
    return const_cast<PseudoModule *>(this)->fs().view(std::nullopt);
  return llvm::vfs::getRealFileSystem();
}

std::string PseudoModule::getDocument(PathRef File) {
  if (auto Draft = DraftMgr.getDraft(File))
    return *Draft->Contents;
  if (auto FS = getFS()) {
    if (auto Buf = FS->getBufferForFile(File))
      return (*Buf)->getBuffer().str();
  }
  return "";
}

llvm::Expected<std::vector<Diagnostic>>
PseudoModule::getDiagnostics(PathRef File, llvm::StringRef Code) {
  return getDiagnostics(Code);
}

llvm::Expected<std::vector<Diagnostic>>
PseudoModule::getDiagnostics(llvm::StringRef Code) {
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());
  return clangd::getDiagnostics(*Parsed, Code);
}

llvm::Expected<std::optional<ASTNode>>
PseudoModule::getAST(PathRef File, llvm::StringRef Code,
                     std::optional<Range> R) {
  return clangd::getAST(File, Code, R);
}

static std::string encodeVersion(std::optional<int64_t> LSPVersion) {
  return LSPVersion ? std::to_string(*LSPVersion) : "";
}

void PseudoModule::updateDraft(PathRef File, llvm::StringRef Contents,
                               llvm::StringRef Version) {
  DraftMgr.addDraft(File, Version, Contents);
}

void PseudoModule::removeDraft(PathRef File) {
  DraftMgr.removeDraft(File);
}

void PseudoModule::publishDiagnosticsFor(PathRef File, llvm::StringRef Contents,
                                         llvm::StringRef Version) {
  if (!PublishDiagnostics)
    return;
  auto Diags = getDiagnostics(File, Contents);
  if (!Diags) {
    consumeError(Diags.takeError());
    return;
  }
  PublishDiagnosticsParams Notification;
  Notification.uri = URIForFile::canonicalize(File, /*TUPath=*/File);
  Notification.diagnostics = std::move(*Diags);
  if (!Version.empty()) {
    int64_t V = 0;
    if (!llvm::to_integer(Version, V))
      Notification.version = V;
  }
  PublishDiagnostics(Notification);
}

void PseudoModule::onDocumentDidOpen(const DidOpenTextDocumentParams &Params) {
  PathRef File = Params.textDocument.uri.file();
  const std::string &Contents = Params.textDocument.text;
  std::string Version = encodeVersion(Params.textDocument.version);
  DraftMgr.addDraft(File, Version, Contents);
  publishDiagnosticsFor(File, Contents, Version);
}

void PseudoModule::onDocumentDidChange(
    const DidChangeTextDocumentParams &Params) {
  PathRef File = Params.textDocument.uri.file();
  auto Draft = DraftMgr.getDraft(File);
  if (!Draft) {
    log("Trying to incrementally change non-added document: {0}", File);
    return;
  }
  std::string NewCode = *Draft->Contents;
  for (const auto &Change : Params.contentChanges) {
    if (auto Err = applyChange(NewCode, Change)) {
      DraftMgr.removeDraft(File);
      elog("Failed to update {0}: {1}", File, std::move(Err));
      return;
    }
  }
  std::string Version = encodeVersion(Params.textDocument.version);
  DraftMgr.addDraft(File, Version, NewCode);
  publishDiagnosticsFor(File, NewCode, Version);
}

void PseudoModule::onDocumentDidClose(
    const DidCloseTextDocumentParams &Params) {
  PathRef File = Params.textDocument.uri.file();
  DraftMgr.removeDraft(File);
  if (PublishDiagnostics) {
    PublishDiagnosticsParams Notification;
    Notification.uri = Params.textDocument.uri;
    Notification.diagnostics = {};
    PublishDiagnostics(Notification);
  }
}

void PseudoModule::initializeLSP(LSPBinder &Bind,
                                 const llvm::json::Object &ClientCaps,
                                 llvm::json::Object &ServerCaps) {
  HasFacilities = true;
  if (!isEnabled())
    return;

  SupportsHierarchicalDocumentSymbol = true;
  SupportsCompletionLabelDetails = true;
  HoverContentFormat = MarkupKind::PlainText;
  SupportsReferenceContainer = true;

  BaseCodeCompleteOpts.EnableSnippets = true;
  BaseCodeCompleteOpts.IncludeFixIts = true;
  BaseCodeCompleteOpts.EnableInsertReplace = true;

  PublishDiagnostics =
      Bind.outgoingNotification("textDocument/publishDiagnostics");

  Bind.notification("textDocument/didOpen", this,
                    &PseudoModule::onDocumentDidOpen);
  Bind.notification("textDocument/didChange", this,
                    &PseudoModule::onDocumentDidChange);
  Bind.notification("textDocument/didClose", this,
                    &PseudoModule::onDocumentDidClose);

  Bind.method("textDocument/definition", this,
              &PseudoModule::onGoToDefinition);
  Bind.method("textDocument/declaration", this,
              &PseudoModule::onGoToDeclaration);
  Bind.method("textDocument/references", this,
              &PseudoModule::onReference);
  Bind.method("textDocument/documentHighlight", this,
              &PseudoModule::onDocumentHighlight);
  Bind.method("textDocument/documentSymbol", this,
              &PseudoModule::onDocumentSymbol);
  Bind.method("textDocument/selectionRange", this,
              &PseudoModule::onSelectionRange);
  Bind.method("textDocument/foldingRange", this,
              &PseudoModule::onFoldingRange);
  Bind.method("textDocument/hover", this,
              &PseudoModule::onHover);
  Bind.method("textDocument/semanticTokens/full", this,
              &PseudoModule::onSemanticTokens);
  Bind.method("textDocument/semanticTokens/full/delta", this,
              &PseudoModule::onSemanticTokensDelta);
  Bind.method("textDocument/completion", this,
              &PseudoModule::onCompletion);
  Bind.method("textDocument/ast", this,
              &PseudoModule::onAST);

  Bind.method("clangd/pseudoSymbols", this,
              &PseudoModule::onCustomPseudoSymbols);
}

std::unique_ptr<FeatureModule::ASTListener> PseudoModule::astListeners() {
  return std::make_unique<PseudoASTListener>();
}

void PseudoModule::onGoToDefinition(const TextDocumentPositionParams &Params,
                                    Callback<std::vector<Location>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, Params, Reply = std::move(Reply)]() mutable {
    PathRef File = Params.textDocument.uri.file();
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::vector<Location>{});
    auto Symbols = locateSymbolAt(File, Code, Params.position);
    if (!Symbols)
      return Reply(Symbols.takeError());
    std::vector<Location> Defs;
    for (auto &S : *Symbols) {
      if (Location *Toggle = getToggle(Params, S))
        return Reply(std::vector<Location>{std::move(*Toggle)});
      Defs.push_back(S.Definition.value_or(S.PreferredDeclaration));
    }
    Reply(std::move(Defs));
  };
  if (hasFacilities())
    scheduler().run("PseudoDefinitions", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onGoToDeclaration(const TextDocumentPositionParams &Params,
                                     Callback<std::vector<Location>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, Params, Reply = std::move(Reply)]() mutable {
    PathRef File = Params.textDocument.uri.file();
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::vector<Location>{});
    auto Symbols = locateSymbolAt(File, Code, Params.position);
    if (!Symbols)
      return Reply(Symbols.takeError());
    std::vector<Location> Decls;
    for (auto &S : *Symbols) {
      if (Location *Toggle = getToggle(Params, S))
        return Reply(std::vector<Location>{std::move(*Toggle)});
      Decls.push_back(std::move(S.PreferredDeclaration));
    }
    Reply(std::move(Decls));
  };
  if (hasFacilities())
    scheduler().run("PseudoDeclarations", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onReference(
    const ReferenceParams &Params,
    Callback<std::vector<ReferenceLocation>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  bool IncludeDecl = Params.context.includeDeclaration;
  auto Action = [this, Params, IncludeDecl,
                 Reply = std::move(Reply)]() mutable {
    PathRef File = Params.textDocument.uri.file();
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::vector<ReferenceLocation>{});
    auto Refs = findReferences(File, Code, Params.position);
    if (!Refs)
      return Reply(Refs.takeError());
    std::vector<ReferenceLocation> Result;
    Result.reserve(Refs->References.size());
    for (auto &Ref : Refs->References) {
      bool IsDecl = Ref.Attributes & ReferencesResult::Declaration;
      if (IncludeDecl || !IsDecl)
        Result.push_back(std::move(Ref.Loc));
    }
    Reply(std::move(Result));
  };
  if (hasFacilities())
    scheduler().run("PseudoReferences", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onDocumentHighlight(
    const TextDocumentPositionParams &Params,
    Callback<std::vector<DocumentHighlight>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, Params, Reply = std::move(Reply)]() mutable {
    PathRef File = Params.textDocument.uri.file();
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::vector<DocumentHighlight>{});
    auto Refs = findReferences(File, Code, Params.position);
    if (!Refs)
      return Reply(Refs.takeError());
    std::vector<DocumentHighlight> Highlights;
    for (const auto &R : Refs->References) {
      DocumentHighlight DH;
      DH.range = R.Loc.range;
      DH.kind = (R.Attributes & ReferencesResult::Declaration)
                    ? DocumentHighlightKind::Write
                    : DocumentHighlightKind::Read;
      Highlights.push_back(std::move(DH));
    }
    Reply(std::move(Highlights));
  };
  if (hasFacilities())
    scheduler().run("PseudoDocumentHighlight", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onDocumentSymbol(const DocumentSymbolParams &Params,
                                    Callback<llvm::json::Value> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  URIForFile FileURI = Params.textDocument.uri;
  auto Action = [this, Params, FileURI, Reply = std::move(Reply)]() mutable {
    PathRef File = Params.textDocument.uri.file();
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::vector<DocumentSymbol>{});
    auto Items = getDocumentSymbols(Code);
    if (!Items)
      return Reply(Items.takeError());
    adjustSymbolKinds(*Items, SupportedSymbolKinds);
    if (SupportsHierarchicalDocumentSymbol)
      return Reply(std::move(*Items));
    return Reply(flattenSymbolHierarchy(*Items, FileURI));
  };
  if (hasFacilities())
    scheduler().run("PseudoDocumentSymbols", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onSelectionRange(
    const SelectionRangeParams &Params,
    Callback<std::vector<SelectionRange>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, Params, Reply = std::move(Reply)]() mutable {
    PathRef File = Params.textDocument.uri.file();
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::vector<SelectionRange>{});
    auto Ranges = getSemanticRanges(Code, Params.positions);
    if (!Ranges)
      return Reply(Ranges.takeError());
    return Reply(std::move(*Ranges));
  };
  if (hasFacilities())
    scheduler().run("PseudoSelectionRange", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onFoldingRange(
    const FoldingRangeParams &Params,
    Callback<std::vector<FoldingRange>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, Params, Reply = std::move(Reply)]() mutable {
    PathRef File = Params.textDocument.uri.file();
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::vector<FoldingRange>{});
    auto Ranges = getFoldingRanges(Code, /*LineFoldingOnly=*/false);
    if (!Ranges)
      return Reply(Ranges.takeError());
    return Reply(std::move(*Ranges));
  };
  if (hasFacilities())
    scheduler().run("PseudoFoldingRange", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onHover(const TextDocumentPositionParams &Params,
                           Callback<std::optional<Hover>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, File = Params.textDocument.uri.file().str(), Params,
                 Reply = std::move(Reply)]() mutable {
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::nullopt);
    auto H = getHover(File, Code, Params.position);
    if (!H)
      return Reply(H.takeError());
    if (*H && HoverContentFormat == MarkupKind::PlainText) {
      (*H)->contents.kind = MarkupKind::PlainText;
      llvm::StringRef Val = (*H)->contents.value;
      if (Val.starts_with("```cpp\n") && Val.ends_with("\n```"))
        (*H)->contents.value = Val.drop_front(7).drop_back(4).str();
    }
    return Reply(std::move(*H));
  };
  if (hasFacilities())
    scheduler().run("PseudoHover", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onSemanticTokens(const SemanticTokensParams &Params,
                                    Callback<SemanticTokens> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, File = Params.textDocument.uri.file().str(),
                 Reply = std::move(Reply)]() mutable {
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(SemanticTokens{});
    auto Toks = getSemanticTokens(Code);
    if (!Toks)
      return Reply(Toks.takeError());
    {
      std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
      auto &Last = LastSemanticTokens[File];
      Last.tokens = Toks->tokens;
      increment(Last.resultId);
      Toks->resultId = Last.resultId;
    }
    return Reply(std::move(*Toks));
  };
  if (hasFacilities())
    scheduler().run("PseudoSemanticTokens", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onSemanticTokensDelta(
    const SemanticTokensDeltaParams &Params,
    Callback<SemanticTokensOrDelta> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto PrevResultID = Params.previousResultId;
  auto Action = [this, File = Params.textDocument.uri.file().str(),
                 PrevResultID, Reply = std::move(Reply)]() mutable {
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(SemanticTokensOrDelta{});
    auto Toks = getSemanticTokens(Code);
    if (!Toks)
      return Reply(Toks.takeError());
    SemanticTokensOrDelta Result;
    {
      std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
      auto &Last = LastSemanticTokens[File];
      if (PrevResultID == Last.resultId) {
        Result.edits = diffTokens(Last.tokens, Toks->tokens);
      } else {
        Result.tokens = Toks->tokens;
      }
      Last.tokens = std::move(Toks->tokens);
      increment(Last.resultId);
      Result.resultId = Last.resultId;
    }
    return Reply(std::move(Result));
  };
  if (hasFacilities())
    scheduler().run("PseudoSemanticTokensDelta",
                    Params.textDocument.uri.file(), std::move(Action));
  else
    Action();
}

bool PseudoModule::shouldRunCompletion(const CompletionParams &Params) const {
  if (Params.context.triggerKind != CompletionTriggerKind::TriggerCharacter)
    return true;
  std::string Code = const_cast<PseudoModule *>(this)->getDocument(
      Params.textDocument.uri.file());
  if (Code.empty())
    return true;
  auto Offset = positionToOffset(Code, Params.position,
                                 /*AllowColumnsBeyondLineLength=*/false);
  if (!Offset) {
    elog("could not convert position '{0}' to offset for file '{1}': {2}",
         Params.position, Params.textDocument.uri.file(), Offset.takeError());
    return false;
  }
  return allowImplicitCompletion(Code, *Offset);
}

void PseudoModule::onCompletion(const CompletionParams &Params,
                                Callback<CompletionList> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  if (!shouldRunCompletion(Params)) {
    vlog("ignored auto-triggered completion, preceding char did not match");
    return Reply(CompletionList());
  }

  auto Action = [this, File = Params.textDocument.uri.file().str(), Params,
                 Reply = std::move(Reply)]() mutable {
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(CompletionList{});
    auto CompList = getCompletions(File, Code, Params.position);
    if (!CompList)
      return Reply(CompList.takeError());
    for (auto &C : CompList->items) {
      C.kind = adjustKindToCapability(C.kind, SupportedCompletionItemKinds);
      if (!SupportsCompletionLabelDetails)
        removeCompletionLabelDetails(C);
    }
    return Reply(std::move(*CompList));
  };
  if (hasFacilities())
    scheduler().run("PseudoCompletion", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

void PseudoModule::onCustomPseudoSymbols(
    const DocumentSymbolParams &Params,
    Callback<std::vector<DocumentSymbol>> Reply) {
  std::string Path = Params.textDocument.uri.file().str();
  auto FS = fs().view(std::nullopt);
  auto Buf = FS->getBufferForFile(Path);
  if (!Buf)
    return Reply(error(llvm::errc::no_such_file_or_directory,
                       "Failed to read file for pseudo parsing: " + Path));
  Reply(getDocumentSymbols((*Buf)->getBuffer()));
}

void PseudoModule::onAST(const ASTParams &Params,
                         Callback<std::optional<ASTNode>> Reply) {
  if (!isEnabled())
    return Reply(llvm::make_error<LSPError>("pseudo-parser is disabled",
                                            ErrorCode::InvalidRequest));
  auto Action = [this, File = Params.textDocument.uri.file().str(), Params,
                 Reply = std::move(Reply)]() mutable {
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::nullopt);
    auto Node = getAST(File, Code, Params.range);
    if (!Node)
      return Reply(Node.takeError());
    return Reply(std::move(*Node));
  };
  if (hasFacilities())
    scheduler().run("PseudoAST", Params.textDocument.uri.file(),
                    std::move(Action));
  else
    Action();
}

static FeatureModuleRegistry::Add<PseudoModule>
    X("pseudo-parser", "C++ pseudo-parser feature module for clangd");

} // namespace clangd
} // namespace clang
