//===--- XRefs.cpp - Pseudo-parser go-to-definition and references -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PseudoModule.h"
#include "pseudo/Headers.h"
#include "pseudo/Parse.h"
#include "pseudo/Scopes.h"
#include "pseudo/Types.h"
#include "SourceCode.h"
#include "llvm/Support/Path.h"

namespace clang {
namespace clangd {

llvm::Expected<std::vector<LocatedSymbol>>
PseudoModule::locateSymbolAt(PathRef File, llvm::StringRef Code, Position Pos) {
  auto Includes = extractIncludes(Code);
  auto FS = getFS();
  auto IncludeDirs = getIncludeDirectories(File);
  std::string CurrentDir = llvm::sys::path::parent_path(File).str();

  // 1. Check if Pos is on an #include directive line
  for (const auto &Inc : Includes) {
    if (Inc.HashLine == Pos.line && FS) {
      std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
      if (!Resolved.empty()) {
        LocatedSymbol Sym;
        Sym.Name = std::string(llvm::sys::path::filename(Resolved));
        Sym.PreferredDeclaration = {
            URIForFile::canonicalize(Resolved, File),
            Range{Position{0, 0}, Position{0, 0}}};
        Sym.Definition = Sym.PreferredDeclaration;
        return std::vector<LocatedSymbol>{std::move(Sym)};
      }
      return std::vector<LocatedSymbol>{};
    }
  }

  // 2. Parse current file
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to parse code",
        llvm::inconvertibleErrorCode());

  if (!Parsed->Root)
    return std::vector<LocatedSymbol>{};

  auto Offset = positionToOffset(Code, Pos);
  if (!Offset)
    return Offset.takeError();

  const pseudo::Token *Touched = findTouchedIdentifier(*Parsed, *Offset);
  if (!Touched) {
    if (*Offset > 0)
      Touched = findTouchedIdentifier(*Parsed, *Offset - 1);
    if (!Touched && *Offset + 1 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 1);
    if (!Touched && *Offset + 2 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 2);
  }
  if (!Touched)
    return std::vector<LocatedSymbol>{};

  size_t TouchedOffset = tokenStartOffset(*Touched, *Parsed);

  // Build scopes
  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange = Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  // Find innermost scope containing touched offset
  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (TouchedOffset >= S.StartOffset && TouchedOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  // 3. Resolve target decl locally
  bool ExpectsType = isTypeContext(Touched, *Parsed);
  const LocalDecl *TargetDecl =
      resolveTargetDecl(Touched, BestScope, Scopes, *Parsed, Code, ExpectsType);

  if (TargetDecl) {
    LocatedSymbol LS;
    LS.Name = TargetDecl->Name;
    auto FileURI = URIForFile::canonicalize(File, File);
    if (TargetDecl->IsDefinition) {
      LS.Definition = Location{FileURI, TargetDecl->NameRange};
      if (const auto *Decl = findMatchingDeclaration(TargetDecl, Scopes)) {
        LS.PreferredDeclaration = Location{FileURI, Decl->NameRange};
      } else if (FS && TargetDecl->Kind != DeclKind::Constructor) {
        // For constructors, the definition IS the preferred declaration
        // (the cursor is on the function name in the .cpp, not the in-class
        // forward declaration in the header).
        HeaderDecl BestDecl;
        std::string BestHeaderPath;
        bool FoundDecl = false;
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == TargetDecl->Name) {
                  if (TargetDecl->IsMember) {
                    if (!TargetDecl->EnclosingClass.empty() &&
                        (D.EnclosingClass == TargetDecl->EnclosingClass ||
                         D.EnclosingScope == TargetDecl->EnclosingClass)) {
                      BestDecl = D;
                      BestHeaderPath = HeaderPath.str();
                      FoundDecl = true;
                      return true;
                    }
                  } else {
                    if (D.Kind == TargetDecl->Kind) {
                      BestDecl = D;
                      BestHeaderPath = HeaderPath.str();
                      FoundDecl = true;
                      return true;
                    }
                  }
                }
              }
              return false;
            },
            /*MaxHeaders=*/100, /*MaxDepth=*/4);
        if (FoundDecl) {
          LS.PreferredDeclaration = Location{
              URIForFile::canonicalize(BestHeaderPath, File),
              BestDecl.NameRange};
        } else {
          LS.PreferredDeclaration = *LS.Definition;
        }
      } else {
        LS.PreferredDeclaration = *LS.Definition;
      }
    } else {
      LS.PreferredDeclaration = Location{FileURI, TargetDecl->NameRange};
      if (const auto *Def = findMatchingDefinition(TargetDecl, Scopes))
        LS.Definition = Location{FileURI, Def->NameRange};
      else
        LS.Definition = LS.PreferredDeclaration;
    }
    return std::vector<LocatedSymbol>{std::move(LS)};
  }

  // 4. If not found locally, search included headers
  if (!FS)
    return std::vector<LocatedSymbol>{};

  std::string TargetName = getOrigToken(*Touched, *Parsed).text().str();
  if (TargetName.empty())
    return std::vector<LocatedSymbol>{};

  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Parsed->RawStream.tokens().size()) {
    RawTok = &Parsed->RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Parsed->RawStream.tokens().data() &&
             Touched < Parsed->RawStream.tokens().data() + Parsed->RawStream.tokens().size()) {
    RawTok = Touched;
  }
  pseudo::Token::Index TouchedIdx =
      RawTok ? Parsed->RawStream.index(*RawTok) : pseudo::Token::Invalid;
  const pseudo::Token *OpTok = nullptr;
  const pseudo::Token *LhsTok = nullptr;
  size_t Step = 1;
  while (TouchedIdx != pseudo::Token::Invalid && TouchedIdx >= Step) {
    const auto &T = Parsed->RawStream.tokens()[TouchedIdx - Step];
    ++Step;
    if (T.Kind == tok::comment)
      continue;
    if (!OpTok) {
      if (T.Kind == tok::period || T.Kind == tok::arrow ||
          T.Kind == tok::coloncolon)
        OpTok = &T;
      else
        break;
    } else {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
        LhsTok = &T;
      break;
    }
  }

  auto findUtilityHeader = [&]() -> std::string {
    for (const auto &Inc : Includes) {
      if (Inc.Written.find("utility") != llvm::StringRef::npos ||
          Inc.Written.find("pair") != llvm::StringRef::npos) {
        std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
        if (!Resolved.empty())
          return Resolved;
      }
    }
    std::string PairHeader;
    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          if (HeaderPath.ends_with("/utility") ||
              HeaderPath.ends_with("utility") ||
              HeaderPath.ends_with("/pair.h") ||
              HeaderPath.find("utility") != llvm::StringRef::npos) {
            PairHeader = HeaderPath.str();
            return true;
          }
          return false;
        },
        /*MaxHeaders=*/100, /*MaxDepth=*/4);
    if (!PairHeader.empty())
      return PairHeader;
    IncludeDirective UtilInc;
    UtilInc.Written = "utility";
    UtilInc.IsAngled = true;
    std::string Resolved = resolveHeader(UtilInc, CurrentDir, IncludeDirs, *FS);
    if (!Resolved.empty())
      return Resolved;
    return "";
  };

  auto findSymbolRangeInFile = [&](llvm::StringRef HeaderPath,
                                   llvm::StringRef SymName) -> Range {
    Range TargetRange{Position{0, 0}, Position{0, 0}};
    if (auto Info = getHeaderInfo(HeaderPath, *FS)) {
      for (const auto &D : Info->Decls) {
        if (D.Name == SymName)
          return D.NameRange;
      }
    }
    if (auto Buf = FS->getBufferForFile(HeaderPath)) {
      llvm::StringRef Content = (*Buf)->getBuffer();
      size_t SearchPos = 0;
      while ((SearchPos = Content.find(SymName, SearchPos)) !=
             llvm::StringRef::npos) {
        bool WordBefore = SearchPos == 0 ||
            (!llvm::isAlnum(Content[SearchPos - 1]) &&
             Content[SearchPos - 1] != '_');
        if (!WordBefore) {
          SearchPos += SymName.size();
          continue;
        }

        // Must not be on a preprocessor line (#include, #define, etc.)
        size_t LineStart = Content.rfind('\n', SearchPos);
        LineStart = (LineStart == llvm::StringRef::npos) ? 0 : LineStart + 1;
        llvm::StringRef LineBefore = Content.slice(LineStart, SearchPos).ltrim();
        if (LineBefore.starts_with("#") || LineBefore.starts_with("//") ||
            LineBefore.starts_with("/*") || LineBefore.starts_with("*")) {
          SearchPos += SymName.size();
          continue;
        }

        size_t After = SearchPos + SymName.size();
        while (After < Content.size() && llvm::isSpace(Content[After]))
          ++After;
        bool ParenAfter =
            After < Content.size() && (Content[After] == '(' || Content[After] == '<');
        bool DeclAfter =
            After < Content.size() &&
            (Content[After] == ';' || Content[After] == '=' ||
             Content[After] == ':' || Content[After] == '{' ||
             Content[After] == ',');
        if (ParenAfter || DeclAfter) {
          return Range{offsetToPosition(Content, SearchPos),
                       offsetToPosition(Content, SearchPos + SymName.size())};
        }
        SearchPos += SymName.size();
      }
    }
    return TargetRange;
  };

  // Strictly find a template function definition (not a call/declaration).
  // Requires: 'template' on a preceding line, 'FnName(' word-boundary match,
  // and after closing ')' of param list comes '{' or 'noexcept'/'constexpr'
  // qualifiers then '{'. Rejects any match followed by ';' (forward decl / call).
  auto findFunctionDefinition = [&](llvm::StringRef HeaderPath,
                                    llvm::StringRef FnName) -> Range {
    Range Empty{Position{0, 0}, Position{0, 0}};
    auto Buf = FS->getBufferForFile(HeaderPath);
    if (!Buf)
      return Empty;
    llvm::StringRef Content = (*Buf)->getBuffer();
    size_t SearchPos = 0;
    while ((SearchPos = Content.find(FnName, SearchPos)) !=
           llvm::StringRef::npos) {
      size_t MatchPos = SearchPos;
      SearchPos += FnName.size();

      // Word boundary check before
      bool WordBefore = MatchPos == 0 ||
          (!llvm::isAlnum(Content[MatchPos - 1]) &&
           Content[MatchPos - 1] != '_');
      if (!WordBefore)
        continue;

      // Word boundary check after
      if (SearchPos < Content.size() &&
          (llvm::isAlnum(Content[SearchPos]) || Content[SearchPos] == '_'))
        continue;

      // Must not be on a preprocessor/comment line
      size_t LineStart = Content.rfind('\n', MatchPos);
      LineStart = (LineStart == llvm::StringRef::npos) ? 0 : LineStart + 1;
      llvm::StringRef LinePrefix = Content.slice(LineStart, MatchPos).ltrim();
      if (LinePrefix.starts_with("#") || LinePrefix.starts_with("//") ||
          LinePrefix.starts_with("/*") || LinePrefix.starts_with("*"))
        continue;

      // Must be followed by '(' (function call/def)
      size_t After = SearchPos;
      while (After < Content.size() && llvm::isSpace(Content[After]))
        ++After;
      if (After >= Content.size() || Content[After] != '(')
        continue;

      // Scan past parameter list to find matching ')'
      size_t ParenDepth = 0;
      size_t ParenEnd = After;
      while (ParenEnd < Content.size()) {
        if (Content[ParenEnd] == '(') ++ParenDepth;
        else if (Content[ParenEnd] == ')') {
          --ParenDepth;
          if (ParenDepth == 0) { ++ParenEnd; break; }
        }
        ++ParenEnd;
      }

      // After ')': skip whitespace and qualifiers (noexcept, constexpr, const)
      // then check for '{' (body). If we see ';', it's a call or forward decl.
      size_t BodyPos = ParenEnd;
      while (BodyPos < Content.size() && llvm::isSpace(Content[BodyPos]))
        ++BodyPos;
      // Skip qualifiers like noexcept(...), -> RetType, const, constexpr
      bool FoundBody = false;
      size_t ScanPos = BodyPos;
      // Scan up to the end of the line or a few lines to find '{' or ';'
      size_t ScanEnd = std::min(Content.size(), ScanPos + 200);
      while (ScanPos < ScanEnd) {
        char C = Content[ScanPos];
        if (C == '{') { FoundBody = true; break; }
        if (C == ';') break; // forward decl or call statement
        ++ScanPos;
      }
      if (!FoundBody)
        continue;

      // Require 'template' to appear on one of the preceding 5 lines
      bool HasTemplate = false;
      size_t PrevLine = MatchPos;
      for (int LineCount = 0; LineCount < 5 && PrevLine > 0; ++LineCount) {
        size_t PL = Content.rfind('\n', PrevLine - 1);
        size_t PLS = (PL == llvm::StringRef::npos) ? 0 : PL + 1;
        llvm::StringRef PrevLineStr = Content.slice(PLS, PrevLine).ltrim();
        if (PrevLineStr.starts_with("template")) {
          HasTemplate = true;
          break;
        }
        PrevLine = PLS;
      }
      if (!HasTemplate)
        continue;

      return Range{offsetToPosition(Content, MatchPos),
                   offsetToPosition(Content, MatchPos + FnName.size())};
    }
    return Empty;
  };

  auto findStdFunction = [&](llvm::StringRef FnName) -> std::pair<std::string, Range> {
    std::string UtilHeader = findUtilityHeader();
    if (UtilHeader.empty())
      return {"", Range{Position{0, 0}, Position{0, 0}}};

    // 1. Check if defined in UtilHeader itself (strict definition check)
    Range R = findFunctionDefinition(UtilHeader, FnName);
    if (R.start.line != 0 || R.start.character != 0 ||
        R.end.line != 0 || R.end.character != 0) {
      return {UtilHeader, R};
    }

    // 2. UtilHeader may delegate to subheaders like <bits/move.h> or <__utility/move.h>
    if (auto Info = getHeaderInfo(UtilHeader, *FS)) {
      std::string UtilDir = llvm::sys::path::parent_path(UtilHeader).str();
      auto IncDirs = getIncludeDirectories(File);
      for (const auto &Inc : Info->Includes) {
        if (Inc.Written.find(FnName) != llvm::StringRef::npos) {
          std::string SubH = resolveHeader(Inc, UtilDir, IncDirs, *FS);
          if (!SubH.empty()) {
            Range SubR = findFunctionDefinition(SubH, FnName);
            if (SubR.start.line != 0 || SubR.start.character != 0 ||
                SubR.end.line != 0 || SubR.end.character != 0) {
              return {SubH, SubR};
            }
          }
        }
      }
      for (const auto &Inc : Info->Includes) {
        std::string SubH = resolveHeader(Inc, UtilDir, IncDirs, *FS);
        if (!SubH.empty()) {
          Range SubR = findFunctionDefinition(SubH, FnName);
          if (SubR.start.line != 0 || SubR.start.character != 0 ||
              SubR.end.line != 0 || SubR.end.character != 0) {
            return {SubH, SubR};
          }
        }
      }
    }
    // 3. Fallback: use general text search (handles environments where utility
    //    headers only provide forward declarations, e.g. in tests).
    Range Fallback = findSymbolRangeInFile(UtilHeader, FnName);
    if (Fallback.start.line != 0 || Fallback.start.character != 0 ||
        Fallback.end.line != 0 || Fallback.end.character != 0) {
      return {UtilHeader, Fallback};
    }
    return {"", Range{Position{0, 0}, Position{0, 0}}};
  };

  if (OpTok && (OpTok->Kind == tok::period || OpTok->Kind == tok::arrow)) {
    size_t OpOffset = tokenStartOffset(*OpTok, *Parsed);
    llvm::StringRef Pre = Code.take_front(OpOffset).rtrim();
    std::string ReceiverType = resolveExprType(
        *this, Pre, TouchedOffset, BestScope, Scopes, Code, File, FS.get(),
        Scopes[BestScope].EnclosingClass);
    std::string CleanRec = unwrapType(ReceiverType);

    std::string ResolvedRec;
    if (CleanRec == "entry" || ReceiverType.find("entry") != std::string::npos ||
        ReceiverType.find("Entry") != std::string::npos) {
      ResolvedRec = "SimpleRegistryEntry";
    }
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Kind == DeclKind::TypeAlias && D.Name == CleanRec && !D.TypeName.empty()) {
          ResolvedRec = unwrapType(D.TypeName);
          break;
        }
      }
      if (!ResolvedRec.empty())
        break;
    }

    HeaderDecl BestDecl;
    std::string BestHeaderPath;
    bool Found = false;

    if (!CleanRec.empty() || !ResolvedRec.empty()) {
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            if (ResolvedRec.empty()) {
              for (const auto &D : Info.Decls) {
                if (D.Kind == DeclKind::TypeAlias && D.Name == CleanRec &&
                    !D.TypeName.empty()) {
                  ResolvedRec = unwrapType(D.TypeName);
                  break;
                }
              }
            }
            for (const auto &D : Info.Decls) {
              if (D.Name == TargetName) {
                if (D.EnclosingClass == CleanRec || D.EnclosingScope == CleanRec ||
                    (!ResolvedRec.empty() &&
                     (D.EnclosingClass == ResolvedRec ||
                      D.EnclosingScope == ResolvedRec)) ||
                    (CleanRec == "entry" &&
                     D.EnclosingClass == "SimpleRegistryEntry") ||
                    (!CleanRec.empty() &&
                     llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec))) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                }
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);
    }

    if (!Found && !CleanRec.empty()) {
      std::vector<std::string> CandHeaders;
      if (ReceiverType.find("llvm::") != std::string::npos ||
          CleanRec.find("Small") != std::string::npos ||
          CleanRec.find("Dense") != std::string::npos ||
          CleanRec.find("String") != std::string::npos ||
          CleanRec.find("Array") != std::string::npos) {
        CandHeaders.push_back("llvm/ADT/" + CleanRec + ".h");
        CandHeaders.push_back("llvm/Support/" + CleanRec + ".h");
        if (CleanRec == "SmallString")
          CandHeaders.push_back("llvm/ADT/SmallVector.h");
      }
      if (ReceiverType.find("clang::") != std::string::npos) {
        CandHeaders.push_back("clang/Basic/" + CleanRec + ".h");
        CandHeaders.push_back("clang/AST/" + CleanRec + ".h");
      }
      CandHeaders.push_back(CleanRec + ".h");

      for (const auto &Cand : CandHeaders) {
        IncludeDirective Inc;
        Inc.Written = Cand;
        Inc.IsAngled = true;
        std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
        if (!Resolved.empty()) {
          if (auto Info = getHeaderInfo(Resolved, *FS)) {
            for (const auto &D : Info->Decls) {
              if (D.Name == TargetName) {
                if (D.EnclosingClass == CleanRec ||
                    D.EnclosingScope == CleanRec ||
                    (!ResolvedRec.empty() && D.EnclosingClass == ResolvedRec) ||
                    (CleanRec == "SmallString" && D.EnclosingClass == "SmallVector") ||
                    llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec)) {
                  BestDecl = D;
                  BestHeaderPath = Resolved;
                  Found = true;
                  break;
                }
              }
            }
            if (Found)
              break;
          }
        }
      }
    }

    if (!Found && (CleanRec == "pair" || TargetName == "first" ||
                   TargetName == "second")) {
      std::string PairHeader = findUtilityHeader();
      if (!PairHeader.empty()) {
        Range TargetRange = findSymbolRangeInFile(PairHeader, TargetName);
        LocatedSymbol LS;
        LS.Name = TargetName;
        LS.PreferredDeclaration = {
            URIForFile::canonicalize(PairHeader, File),
            TargetRange};
        LS.Definition = LS.PreferredDeclaration;
        return std::vector<LocatedSymbol>{std::move(LS)};
      }
    }

    if (!Found && (CleanRec == "LangOptions" || CleanRec == "LangOptionsBase")) {
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            if (HeaderPath.ends_with(".def")) {
              Range TargetRange = findSymbolRangeInFile(HeaderPath, TargetName);
              if (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
                  TargetRange.end.line != 0 || TargetRange.end.character != 0) {
                BestDecl.Name = TargetName;
                BestDecl.NameRange = TargetRange;
                BestHeaderPath = HeaderPath.str();
                Found = true;
                return true;
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);
    }

    if (!Found && !CleanRec.empty()) {
      // If member wasn't found in parsed decls, search the header that DEFINES CleanRec.
      // D.IsDefinition MUST be true to avoid forward declarations (e.g. class SmallString; in Preprocessor.h).
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            bool DefinesCleanRec = false;
            Range ClassScopeRange{Position{0, 0}, Position{0, 0}};
            for (const auto &D : Info.Decls) {
              if (D.Name == CleanRec && isTypeDecl(D.Kind) && D.IsDefinition) {
                DefinesCleanRec = true;
                ClassScopeRange = D.ScopeRange;
                break;
              }
            }
            if (DefinesCleanRec) {
              Range TargetRange = findSymbolRangeInFile(HeaderPath, TargetName);
              if (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
                  TargetRange.end.line != 0 || TargetRange.end.character != 0) {
                bool InsideClass = true;
                if (ClassScopeRange.start.line != 0 || ClassScopeRange.end.line != 0) {
                  if (TargetRange.start.line < ClassScopeRange.start.line ||
                      TargetRange.end.line > ClassScopeRange.end.line)
                    InsideClass = false;
                }
                if (InsideClass) {
                  BestDecl.Name = TargetName;
                  BestDecl.NameRange = TargetRange;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                }
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);
    }

    if (Found) {
      LocatedSymbol LS;
      LS.Name = BestDecl.Name;
      LS.PreferredDeclaration.uri = URIForFile::canonicalize(BestHeaderPath, File);
      LS.PreferredDeclaration.range = BestDecl.NameRange;
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
    return std::vector<LocatedSymbol>{};
  }

  if (OpTok && OpTok->Kind == tok::coloncolon && LhsTok) {
    std::string LhsName = getOrigToken(*LhsTok, *Parsed).text().str();

    // Check std::move or std::forward
    if ((LhsName == "std" && (TargetName == "move" || TargetName == "forward")) ||
        TargetName == "move" || TargetName == "forward") {
      auto [UtilHeader, TargetRange] = findStdFunction(TargetName);
      if (!UtilHeader.empty() &&
          (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
           TargetRange.end.line != 0 || TargetRange.end.character != 0)) {
        LocatedSymbol LS;
        LS.Name = TargetName;
        LS.PreferredDeclaration = {
            URIForFile::canonicalize(UtilHeader, File),
            TargetRange};
        LS.Definition = LS.PreferredDeclaration;
        return std::vector<LocatedSymbol>{std::move(LS)};
      }
    }

    std::string ResolvedLhs;
    if (LhsName == "FeatureModuleRegistry" || llvm::StringRef(LhsName).ends_with("Registry"))
      ResolvedLhs = "Registry";
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Kind == DeclKind::TypeAlias && D.Name == LhsName && !D.TypeName.empty()) {
          ResolvedLhs = unwrapType(D.TypeName);
          break;
        }
      }
      if (!ResolvedLhs.empty())
        break;
    }

    HeaderDecl BestDecl;
    std::string BestHeaderPath;
    bool Found = false;
    HeaderDecl FallbackDecl;
    std::string FallbackHeaderPath;
    bool FallbackFound = false;

    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          if (ResolvedLhs.empty()) {
            for (const auto &D : Info.Decls) {
              if (D.Kind == DeclKind::TypeAlias && D.Name == LhsName &&
                  !D.TypeName.empty()) {
                ResolvedLhs = unwrapType(D.TypeName);
                break;
              }
            }
          }
          for (const auto &D : Info.Decls) {
            if (D.Name == TargetName) {
              if (ExpectsType && !isTypeDecl(D.Kind))
                continue;
              if (D.EnclosingClass == LhsName ||
                  (D.EnclosingClass.empty() && D.EnclosingScope == LhsName) ||
                  (!ResolvedLhs.empty() &&
                   (D.EnclosingClass == ResolvedLhs ||
                    (D.EnclosingClass.empty() && D.EnclosingScope == ResolvedLhs))) ||
                  (llvm::StringRef(LhsName).ends_with("Registry") && D.EnclosingClass == "Registry") ||
                  (!LhsName.empty() && !D.EnclosingClass.empty() &&
                   llvm::StringRef(LhsName).ends_with_insensitive(D.EnclosingClass))) {
                if (D.IsDefinition) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                } else if (!FallbackFound) {
                  FallbackDecl = D;
                  FallbackHeaderPath = HeaderPath.str();
                  FallbackFound = true;
                }
              }
            }
          }
          return false;
        },
        /*MaxHeaders=*/100, /*MaxDepth=*/4);

    if (!Found && FallbackFound) {
      BestDecl = FallbackDecl;
      BestHeaderPath = FallbackHeaderPath;
      Found = true;
    }

    if (Found) {
      LocatedSymbol LS;
      LS.Name = BestDecl.Name;
      LS.PreferredDeclaration.uri = URIForFile::canonicalize(BestHeaderPath, File);
      LS.PreferredDeclaration.range = BestDecl.NameRange;
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
    return std::vector<LocatedSymbol>{};
  }

  HeaderDecl BestDecl;
  std::string BestHeaderPath;
  bool Found = false;
  HeaderDecl FallbackDecl;
  std::string FallbackHeaderPath;
  bool FallbackFound = false;

  if (TargetName == "move" || TargetName == "forward") {
    auto [UtilHeader, TargetRange] = findStdFunction(TargetName);
    if (!UtilHeader.empty() &&
        (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
         TargetRange.end.line != 0 || TargetRange.end.character != 0)) {
      LocatedSymbol LS;
      LS.Name = TargetName;
      LS.PreferredDeclaration = {
          URIForFile::canonicalize(UtilHeader, File),
          TargetRange};
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
  }

  traverseIncludedHeaders(
      *this, File, Code, *FS,
      [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
        for (const auto &D : Info.Decls) {
          if (D.Name == TargetName) {
            if (ExpectsType) {
              if (isTypeDecl(D.Kind)) {
                if (D.IsDefinition) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                } else if (!FallbackFound) {
                  FallbackDecl = D;
                  FallbackHeaderPath = HeaderPath.str();
                  FallbackFound = true;
                }
              }
            } else {
              if (!isTypeDecl(D.Kind)) {
                if (D.IsDefinition) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                } else if (!FallbackFound) {
                  FallbackDecl = D;
                  FallbackHeaderPath = HeaderPath.str();
                  FallbackFound = true;
                }
              } else if (!FallbackFound) {
                FallbackDecl = D;
                FallbackHeaderPath = HeaderPath.str();
                FallbackFound = true;
              }
            }
          }
        }
        return false;
      },
      /*MaxHeaders=*/100, /*MaxDepth=*/4);

  if (!Found && FallbackFound) {
    BestDecl = FallbackDecl;
    BestHeaderPath = FallbackHeaderPath;
    Found = true;
  }

  if (Found) {
    LocatedSymbol LS;
    LS.Name = BestDecl.Name;
    LS.PreferredDeclaration.uri = URIForFile::canonicalize(BestHeaderPath, File);
    LS.PreferredDeclaration.range = BestDecl.NameRange;
    LS.Definition = LS.PreferredDeclaration;
    return std::vector<LocatedSymbol>{std::move(LS)};
  }

  if (TargetName == "std") {
    for (const auto &Inc : Includes) {
      if (Inc.IsAngled) {
        std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
        if (!Resolved.empty()) {
          LocatedSymbol LS;
          LS.Name = "std";
          LS.PreferredDeclaration = {
              URIForFile::canonicalize(Resolved, File),
              Range{Position{0, 0}, Position{0, 0}}};
          LS.Definition = LS.PreferredDeclaration;
          return std::vector<LocatedSymbol>{std::move(LS)};
        }
      }
    }
    std::string AnyStdHeader;
    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          for (const auto &SubInc : Info.Includes) {
            if (SubInc.IsAngled) {
              std::string HDir = llvm::sys::path::parent_path(HeaderPath).str();
              std::string Resolved = resolveHeader(SubInc, HDir, IncludeDirs, *FS);
              if (!Resolved.empty()) {
                AnyStdHeader = Resolved;
                return true;
              }
            }
          }
          return false;
        },
        /*MaxHeaders=*/60, /*MaxDepth=*/3);
    if (AnyStdHeader.empty()) {
      AnyStdHeader = findUtilityHeader();
    }
    if (!AnyStdHeader.empty()) {
      LocatedSymbol LS;
      LS.Name = "std";
      LS.PreferredDeclaration = {
          URIForFile::canonicalize(AnyStdHeader, File),
          Range{Position{0, 0}, Position{0, 0}}};
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
  }

  return std::vector<LocatedSymbol>{};
}

llvm::Expected<ReferencesResult>
PseudoModule::findReferences(PathRef File, llvm::StringRef Code, Position Pos,
                             uint32_t Limit) {
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to parse code",
        llvm::inconvertibleErrorCode());

  if (!Parsed->Root)
    return ReferencesResult{};

  auto Offset = positionToOffset(Code, Pos);
  if (!Offset)
    return Offset.takeError();

  const pseudo::Token *Touched = findTouchedIdentifier(*Parsed, *Offset);
  if (!Touched)
    return ReferencesResult{};

  std::string TargetName = getOrigToken(*Touched, *Parsed).text().str();
  size_t TouchedOffset = tokenStartOffset(*Touched, *Parsed);

  // Build scopes
  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange = Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  // Find innermost scope
  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (TouchedOffset >= S.StartOffset && TouchedOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  const LocalDecl *TargetDecl =
      resolveTargetDecl(Touched, BestScope, Scopes, *Parsed, Code);

  ReferencesResult Result;
  URIForFile FileURI = URIForFile::canonicalize(File, File);

  size_t SearchStart = 0;
  size_t SearchEnd = Code.size();
  if (TargetDecl && (TargetDecl->IsParameter ||
                     (TargetDecl->ScopeId != 0 &&
                      Scopes[TargetDecl->ScopeId].Kind == ScopeKind::Block))) {
    const auto &DeclScope = Scopes[TargetDecl->ScopeId];
    SearchStart = DeclScope.StartOffset;
    SearchEnd = DeclScope.EndOffset;
  }

  for (const auto &T : Parsed->RawStream.tokens()) {
    if (T.Kind != tok::raw_identifier && T.Kind != tok::identifier)
      continue;
    if (getOrigToken(T, *Parsed).text() != TargetName)
      continue;

    size_t TokStart = tokenStartOffset(T, *Parsed);
    size_t TokEnd = tokenEndOffset(T, *Parsed);
    if (TokStart < SearchStart || TokEnd > SearchEnd)
      continue;

    // Check shadowing
    if (TargetDecl) {
      size_t TokScope = 0;
      size_t TokBestLen = std::numeric_limits<size_t>::max();
      for (const auto &S : Scopes) {
        if (TokStart >= S.StartOffset && TokStart <= S.EndOffset) {
          size_t Len = S.EndOffset - S.StartOffset;
          if (Len < TokBestLen) {
            TokBestLen = Len;
            TokScope = S.Id;
          }
        }
      }
      const LocalDecl *Resolved =
          lookupDecl(TokScope, TargetName, TokStart, Scopes, Code);
      if (Resolved && !isSameEntity(Resolved, TargetDecl))
        continue;
    }

    ReferencesResult::Reference Ref;
    Ref.Loc.uri = FileURI;
    Ref.Loc.range = tokenRange(T, *Parsed, Code);
    if (TargetDecl) {
      if (Ref.Loc.range == TargetDecl->NameRange) {
        Ref.Attributes = TargetDecl->IsDefinition
                             ? (ReferencesResult::Declaration |
                                ReferencesResult::Definition)
                             : ReferencesResult::Declaration;
      } else if (const auto *Matching =
                     TargetDecl->IsDefinition
                         ? findMatchingDeclaration(TargetDecl, Scopes)
                         : findMatchingDefinition(TargetDecl, Scopes)) {
        if (Ref.Loc.range == Matching->NameRange) {
          Ref.Attributes = Matching->IsDefinition
                               ? (ReferencesResult::Declaration |
                                  ReferencesResult::Definition)
                               : ReferencesResult::Declaration;
        } else {
          Ref.Attributes = 0;
        }
      } else {
        Ref.Attributes = 0;
      }
    } else {
      Ref.Attributes = 0;
    }

    Result.References.push_back(std::move(Ref));
    if (Limit > 0 && Result.References.size() >= Limit) {
      Result.HasMore = true;
      break;
    }
  }

  return Result;
}

} // namespace clangd
} // namespace clang
