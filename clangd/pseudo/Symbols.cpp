//===--- Symbols.cpp - Document symbols, folding, and selection ranges -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PseudoModule.h"
#include "pseudo/Parse.h"
#include "SourceCode.h"

namespace clang {
namespace clangd {

void walkSymbols(const pseudo::ForestNode *N, pseudo::Token::Index End,
                 const ParseOutput &Out, llvm::StringRef Code,
                 std::vector<DocumentSymbol> &Results,
                 bool InsideClass = false,
                 llvm::StringRef EnclosingClassName = "") {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    walkSymbols(Alts[AltIdx], End, Out, Code, Results, InsideClass,
                EnclosingClassName);
    return;
  }

  if (N->kind() != pseudo::ForestNode::Sequence)
    return;

  pseudo::SymbolID Sym = N->symbol();
  auto StartTok = N->startTokenIndex();
  auto EndTok = End;

  if (StartTok >= EndTok || StartTok >= Out.ParseableStream.tokens().size())
    return;

  auto NodeTokens =
      Out.ParseableStream.tokens().slice(StartTok, EndTok - StartTok);

  // 1. Named namespace
  if (Sym == pseudo::cxx::Symbol::named_namespace_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Namespace;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    // Find identifier after namespace keyword
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_namespace && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          DS.name = getOrigToken(IdTok, Out).text().str();
          DS.selectionRange = tokenRange(IdTok, Out, Code);
          break;
        }
      }
    }
    if (DS.name.empty()) {
      DS.name = "(namespace)";
      DS.selectionRange = DS.range;
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, false, "");
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 2. Unnamed namespace
  if (Sym == pseudo::cxx::Symbol::unnamed_namespace_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Namespace;
    DS.name = "(anonymous namespace)";
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    DS.selectionRange =
        NodeTokens.empty() ? DS.range : tokenRange(NodeTokens.front(), Out, Code);
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, false, "");
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 3. Nested namespace definition (e.g. namespace a::b)
  if (Sym == pseudo::cxx::Symbol::nested_namespace_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Namespace;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    std::string Name;
    Range SelRange = DS.range;
    bool FoundStart = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::kw_namespace) {
        FoundStart = true;
        continue;
      }
      if (FoundStart) {
        if (!Name.empty())
          Name += NodeTokens[I].Kind == tok::coloncolon ? "::" : "";
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          Name += getOrigToken(NodeTokens[I], Out).text().str();
          SelRange = tokenRange(NodeTokens[I], Out, Code);
        }
      }
    }
    DS.name = Name.empty() ? "(namespace)" : Name;
    DS.selectionRange = SelRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, false, "");
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 4. Class / Struct / Union specifier
  if (Sym == pseudo::cxx::Symbol::class_specifier) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Class;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);

    // Determine struct/class/union and find name
    const pseudo::Token *NameTok = nullptr;
    const pseudo::Token *KeyTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::kw_struct) {
        DS.kind = SymbolKind::Struct;
        KeyTok = &NodeTokens[I];
      } else if (NodeTokens[I].Kind == tok::kw_class) {
        DS.kind = SymbolKind::Class;
        KeyTok = &NodeTokens[I];
      } else if (NodeTokens[I].Kind == tok::kw_union) {
        DS.kind = SymbolKind::Class;
        KeyTok = &NodeTokens[I];
      } else if (NodeTokens[I].Kind == tok::raw_identifier ||
                 NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }

    if (NameTok) {
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
    } else {
      DS.name = (DS.kind == SymbolKind::Struct) ? "(anonymous struct)"
                                                : "(anonymous class)";
      DS.selectionRange =
          KeyTok ? tokenRange(*KeyTok, Out, Code) : DS.range;
    }

    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, true, DS.name);
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 5. Function definition
  if (Sym == pseudo::cxx::Symbol::function_definition) {
    DocumentSymbol DS;
    DS.kind = InsideClass ? SymbolKind::Method : SymbolKind::Function;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);

    // Find function name: scan backwards before parameter list '('
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_paren) {
        // Find identifier preceding '('
        for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
          if (NodeTokens[J].Kind == tok::raw_identifier ||
              NodeTokens[J].Kind == tok::identifier) {
            NameTok = &NodeTokens[J];
            break;
          }
        }
        break;
      }
    }

    if (NameTok) {
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
      if (InsideClass && DS.name == EnclosingClassName)
        DS.kind = SymbolKind::Constructor;
      else if (InsideClass && DS.name.rfind('~') != std::string::npos)
        DS.kind = SymbolKind::Constructor;
    } else {
      DS.name = "(function)";
      DS.selectionRange = DS.range;
    }

    Results.push_back(std::move(DS));
    return;
  }

  // 6. Enum specifier
  if (Sym == pseudo::cxx::Symbol::enum_specifier) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Enum;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);

    const pseudo::Token *NameTok = nullptr;
    const pseudo::Token *EnumTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::kw_enum)
        EnumTok = &NodeTokens[I];
      else if (NodeTokens[I].Kind == tok::raw_identifier ||
               NodeTokens[I].Kind == tok::identifier)
        NameTok = &NodeTokens[I];
    }

    if (NameTok) {
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
    } else {
      DS.name = "(anonymous enum)";
      DS.selectionRange =
          EnumTok ? tokenRange(*EnumTok, Out, Code) : DS.range;
    }

    // Collect enumerator members inside braces
    bool InBraces = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace) {
        InBraces = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::r_brace)
        break;
      if (InBraces && (NodeTokens[I].Kind == tok::raw_identifier ||
                       NodeTokens[I].Kind == tok::identifier)) {
        DocumentSymbol Member;
        Member.kind = SymbolKind::EnumMember;
        Member.name = getOrigToken(NodeTokens[I], Out).text().str();
        Member.selectionRange = tokenRange(NodeTokens[I], Out, Code);
        Member.range = Member.selectionRange;
        DS.children.push_back(std::move(Member));
        // Skip till next comma
        while (I + 1 < NodeTokens.size() &&
               NodeTokens[I + 1].Kind != tok::comma &&
               NodeTokens[I + 1].Kind != tok::r_brace)
          ++I;
      }
    }

    Results.push_back(std::move(DS));
    return;
  }

  // 7. Alias declaration (using X = Y;)
  if (Sym == pseudo::cxx::Symbol::alias_declaration) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Class;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_using && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          DS.name = getOrigToken(IdTok, Out).text().str();
          DS.selectionRange = tokenRange(IdTok, Out, Code);
          break;
        }
      }
    }
    if (!DS.name.empty()) {
      Results.push_back(std::move(DS));
      return;
    }
  }

  // 8. Concept definition (concept X = ...;)
  if (Sym == pseudo::cxx::Symbol::concept_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Interface;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_concept && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          DS.name = getOrigToken(IdTok, Out).text().str();
          DS.selectionRange = tokenRange(IdTok, Out, Code);
          break;
        }
      }
    }
    if (!DS.name.empty()) {
      Results.push_back(std::move(DS));
      return;
    }
  }

  // 9. Init declarator (variable or function declaration outside class)
  // or Member declarator (field or method declaration inside class)
  if (Sym == pseudo::cxx::Symbol::init_declarator ||
      Sym == pseudo::cxx::Symbol::member_declarator) {
    const pseudo::Token *NameTok = nullptr;
    bool HasParen = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
          NodeTokens[I].Kind == tok::colon || NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::l_paren)
        HasParen = true;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (!HasParen)
          NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      DocumentSymbol DS;
      if (InsideClass)
        DS.kind = HasParen ? SymbolKind::Method : SymbolKind::Field;
      else
        DS.kind = HasParen ? SymbolKind::Function : SymbolKind::Variable;
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
      DS.range = nodeRange(StartTok, EndTok, Out, Code);
      Results.push_back(std::move(DS));
      return;
    }
  }

  // Recurse down children
  auto Children = N->elements();
  for (size_t I = 0; I < Children.size(); ++I) {
    pseudo::Token::Index ChildEnd =
        (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
    walkSymbols(Children[I], ChildEnd, Out, Code, Results, InsideClass,
                EnclosingClassName);
  }
}

void collectSelectionRanges(const pseudo::ForestNode *N,
                            pseudo::Token::Index End, const ParseOutput &Out,
                            llvm::StringRef Code, size_t Offset,
                            std::vector<Range> &Path) {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    collectSelectionRanges(Alts[AltIdx], End, Out, Code, Offset, Path);
    return;
  }

  auto StartTok = N->startTokenIndex();
  if (StartTok >= End || StartTok >= Out.ParseableStream.tokens().size())
    return;

  size_t StartOff =
      tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
  size_t EndOff =
      tokenEndOffset(Out.ParseableStream.tokens()[End - 1], Out);
  if (Offset < StartOff || Offset > EndOff)
    return;

  Range R = nodeRange(StartTok, End, Out, Code);
  if (Path.empty() || Path.back() != R)
    Path.push_back(R);

  if (N->kind() == pseudo::ForestNode::Sequence) {
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      collectSelectionRanges(Children[I], ChildEnd, Out, Code, Offset, Path);
    }
  }
}

void collectFoldingRanges(const pseudo::ForestNode *N,
                         pseudo::Token::Index End, const ParseOutput &Out,
                         llvm::StringRef Code,
                         std::vector<FoldingRange> &Result) {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    collectFoldingRanges(Alts[AltIdx], End, Out, Code, Result);
    return;
  }

  if (N->kind() == pseudo::ForestNode::Sequence) {
    auto StartTok = N->startTokenIndex();
    if (StartTok < End && End <= Out.ParseableStream.tokens().size()) {
      auto Tokens = Out.ParseableStream.tokens().slice(StartTok, End - StartTok);
      int LBraceIdx = -1, RBraceIdx = -1;
      for (size_t I = 0; I < Tokens.size(); ++I) {
        if (Tokens[I].Kind == tok::l_brace && LBraceIdx == -1)
          LBraceIdx = static_cast<int>(I);
        if (Tokens[I].Kind == tok::r_brace)
          RBraceIdx = static_cast<int>(I);
      }
      if (LBraceIdx != -1 && RBraceIdx != -1 && LBraceIdx < RBraceIdx) {
        Range LRange = tokenRange(Tokens[LBraceIdx], Out, Code);
        Range RRange = tokenRange(Tokens[RBraceIdx], Out, Code);
        if (RRange.end.line > LRange.start.line) {
          FoldingRange FR;
          FR.startLine = LRange.start.line;
          FR.startCharacter = LRange.start.character;
          FR.endLine = RRange.end.line;
          FR.endCharacter = RRange.end.character;
          FR.kind = "region";
          Result.push_back(FR);
        }
      }
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      collectFoldingRanges(Children[I], ChildEnd, Out, Code, Result);
    }
  }
}

llvm::Expected<std::vector<DocumentSymbol>>
PseudoModule::getDocumentSymbols(llvm::StringRef Code) {
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<DocumentSymbol> Results;
  if (Parsed->Root) {
    pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
    walkSymbols(Parsed->Root, NumTokens, *Parsed, Code, Results);
  }
  return Results;
}

llvm::Expected<std::vector<SelectionRange>>
PseudoModule::getSemanticRanges(llvm::StringRef Code,
                                llvm::ArrayRef<Position> Positions) {
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<SelectionRange> Results;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  for (const auto &Pos : Positions) {
    auto Offset = positionToOffset(Code, Pos);
    if (!Offset) {
      consumeError(Offset.takeError());
      continue;
    }
    std::vector<Range> Path;
    if (Parsed->Root)
      collectSelectionRanges(Parsed->Root, NumTokens, *Parsed, Code, *Offset,
                             Path);
    if (Path.empty())
      Path.push_back(Range{Pos, Pos});

    std::unique_ptr<SelectionRange> Current;
    for (const auto &R : Path) {
      auto Next = std::make_unique<SelectionRange>();
      Next->range = R;
      Next->parent = std::move(Current);
      Current = std::move(Next);
    }
    Results.push_back(std::move(*Current));
  }
  return Results;
}

llvm::Expected<std::vector<FoldingRange>>
PseudoModule::getFoldingRanges(llvm::StringRef Code, bool LineFoldingOnly) {
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<FoldingRange> Results;
  if (Parsed->Root) {
    pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
    collectFoldingRanges(Parsed->Root, NumTokens, *Parsed, Code, Results);
  }
  return Results;
}

} // namespace clangd
} // namespace clang
