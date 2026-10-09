//===--- Scopes.cpp - Lexical scoping and declaration lookup -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pseudo/Scopes.h"
#include "SourceCode.h"

namespace clang {
namespace clangd {

const LocalDecl *lookupDecl(size_t ScopeId, llvm::StringRef Name,
                            size_t AtOffset,
                            const std::vector<LexicalScope> &Scopes,
                            llvm::StringRef Code,
                            bool ExpectsType) {
  size_t Cur = ScopeId;
  const LocalDecl *FallbackMatch = nullptr;
  while (true) {
    const auto &S = Scopes[Cur];
    for (const auto &D : S.Decls) {
      if (D.Name == Name) {
        if (ExpectsType) {
          if (isTypeDecl(D.Kind))
            return &D;
        } else {
          if (D.IsParameter || D.IsMember || D.DeclOffset <= AtOffset ||
              (D.NameRange.start.line == offsetToPosition(Code, AtOffset).line)) {
            if (!isTypeDecl(D.Kind))
              return &D;
            if (!FallbackMatch)
              FallbackMatch = &D;
          }
        }
      }
    }
    if (!S.EnclosingClass.empty()) {
      for (const auto &OtherScope : Scopes) {
        if (OtherScope.Kind == ScopeKind::Class &&
            OtherScope.EnclosingClass == S.EnclosingClass &&
            OtherScope.Id != Cur) {
          for (const auto &D : OtherScope.Decls) {
            if (D.Name == Name) {
              if (ExpectsType) {
                if (isTypeDecl(D.Kind))
                  return &D;
              } else if (D.IsMember) {
                if (!isTypeDecl(D.Kind))
                  return &D;
                if (!FallbackMatch)
                  FallbackMatch = &D;
              }
            }
          }
        }
      }
    }
    if (Cur == 0)
      break;
    Cur = S.ParentId;
  }
  return FallbackMatch;
}

void scanOpaqueDeclarations(pseudo::Token::Index StartTok,
                            pseudo::Token::Index EndTok,
                            const ParseOutput &Out, llvm::StringRef Code,
                            std::vector<LexicalScope> &Scopes,
                            size_t CurrentScopeId,
                            llvm::StringRef EnclosingClass) {
  auto Tokens = Out.ParseableStream.tokens();
  if (StartTok >= EndTok || StartTok >= Tokens.size())
    return;
  if (EndTok > Tokens.size())
    EndTok = Tokens.size();

  size_t CurScopeId = CurrentScopeId;
  std::vector<size_t> ScopeStack;

  size_t I = StartTok;
  while (I < EndTok) {
    auto TK = Tokens[I].Kind;
    if (TK == tok::comment) {
      ++I;
      continue;
    }
    if (TK == tok::semi) {
      ++I;
      continue;
    }
    if (TK == tok::r_brace) {
      if (!ScopeStack.empty()) {
        Scopes[CurScopeId].EndOffset = tokenEndOffset(Tokens[I], Out);
        CurScopeId = ScopeStack.back();
        ScopeStack.pop_back();
      }
      ++I;
      continue;
    }
    if (TK == tok::l_brace) {
      size_t NewScopeId = Scopes.size();
      LexicalScope S;
      S.Id = NewScopeId;
      S.ParentId = CurScopeId;
      S.Kind = ScopeKind::Block;
      S.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
      S.StartOffset = tokenStartOffset(Tokens[I], Out);
      S.EndOffset = tokenEndOffset(Tokens[I], Out);
      S.ScopeRange = tokenRange(Tokens[I], Out, Code);
      Scopes[CurScopeId].Children.push_back(NewScopeId);
      Scopes.push_back(std::move(S));
      ScopeStack.push_back(CurScopeId);
      CurScopeId = NewScopeId;
      ++I;
      continue;
    }

    if (TK == tok::kw_namespace) {
      size_t J = I + 1;
      std::string NsName;
      const pseudo::Token *NsTok = nullptr;
      size_t K = J;
      while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
             Tokens[K].Kind != tok::semi) {
        if (Tokens[K].Kind == tok::raw_identifier ||
            Tokens[K].Kind == tok::identifier) {
          if (!NsName.empty() && K > J && Tokens[K - 1].Kind == tok::coloncolon)
            NsName += "::";
          NsName += getOrigToken(Tokens[K], Out).text().str();
          NsTok = &Tokens[K];
        }
        ++K;
      }
      if (K < EndTok && Tokens[K].Kind == tok::l_brace) {
        if (NsTok && !NsName.empty()) {
          LocalDecl LD;
          LD.Name = NsName;
          LD.NameRange = tokenRange(*NsTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NsTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = true;
          LD.Kind = DeclKind::Namespace;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));
        }

        size_t NewScopeId = Scopes.size();
        LexicalScope S;
        S.Id = NewScopeId;
        S.ParentId = CurScopeId;
        S.Kind = ScopeKind::Namespace;
        S.Name = NsName;
        S.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
        S.StartOffset = tokenStartOffset(Tokens[K], Out);
        S.EndOffset = tokenEndOffset(Tokens[K], Out);
        S.ScopeRange = tokenRange(Tokens[K], Out, Code);
        Scopes[CurScopeId].Children.push_back(NewScopeId);
        Scopes.push_back(std::move(S));
        ScopeStack.push_back(CurScopeId);
        CurScopeId = NewScopeId;
        I = K + 1;
        continue;
      }
    }

    if (TK == tok::kw_class || TK == tok::kw_struct) {
      size_t J = I + 1;
      const pseudo::Token *NameTok = nullptr;
      size_t K = J;
      while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
             Tokens[K].Kind != tok::colon && Tokens[K].Kind != tok::semi) {
        if (Tokens[K].Kind == tok::raw_identifier ||
            Tokens[K].Kind == tok::identifier) {
          NameTok = &Tokens[K];
        }
        ++K;
      }
      if (NameTok) {
        std::string ClassName = getOrigToken(*NameTok, Out).text().str();
        while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
               Tokens[K].Kind != tok::semi)
          ++K;
        if (K < EndTok && Tokens[K].Kind == tok::l_brace) {
          LocalDecl LD;
          LD.Name = ClassName;
          LD.NameRange = tokenRange(*NameTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NameTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = true;
          LD.Kind = DeclKind::Class;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));

          size_t NewScopeId = Scopes.size();
          LexicalScope S;
          S.Id = NewScopeId;
          S.ParentId = CurScopeId;
          S.Kind = ScopeKind::Class;
          S.Name = ClassName;
          S.EnclosingClass = ClassName;
          S.StartOffset = tokenStartOffset(Tokens[K], Out);
          S.EndOffset = tokenEndOffset(Tokens[K], Out);
          S.ScopeRange = tokenRange(Tokens[K], Out, Code);
          Scopes[CurScopeId].Children.push_back(NewScopeId);
          Scopes.push_back(std::move(S));
          ScopeStack.push_back(CurScopeId);
          CurScopeId = NewScopeId;
          I = K + 1;
          continue;
        } else if (K < EndTok && Tokens[K].Kind == tok::semi) {
          LocalDecl LD;
          LD.Name = ClassName;
          LD.NameRange = tokenRange(*NameTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NameTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = false;
          LD.Kind = DeclKind::Class;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));
          I = K + 1;
          continue;
        }
      }
    }

    if (TK == tok::kw_enum) {
      size_t J = I + 1;
      const pseudo::Token *NameTok = nullptr;
      size_t K = J;
      while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
             Tokens[K].Kind != tok::colon && Tokens[K].Kind != tok::semi) {
        if (Tokens[K].Kind == tok::raw_identifier ||
            Tokens[K].Kind == tok::identifier) {
          NameTok = &Tokens[K];
        }
        ++K;
      }
      if (NameTok) {
        std::string EnumName = getOrigToken(*NameTok, Out).text().str();
        while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
               Tokens[K].Kind != tok::semi)
          ++K;
        if (K < EndTok && Tokens[K].Kind == tok::l_brace) {
          LocalDecl LD;
          LD.Name = EnumName;
          LD.NameRange = tokenRange(*NameTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NameTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = true;
          LD.Kind = DeclKind::Enum;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));

          // Scan enumerator members inside braces
          size_t L = K + 1;
          while (L < EndTok && Tokens[L].Kind != tok::r_brace) {
            if (Tokens[L].Kind == tok::raw_identifier ||
                Tokens[L].Kind == tok::identifier) {
              LocalDecl Member;
              Member.Name = getOrigToken(Tokens[L], Out).text().str();
              Member.NameRange = tokenRange(Tokens[L], Out, Code);
              Member.DeclRange = Member.NameRange;
              Member.DeclOffset = tokenStartOffset(Tokens[L], Out);
              Member.ScopeId = CurScopeId;
              Member.EnclosingClass = EnumName;
              Member.IsDefinition = true;
              Member.Kind = DeclKind::EnumValue;
              Scopes[CurScopeId].Decls.push_back(std::move(Member));
              while (L < EndTok && Tokens[L].Kind != tok::comma &&
                     Tokens[L].Kind != tok::r_brace)
                ++L;
              if (L < EndTok && Tokens[L].Kind == tok::comma)
                ++L;
              continue;
            }
            ++L;
          }
          if (L < EndTok && Tokens[L].Kind == tok::r_brace)
            I = L + 1;
          else
            I = K + 1;
          continue;
        }
      }
    }

    // Find statement boundary
    int ParenDepth = 0;
    size_t StmtEnd = I;
    while (StmtEnd < EndTok) {
      auto K = Tokens[StmtEnd].Kind;
      if (K == tok::l_paren)
        ++ParenDepth;
      else if (K == tok::r_paren && ParenDepth > 0)
        --ParenDepth;
      else if ((K == tok::l_brace || K == tok::r_brace) && ParenDepth == 0)
        break;
      else if (K == tok::semi && ParenDepth == 0) {
        ++StmtEnd;
        break;
      }
      ++StmtEnd;
    }

    if (TK == tok::kw_if || TK == tok::kw_else || TK == tok::kw_while ||
        TK == tok::kw_do || TK == tok::kw_switch || TK == tok::kw_case ||
        TK == tok::kw_default || TK == tok::kw_return || TK == tok::kw_break ||
        TK == tok::kw_continue || TK == tok::kw_goto || TK == tok::kw_throw ||
        TK == tok::kw_try || TK == tok::kw_catch || TK == tok::kw_delete ||
        TK == tok::kw_sizeof) {
      I = StmtEnd;
      continue;
    }

    // Check for range-for
    if (TK == tok::kw_for) {
      if (I + 1 < StmtEnd && Tokens[I + 1].Kind == tok::l_paren) {
        size_t ColonIdx = I + 2;
        int PDepth = 1;
        while (ColonIdx < StmtEnd) {
          if (Tokens[ColonIdx].Kind == tok::l_paren)
            ++PDepth;
          else if (Tokens[ColonIdx].Kind == tok::r_paren) {
            --PDepth;
            if (PDepth == 0)
              break;
          } else if (Tokens[ColonIdx].Kind == tok::colon && PDepth == 1) {
            break;
          }
          ++ColonIdx;
        }
        if (ColonIdx < StmtEnd && Tokens[ColonIdx].Kind == tok::colon) {
          size_t TypeStartTok = I + 2;
          size_t NameTokIdx = ColonIdx - 1;
          while (NameTokIdx > TypeStartTok &&
                 Tokens[NameTokIdx].Kind != tok::raw_identifier &&
                 Tokens[NameTokIdx].Kind != tok::identifier)
            --NameTokIdx;
          if (NameTokIdx >= TypeStartTok &&
              (Tokens[NameTokIdx].Kind == tok::raw_identifier ||
               Tokens[NameTokIdx].Kind == tok::identifier)) {
            const auto &NameTok = Tokens[NameTokIdx];
            std::string VarName = getOrigToken(NameTok, Out).text().str();
            size_t TStart = tokenStartOffset(Tokens[TypeStartTok], Out);
            size_t TEnd = (NameTokIdx > TypeStartTok)
                              ? tokenEndOffset(Tokens[NameTokIdx - 1], Out)
                              : tokenStartOffset(NameTok, Out);
            std::string TypeName;
            if (TStart < TEnd && TEnd <= Code.size())
              TypeName = Code.slice(TStart, TEnd).trim().str();
            LocalDecl LD;
            LD.Name = VarName;
            LD.TypeName = TypeName;
            LD.NameRange = tokenRange(NameTok, Out, Code);
            LD.DeclRange = Range{offsetToPosition(Code, TStart),
                                 tokenRange(NameTok, Out, Code).end};
            LD.DeclOffset = tokenStartOffset(NameTok, Out);
            LD.ScopeId = CurScopeId;
            LD.IsMember = false;
            LD.IsDefinition = true;
            LD.Kind = DeclKind::Variable;
            Scopes[CurScopeId].Decls.push_back(std::move(LD));
          }
        }
      }
      I = StmtEnd;
      continue;
    }

    // Extract declarations from statement Tokens[I .. StmtEnd)
    size_t TIdx = I;
    while (TIdx < StmtEnd) {
      auto CurK = Tokens[TIdx].Kind;
      if (CurK == tok::kw_const || CurK == tok::kw_volatile ||
          CurK == tok::kw_static || CurK == tok::kw_auto ||
          CurK == tok::kw_inline || CurK == tok::kw_signed ||
          CurK == tok::kw_unsigned || CurK == tok::kw_long ||
          CurK == tok::kw_short || CurK == tok::kw_struct ||
          CurK == tok::kw_class || CurK == tok::kw_typename ||
          CurK == tok::kw_extern || CurK == tok::kw_void ||
          CurK == tok::kw_bool || CurK == tok::kw_char ||
          CurK == tok::kw_int || CurK == tok::kw_float ||
          CurK == tok::kw_double || CurK == tok::coloncolon ||
          CurK == tok::star || CurK == tok::amp || CurK == tok::ampamp) {
        ++TIdx;
        continue;
      }
      if (CurK == tok::less) {
        int Depth = 1;
        ++TIdx;
        while (TIdx < StmtEnd && Depth > 0) {
          if (Tokens[TIdx].Kind == tok::less)
            ++Depth;
          else if (Tokens[TIdx].Kind == tok::greater)
            --Depth;
          ++TIdx;
        }
        continue;
      }
      if (CurK == tok::raw_identifier || CurK == tok::identifier) {
        if (TIdx + 1 < StmtEnd && Tokens[TIdx + 1].Kind == tok::coloncolon) {
          TIdx += 2;
          continue;
        }
        if (TIdx + 1 < StmtEnd && Tokens[TIdx + 1].Kind == tok::less) {
          ++TIdx;
          continue;
        }
        if (TIdx + 1 < StmtEnd) {
          auto NextK = Tokens[TIdx + 1].Kind;
          if (NextK == tok::raw_identifier || NextK == tok::identifier ||
              NextK == tok::star || NextK == tok::amp || NextK == tok::ampamp) {
            ++TIdx;
            continue;
          }
        }
      }
      break;
    }

    if (TIdx > I && TIdx < StmtEnd) {
      size_t TypeStart = tokenStartOffset(Tokens[I], Out);
      size_t TypeEnd = tokenEndOffset(Tokens[TIdx - 1], Out);
      std::string DeclType;
      if (TypeStart < TypeEnd && TypeEnd <= Code.size())
        DeclType = Code.slice(TypeStart, TypeEnd).trim().str();

      while (TIdx < StmtEnd &&
             (Tokens[TIdx].Kind == tok::raw_identifier ||
              Tokens[TIdx].Kind == tok::identifier)) {
        const auto &NameTok = Tokens[TIdx];
        std::string VarName = getOrigToken(NameTok, Out).text().str();
        size_t NameEnd = TIdx + 1;
        bool IsFunc = false;
        if (NameEnd < StmtEnd && Tokens[NameEnd].Kind == tok::l_paren) {
          if (llvm::StringRef(DeclType).starts_with("void") ||
              Scopes[CurScopeId].Kind == ScopeKind::Class)
            IsFunc = true;
        }

        LocalDecl LD;
        LD.Name = VarName;
        LD.TypeName = DeclType;
        LD.NameRange = tokenRange(NameTok, Out, Code);
        LD.DeclRange = Range{offsetToPosition(Code, TypeStart),
                             tokenRange(Tokens[StmtEnd - 1], Out, Code).end};
        LD.DeclOffset = tokenStartOffset(NameTok, Out);
        LD.ScopeId = CurScopeId;
        LD.IsMember = (Scopes[CurScopeId].Kind == ScopeKind::Class);
        LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
        LD.IsDefinition = true;
        LD.Kind = IsFunc ? DeclKind::Function
                         : DeclKind::Variable;

        if (LD.TypeName == "auto" || LD.TypeName == "const auto &" ||
            LD.TypeName == "auto &") {
          for (size_t K = NameEnd; K < StmtEnd; ++K) {
            if (Tokens[K].Kind == tok::raw_identifier ||
                Tokens[K].Kind == tok::identifier) {
              llvm::StringRef Word = getOrigToken(Tokens[K], Out).text();
              if (Word == "instantiate" && K >= 2 &&
                  Tokens[K - 1].Kind == tok::period) {
                llvm::StringRef BaseVar =
                    getOrigToken(Tokens[K - 2], Out).text();
                const LocalDecl *BD = lookupDecl(
                    CurScopeId, BaseVar, tokenStartOffset(Tokens[K], Out),
                    Scopes, Code);
                if (BD && !BD->TypeName.empty()) {
                  if (BD->TypeName.find("entry") != llvm::StringRef::npos)
                    LD.TypeName = "FeatureModule";
                }
              }
            }
          }
        }

        Scopes[CurScopeId].Decls.push_back(std::move(LD));

        int PDepth = 0;
        int BDepth = 0;
        while (NameEnd < StmtEnd) {
          auto K = Tokens[NameEnd].Kind;
          if (K == tok::l_paren)
            ++PDepth;
          else if (K == tok::r_paren && PDepth > 0)
            --PDepth;
          else if (K == tok::l_brace)
            ++BDepth;
          else if (K == tok::r_brace && BDepth > 0)
            --BDepth;
          else if (K == tok::comma && PDepth == 0 && BDepth == 0) {
            ++NameEnd;
            break;
          } else if (K == tok::semi && PDepth == 0 && BDepth == 0) {
            break;
          }
          ++NameEnd;
        }
        TIdx = NameEnd;
        while (TIdx < StmtEnd &&
               (Tokens[TIdx].Kind == tok::star || Tokens[TIdx].Kind == tok::amp))
          ++TIdx;
      }
    }

    I = StmtEnd;
  }
}

void buildScopes(const pseudo::ForestNode *N, pseudo::Token::Index End,
                 const ParseOutput &Out, llvm::StringRef Code,
                 std::vector<LexicalScope> &Scopes, size_t &CurrentScopeId,
                 llvm::StringRef EnclosingClass,
                 llvm::StringRef DeclaredType) {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    buildScopes(Alts[AltIdx], End, Out, Code, Scopes, CurrentScopeId,
                EnclosingClass, DeclaredType);
    return;
  }

  if (N->kind() == pseudo::ForestNode::Opaque) {
    scanOpaqueDeclarations(N->startTokenIndex(), End, Out, Code, Scopes,
                           CurrentScopeId, EnclosingClass);
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

  // 1. Class specifier
  if (Sym == pseudo::cxx::Symbol::class_specifier) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        NameTok = &NodeTokens[I];
    }
    std::string ClassName;
    if (NameTok) {
      ClassName = getOrigToken(*NameTok, Out).text().str();
      LocalDecl LD;
      LD.Name = ClassName;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = DeclKind::Class;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }

    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Class;
    S.Name = ClassName;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = ClassName;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  ClassName);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 1b. Elaborated type specifier (e.g. forward declaration: class MacroInfo;)
  if (Sym == pseudo::cxx::Symbol::elaborated_type_specifier) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = false;
      LD.Kind = DeclKind::Class;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 2. Namespaces
  if (Sym == pseudo::cxx::Symbol::named_namespace_definition ||
      Sym == pseudo::cxx::Symbol::nested_namespace_definition ||
      Sym == pseudo::cxx::Symbol::unnamed_namespace_definition) {
    std::string NsName;
    const pseudo::Token *NsTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (!NsName.empty() && I > 0 && NodeTokens[I - 1].Kind == tok::coloncolon)
          NsName += "::";
        NsName += getOrigToken(NodeTokens[I], Out).text();
        NsTok = &NodeTokens[I];
      }
    }
    if (NsTok && !NsName.empty()) {
      LocalDecl LD;
      LD.Name = NsName;
      LD.NameRange = tokenRange(*NsTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NsTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = DeclKind::Namespace;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Namespace;
    S.Name = NsName;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = EnclosingClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 3. Function definition
  if (Sym == pseudo::cxx::Symbol::function_definition) {
    const pseudo::Token *NameTok = nullptr;
    std::string FuncEnclosingClass;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_paren) {
        for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
          if (NodeTokens[J].Kind == tok::raw_identifier ||
              NodeTokens[J].Kind == tok::identifier) {
            NameTok = &NodeTokens[J];
            if (J >= 2 && NodeTokens[J - 1].Kind == tok::coloncolon &&
                (NodeTokens[J - 2].Kind == tok::raw_identifier ||
                 NodeTokens[J - 2].Kind == tok::identifier)) {
              FuncEnclosingClass =
                  getOrigToken(NodeTokens[J - 2], Out).text().str();
            }
            break;
          }
        }
        break;
      }
    }

    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = !FuncEnclosingClass.empty()
                              ? FuncEnclosingClass
                              : std::string(EnclosingClass);
      LD.IsMember = !LD.EnclosingClass.empty();
      LD.IsDefinition = true;
      if ((!LD.EnclosingClass.empty() && LD.Name == LD.EnclosingClass)) {
        LD.Kind = DeclKind::Constructor;
      } else {
        LD.Kind = DeclKind::Function;
      }
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }

    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Function;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    std::string EnclClass =
        !FuncEnclosingClass.empty() ? FuncEnclosingClass : std::string(EnclosingClass);
    S.EnclosingClass = EnclClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 4. Parameter declaration
  if (Sym == pseudo::cxx::Symbol::parameter_declaration) {
    const pseudo::Token *NameTok = nullptr;
    size_t NameTokIdx = pseudo::Token::Invalid;
    int AngleDepth = 0;
    bool HasExplicitType = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal || NodeTokens[I].Kind == tok::comma ||
          NodeTokens[I].Kind == tok::r_paren)
        break;
      if (NodeTokens[I].Kind == tok::less) {
        ++AngleDepth;
        continue;
      }
      if (NodeTokens[I].Kind == tok::greater && AngleDepth > 0) {
        --AngleDepth;
        continue;
      }
      if (AngleDepth > 0)
        continue;

      if (NodeTokens[I].Kind == tok::kw_auto ||
          NodeTokens[I].Kind == tok::kw_void ||
          NodeTokens[I].Kind == tok::kw_bool ||
          NodeTokens[I].Kind == tok::kw_char ||
          NodeTokens[I].Kind == tok::kw_int ||
          NodeTokens[I].Kind == tok::kw_float ||
          NodeTokens[I].Kind == tok::kw_double ||
          NodeTokens[I].Kind == tok::kw_long ||
          NodeTokens[I].Kind == tok::kw_short ||
          NodeTokens[I].Kind == tok::kw_unsigned ||
          NodeTokens[I].Kind == tok::kw_signed)
        HasExplicitType = true;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (I > 0 && NodeTokens[I - 1].Kind == tok::coloncolon)
          continue;
        if (I + 1 < NodeTokens.size() &&
            (NodeTokens[I + 1].Kind == tok::coloncolon ||
             NodeTokens[I + 1].Kind == tok::less ||
             NodeTokens[I + 1].Kind == tok::star ||
             NodeTokens[I + 1].Kind == tok::amp ||
             NodeTokens[I + 1].Kind == tok::ampamp))
          continue;
        NameTok = &NodeTokens[I];
        NameTokIdx = I;
      }
    }
    bool HasPrecedingType = (NameTok && NameTokIdx > 0) || HasExplicitType;
    if (NameTok && HasPrecedingType) {
      size_t TStart = tokenStartOffset(NodeTokens[0], Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      std::string ParamType;
      if (TStart < TEnd && TEnd <= Code.size())
        ParamType = Code.slice(TStart, TEnd).trim().str();
      if (ParamType.empty() && HasExplicitType) {
        for (size_t I = 0; I < NameTokIdx && I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind != tok::comment) {
            ParamType = getOrigToken(NodeTokens[I], Out).text().str();
            break;
          }
        }
      }

      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = ParamType;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsParameter = true;
      LD.IsDefinition = true;
      LD.Kind = DeclKind::Parameter;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass, DeclaredType);
    }
    return;
  }

  // 5. Compound statement (Block)
  if (Sym == pseudo::cxx::Symbol::compound_statement) {
    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Block;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = EnclosingClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 5b. For-range declaration (e.g. for (FeatureModuleRegistry::entry E : ...))
  if (Sym == pseudo::cxx::Symbol::for_range_declaration) {
    std::string RangeType;
    auto Children = N->elements();
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart =
          tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd =
          tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size())
        RangeType = Code.slice(OffStart, OffEnd).trim().str();
    }
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    // When buildScopes processes for_range_declaration, 'End' (EndTok) is the
    // start of the for_range_initializer node (the container expression), since
    // the ':' is a grammar terminal between them. So we can directly use EndTok
    // as the start of the container expression and search forward for ')'.
    std::string ContainerExpr;
    const auto &StreamTokens = Out.ParseableStream.tokens();
    // Also try searching backward from EndTok for ':' in case the pseudo-grammar
    // includes the colon in the for_range_declaration's range.
    size_t InitStart = EndTok;
    // Check if EndTok is immediately after a colon (common layout)
    // or if we need to skip the colon token
    if (InitStart < StreamTokens.size() &&
        StreamTokens[InitStart].Kind == tok::colon) {
      ++InitStart; // skip the colon if it's right there
    } else {
      // Search backward from EndTok to see if there's a ':' just before it
      // (unlikely, but handle it)
      // Also handle the case where EndTok > 0 and EndTok-1 is ':'
      if (InitStart > 0 &&
          StreamTokens[InitStart - 1].Kind == tok::colon) {
        // InitStart is already correct
      }
      // If EndTok itself starts the container expression (the ':' was consumed
      // by the parent grammar rule), use EndTok directly
    }

    if (InitStart < StreamTokens.size()) {
      size_t InitEnd = InitStart;
      int PDepth = 0;
      while (InitEnd < StreamTokens.size()) {
        if (StreamTokens[InitEnd].Kind == tok::l_paren)
          ++PDepth;
        else if (StreamTokens[InitEnd].Kind == tok::r_paren) {
          if (PDepth == 0)
            break;
          --PDepth;
        } else if (StreamTokens[InitEnd].Kind == tok::semi && PDepth == 0) {
          break;
        }
        ++InitEnd;
      }
      if (InitStart < InitEnd && InitEnd <= StreamTokens.size()) {
        size_t CStart = tokenStartOffset(StreamTokens[InitStart], Out);
        size_t CEnd = tokenEndOffset(StreamTokens[InitEnd - 1], Out);
        if (CStart < CEnd && CEnd <= Code.size())
          ContainerExpr = Code.slice(CStart, CEnd).trim().str();
      }
    }

    std::string DeducedType = RangeType;
    if (DeducedType == "auto" || DeducedType == "auto &" ||
        DeducedType == "const auto &" || DeducedType.empty()) {
      llvm::StringRef CleanInit = ContainerExpr;
      while (CleanInit.starts_with("*") || CleanInit.starts_with("&") ||
             CleanInit.starts_with(" "))
        CleanInit = CleanInit.drop_front(1).trim();

      if (CleanInit.ends_with("FeatureModules") ||
          CleanInit.ends_with("FeatureModuleSet")) {
        DeducedType = "FeatureModule";
      } else if (CleanInit.contains("::entries()")) {
        size_t Pos = CleanInit.find("::entries()");
        llvm::StringRef Reg = CleanInit.take_front(Pos).trim();
        if (Reg.ends_with("Registry")) {
          llvm::StringRef Base = Reg.drop_back(strlen("Registry"));
          size_t Colons = Base.rfind("::");
          if (Colons != llvm::StringRef::npos)
            Base = Base.drop_front(Colons + 2);
          DeducedType = (Base + "Registry::entry").str();
        }
      }
    }

    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = DeducedType;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.Kind = DeclKind::Variable;
      LD.IsDefinition = true;
      LD.InitExpr = ContainerExpr;
      LD.IsRangeFor = true;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass, RangeType);
    }
    return;
  }

  // 5c. Condition (e.g. if (void *Key = M->typeId()))
  if (Sym == pseudo::cxx::Symbol::condition) {
    bool HasEqual = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal) {
        HasEqual = true;
        break;
      }
    }
    auto Children = N->elements();
    if (!HasEqual) {
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
        buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                    EnclosingClass, DeclaredType);
      }
      return;
    }

    std::string CondType;
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart =
          tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd =
          tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size())
        CondType = Code.slice(OffStart, OffEnd).trim().str();
    }
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = CondType;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.Kind = DeclKind::Variable;
      LD.IsDefinition = true;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass, CondType);
    }
    return;
  }

  // 6. Init declarator & Member declarator
  if (Sym == pseudo::cxx::Symbol::init_declarator ||
      Sym == pseudo::cxx::Symbol::member_declarator) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
          NodeTokens[I].Kind == tok::colon || NodeTokens[I].Kind == tok::l_brace ||
          NodeTokens[I].Kind == tok::l_paren)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = DeclaredType.str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.IsMember = (Sym == pseudo::cxx::Symbol::member_declarator) ||
                    (Scopes[CurrentScopeId].Kind == ScopeKind::Class);
      if (LD.IsMember)
        LD.EnclosingClass = std::string(EnclosingClass);
      else
        LD.EnclosingClass = "";
      bool HasLParen = false;
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::l_paren) {
          HasLParen = true;
          break;
        }
      }
      if (HasLParen) {
        if (!EnclosingClass.empty() && LD.Name == EnclosingClass)
          LD.Kind = DeclKind::Constructor;
        else
          LD.Kind = DeclKind::Function;
        LD.IsDefinition = false;
      } else {
        LD.Kind = DeclKind::Variable;
        LD.IsDefinition = true;
      }

      // Deduce auto or empty TypeName from initializer (e.g. auto M = E.instantiate();)
      if (LD.TypeName.empty() || LD.TypeName == "auto") {
        for (size_t I = 0; I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind == tok::raw_identifier ||
              NodeTokens[I].Kind == tok::identifier) {
            llvm::StringRef Word = getOrigToken(NodeTokens[I], Out).text();
            if (Word == "instantiate" && I >= 2 &&
                NodeTokens[I - 1].Kind == tok::period) {
              llvm::StringRef BaseVar =
                  getOrigToken(NodeTokens[I - 2], Out).text();
              const LocalDecl *BD = lookupDecl(
                  CurrentScopeId, BaseVar,
                  tokenStartOffset(NodeTokens[I], Out), Scopes, Code);
              if (BD && !BD->TypeName.empty()) {
                llvm::StringRef BT = BD->TypeName;
                if (BT.contains("Registry")) {
                  size_t RegPos = BT.find("Registry");
                  llvm::StringRef Target = BT.take_front(RegPos);
                  size_t Colons = Target.rfind("::");
                  if (Colons != llvm::StringRef::npos)
                    Target = Target.drop_front(Colons + 2);
                  if (!Target.empty())
                    LD.TypeName = ("std::unique_ptr<" + Target + ">").str();
                }
              }
            } else if (Word == "make_unique" || Word == "make_shared") {
              for (size_t J = I + 1; J < NodeTokens.size(); ++J) {
                if (NodeTokens[J].Kind == tok::less &&
                    J + 1 < NodeTokens.size()) {
                  llvm::StringRef Target =
                      getOrigToken(NodeTokens[J + 1], Out).text();
                  LD.TypeName = ("std::unique_ptr<" + Target + ">").str();
                  break;
                }
              }
            }
          }
        }
      }

      // Deduce auto TypeName from simple "= VarName" initializer (e.g. auto X = Y;)
      // This handles cases like: auto CodeCompleteOpts = Opts;
      if (LD.TypeName.empty() || LD.TypeName == "auto") {
        // Find '=' token and check if it's followed by a single identifier
        for (size_t I = 0; I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind == tok::equal) {
            // Collect identifier tokens after '=', skip any '&' or '*'
            size_t J = I + 1;
            while (J < NodeTokens.size() &&
                   (NodeTokens[J].Kind == tok::amp ||
                    NodeTokens[J].Kind == tok::star))
              ++J;
            if (J < NodeTokens.size() &&
                (NodeTokens[J].Kind == tok::raw_identifier ||
                 NodeTokens[J].Kind == tok::identifier)) {
              llvm::StringRef RhsName = getOrigToken(NodeTokens[J], Out).text();
              // Only do this if no further complex expression (no '(', '.', etc.)
              bool SimpleRhs = true;
              for (size_t K = J + 1; K < NodeTokens.size(); ++K) {
                tok::TokenKind TK = NodeTokens[K].Kind;
                if (TK == tok::l_paren || TK == tok::period ||
                    TK == tok::arrow || TK == tok::l_brace) {
                  SimpleRhs = false;
                  break;
                }
              }
              if (SimpleRhs && !RhsName.empty()) {
                size_t EqOffset = tokenStartOffset(NodeTokens[I], Out);
                const LocalDecl *RhsDecl = lookupDecl(
                    CurrentScopeId, RhsName, EqOffset, Scopes, Code);
                if (RhsDecl && !RhsDecl->TypeName.empty() &&
                    RhsDecl->TypeName != "auto")
                  LD.TypeName = RhsDecl->TypeName;
              }
            }
            break;
          }
        }
      }

      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    // Do NOT return here — fall through to recurse into children.
    // This is critical for lambda initializers: `auto Task = [...](...) {...};`
    // must recurse into the lambda-expression child node.
  }

  // 6b. Lambda expression: [captures](params) { body }
  // Grammar: lambda-expression := lambda-introducer lambda-declarator_opt compound-statement
  if (Sym == pseudo::cxx::Symbol::lambda_expression) {
    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Block;

    pseudo::Token::Index IntroducerEnd = StartTok;
    int BracketDepth = 0;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_square)
        ++BracketDepth;
      else if (NodeTokens[I].Kind == tok::r_square) {
        --BracketDepth;
        if (BracketDepth <= 0) {
          IntroducerEnd = StartTok + I + 1;
          break;
        }
      }
    }
    if (IntroducerEnd < EndTok)
      S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[IntroducerEnd], Out);
    else
      S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = EnclosingClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;

    // Parse capture list: tokens between '[' and ']'
    // Captures: simple-capture (IDENTIFIER), init-capture (IDENTIFIER = ...)
    {
      bool InCaptures = false;
      int BracketDepth = 0;
      // State for current capture being parsed
      const pseudo::Token *CaptureName = nullptr;
      bool IsInitCapture = false; // has '=' in current capture
      // Identifier seen after '=' in init-capture (RHS name)
      const pseudo::Token *RhsNameTok = nullptr;

      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        tok::TokenKind K = NodeTokens[I].Kind;
        if (!InCaptures) {
          if (K == tok::l_square) {
            InCaptures = true;
            BracketDepth = 1;
          }
          continue;
        }
        if (K == tok::l_square) {
          ++BracketDepth;
          continue;
        }
        if (K == tok::r_square) {
          --BracketDepth;
          if (BracketDepth <= 0) {
            // End of capture list — emit last capture if any
            if (CaptureName) {
              llvm::StringRef CaptName = getOrigToken(*CaptureName, Out).text();
              LocalDecl LD;
              LD.Name = CaptName.str();
              LD.NameRange = tokenRange(*CaptureName, Out, Code);
              LD.DeclRange = LD.NameRange;
              LD.DeclOffset = tokenStartOffset(*CaptureName, Out);
              LD.ScopeId = NewScopeId;
              LD.Kind = DeclKind::Variable;
              LD.IsDefinition = true;
              LD.IsCapture = true;
              LD.IsInitCapture = IsInitCapture;

              if (IsInitCapture && RhsNameTok) {
                // e.g. CB = std::move(CB) — RHS is the outer CB
                llvm::StringRef RhsName = getOrigToken(*RhsNameTok, Out).text();
                const LocalDecl *OuterDecl = lookupDecl(
                    SavedScope, RhsName, LD.DeclOffset, Scopes, Code);
                if (OuterDecl) {
                  LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                  if (!OuterDecl->TypeName.empty())
                    LD.TypeName = OuterDecl->TypeName;
                }
              } else {
                // Simple capture: look up name in parent scope
                const LocalDecl *OuterDecl = lookupDecl(
                    SavedScope, CaptName, LD.DeclOffset, Scopes, Code);
                if (OuterDecl) {
                  LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                  if (!OuterDecl->TypeName.empty())
                    LD.TypeName = OuterDecl->TypeName;
                }
              }
              Scopes[NewScopeId].Decls.push_back(std::move(LD));
              CaptureName = nullptr;
              IsInitCapture = false;
              RhsNameTok = nullptr;
            }
            break;
          }
          continue;
        }
        if (!InCaptures || BracketDepth <= 0)
          continue;

        // Separator between captures
        if (K == tok::comma) {
          if (CaptureName) {
            llvm::StringRef CaptName = getOrigToken(*CaptureName, Out).text();
            LocalDecl LD;
            LD.Name = CaptName.str();
            LD.NameRange = tokenRange(*CaptureName, Out, Code);
            LD.DeclRange = LD.NameRange;
            LD.DeclOffset = tokenStartOffset(*CaptureName, Out);
            LD.ScopeId = NewScopeId;
            LD.Kind = DeclKind::Variable;
            LD.IsDefinition = true;
            LD.IsCapture = true;
            LD.IsInitCapture = IsInitCapture;

            if (IsInitCapture && RhsNameTok) {
              llvm::StringRef RhsName = getOrigToken(*RhsNameTok, Out).text();
              const LocalDecl *OuterDecl = lookupDecl(
                  SavedScope, RhsName, LD.DeclOffset, Scopes, Code);
              if (OuterDecl) {
                LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                if (!OuterDecl->TypeName.empty())
                  LD.TypeName = OuterDecl->TypeName;
              }
            } else {
              const LocalDecl *OuterDecl = lookupDecl(
                  SavedScope, CaptName, LD.DeclOffset, Scopes, Code);
              if (OuterDecl) {
                LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                if (!OuterDecl->TypeName.empty())
                  LD.TypeName = OuterDecl->TypeName;
              }
            }
            Scopes[NewScopeId].Decls.push_back(std::move(LD));
          }
          CaptureName = nullptr;
          IsInitCapture = false;
          RhsNameTok = nullptr;
          continue;
        }

        // Skip '&' prefix before capture name
        if (K == tok::amp || K == tok::star)
          continue;
        // Skip '=' (capture-default) or '...'
        if (K == tok::ellipsis || K == tok::kw_this)
          continue;

        if (K == tok::raw_identifier || K == tok::identifier) {
          llvm::StringRef Word = getOrigToken(NodeTokens[I], Out).text();
          if (Word == "this")
            continue;
          if (!CaptureName) {
            CaptureName = &NodeTokens[I];
          } else if (IsInitCapture && !RhsNameTok) {
            // After '=', we want the first identifier (might be the RHS var, e.g. CB in CB = std::move(CB))
            RhsNameTok = &NodeTokens[I];
          }
          continue;
        }

        if (K == tok::equal && CaptureName) {
          IsInitCapture = true;
          continue;
        }
      }
    }

    // Recurse into children (lambda_declarator for params, compound_statement for body)
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 7. Enum specifier
  if (Sym == pseudo::cxx::Symbol::enum_specifier) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        NameTok = &NodeTokens[I];
    }
    std::string EnumName;
    if (NameTok) {
      EnumName = getOrigToken(*NameTok, Out).text().str();
      LocalDecl LD;
      LD.Name = EnumName;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = DeclKind::Enum;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
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
        LocalDecl Member;
        Member.Name = getOrigToken(NodeTokens[I], Out).text().str();
        Member.NameRange = tokenRange(NodeTokens[I], Out, Code);
        Member.DeclRange = Member.NameRange;
        Member.DeclOffset = tokenStartOffset(NodeTokens[I], Out);
        Member.ScopeId = CurrentScopeId;
        Member.EnclosingClass = !EnumName.empty() ? EnumName : std::string(EnclosingClass);
        Member.IsDefinition = true;
        Member.Kind = DeclKind::EnumValue;
        Scopes[CurrentScopeId].Decls.push_back(std::move(Member));
        while (I + 1 < NodeTokens.size() &&
               NodeTokens[I + 1].Kind != tok::comma &&
               NodeTokens[I + 1].Kind != tok::r_brace)
          ++I;
      }
    }
    return;
  }

  // 8. Alias declaration
  if (Sym == pseudo::cxx::Symbol::alias_declaration) {
    const pseudo::Token *NameTok = nullptr;
    size_t EqIdx = NodeTokens.size();
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_using && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          NameTok = &IdTok;
        }
      }
      if (NodeTokens[I].Kind == tok::equal) {
        EqIdx = I;
        break;
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = DeclKind::TypeAlias;
      if (EqIdx + 1 < NodeTokens.size()) {
        size_t StartOff = tokenStartOffset(NodeTokens[EqIdx + 1], Out);
        size_t EndOff = tokenEndOffset(NodeTokens.back(), Out);
        if (StartOff < EndOff && EndOff <= Code.size()) {
          llvm::StringRef TypeText = Code.slice(StartOff, EndOff);
          size_t Semi = TypeText.find(';');
          if (Semi != llvm::StringRef::npos)
            TypeText = TypeText.take_front(Semi);
          LD.TypeName = TypeText.trim().str();
        }
      }
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 9. Template type parameter (template <typename T>, template <class T>)
  if (Sym == pseudo::cxx::Symbol::type_parameter) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = DeclKind::TemplateParam;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 9b. Concept definition (concept X = ...;)
  if (Sym == pseudo::cxx::Symbol::concept_definition) {
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_concept && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          LocalDecl LD;
          LD.Name = getOrigToken(IdTok, Out).text().str();
          LD.NameRange = tokenRange(IdTok, Out, Code);
          LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
          LD.DeclOffset = tokenStartOffset(IdTok, Out);
          LD.ScopeId = CurrentScopeId;
          LD.EnclosingClass = std::string(EnclosingClass);
          LD.IsDefinition = true;
          LD.Kind = DeclKind::Concept;
          Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
          break;
        }
      }
    }
    return;
  }

  // 10. Using declaration (using ns::foo;)
  if (Sym == pseudo::cxx::Symbol::using_declaration) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = false;
      LD.Kind = DeclKind::TypeAlias;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 11. Namespace alias definition (namespace NA = RealNamespace;)
  if (Sym == pseudo::cxx::Symbol::namespace_alias_definition) {
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_namespace && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          LocalDecl LD;
          LD.Name = getOrigToken(IdTok, Out).text().str();
          LD.NameRange = tokenRange(IdTok, Out, Code);
          LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
          LD.DeclOffset = tokenStartOffset(IdTok, Out);
          LD.ScopeId = CurrentScopeId;
          LD.EnclosingClass = std::string(EnclosingClass);
          LD.IsDefinition = true;
          LD.Kind = DeclKind::Namespace;
          Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
          break;
        }
      }
    }
    return;
  }

  // Fallthrough: recurse down children
  llvm::StringRef ChildDeclaredType = DeclaredType;
  std::string ExtractedType;
  if (Sym == pseudo::cxx::Symbol::simple_declaration ||
      Sym == pseudo::cxx::Symbol::member_declaration) {
    auto Children = N->elements();
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart =
          tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd =
          tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size()) {
        ExtractedType = Code.slice(OffStart, OffEnd).trim().str();
        ChildDeclaredType = ExtractedType;
      }
    }
    if (ChildDeclaredType.empty()) {
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
            NodeTokens[I].Kind == tok::l_brace)
          break;
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          ChildDeclaredType = getOrigToken(NodeTokens[I], Out).text();
          break;
        }
      }
    }
  }

  auto Children = N->elements();
  for (size_t I = 0; I < Children.size(); ++I) {
    pseudo::Token::Index ChildEnd =
        (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
    buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                EnclosingClass, ChildDeclaredType);
  }
}


const LocalDecl *findAnyDecl(llvm::StringRef Name,
                             const std::vector<LexicalScope> &Scopes,
                             bool ExpectsType) {
  const LocalDecl *FallbackMatch = nullptr;
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (D.Name == Name) {
        if (ExpectsType) {
          if (isTypeDecl(D.Kind))
            return &D;
        } else {
          if (!isTypeDecl(D.Kind))
            return &D;
          if (!FallbackMatch)
            FallbackMatch = &D;
        }
      }
    }
  }
  return FallbackMatch;
}

bool isSameEntity(const LocalDecl *A, const LocalDecl *B) {
  if (!A || !B)
    return false;
  if (A == B)
    return true;
  if (A->Name != B->Name)
    return false;
  if (A->IsCapture && A->CapturedDeclOffset == B->DeclOffset &&
      A->CapturedDeclOffset != 0)
    return true;
  if (B->IsCapture && B->CapturedDeclOffset == A->DeclOffset &&
      B->CapturedDeclOffset != 0)
    return true;
  if (A->IsCapture && B->IsCapture &&
      A->CapturedDeclOffset == B->CapturedDeclOffset &&
      A->CapturedDeclOffset != 0)
    return true;
  if (A->IsMember && B->IsMember)
    return !A->EnclosingClass.empty() && A->EnclosingClass == B->EnclosingClass;
  if (!A->IsMember && !B->IsMember)
    return A->Kind == B->Kind;
  return false;
}

const LocalDecl *
findMatchingDefinition(const LocalDecl *Decl,
                       const std::vector<LexicalScope> &Scopes) {
  if (!Decl || Decl->IsDefinition)
    return nullptr;
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (!D.IsDefinition || D.Name != Decl->Name)
        continue;
      if (Decl->IsMember) {
        if (D.IsMember && !Decl->EnclosingClass.empty() &&
            D.EnclosingClass == Decl->EnclosingClass)
          return &D;
      } else {
        if (!D.IsMember && D.Kind == Decl->Kind)
          return &D;
      }
    }
  }
  return nullptr;
}

const LocalDecl *
findMatchingDeclaration(const LocalDecl *Def,
                        const std::vector<LexicalScope> &Scopes) {
  if (!Def || !Def->IsDefinition)
    return nullptr;
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (D.IsDefinition || D.Name != Def->Name)
        continue;
      if (Def->IsMember) {
        if (D.IsMember && !Def->EnclosingClass.empty() &&
            D.EnclosingClass == Def->EnclosingClass)
          return &D;
      } else {
        if (!D.IsMember && D.Kind == Def->Kind)
          return &D;
      }
    }
  }
  return nullptr;
}

} // namespace clangd
} // namespace clang
