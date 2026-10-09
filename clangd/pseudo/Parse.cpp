//===--- Parse.cpp - Pseudo-parser parsing utilities ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pseudo/Parse.h"
#include "SourceCode.h"

namespace clang {
namespace clangd {

std::unique_ptr<ParseOutput> parseCode(llvm::StringRef Code) {
  clang::LangOptions LangOpts = pseudo::genericLangOpts(
      clang::Language::CXX, clang::LangStandard::lang_cxx20);
  auto Out = std::make_unique<ParseOutput>();
  Out->CodeStorage = Code.str();
  Out->RawStream = pseudo::lex(Out->CodeStorage, LangOpts);
  auto DirectiveStructure = pseudo::DirectiveTree::parse(Out->RawStream);
  pseudo::chooseConditionalBranches(DirectiveStructure, Out->RawStream);
  auto StrippedStream = DirectiveStructure.stripDirectives(Out->RawStream);
  Out->Directives = std::move(DirectiveStructure);
  Out->ParseableStream = pseudo::stripAttributes(
      pseudo::stripComments(pseudo::cook(StrippedStream, LangOpts)));
  pseudo::pairBrackets(Out->ParseableStream);

  const auto &Lang = pseudo::cxx::getLanguage();
  std::optional<pseudo::SymbolID> StartSym =
      Lang.G.findNonterminal("translation-unit");
  if (!StartSym)
    return nullptr;

  bool HasTokens = false;
  for (const auto &Tok : Out->ParseableStream.tokens()) {
    if (Tok.Kind != tok::eof && Tok.Kind != tok::comment) {
      HasTokens = true;
      break;
    }
  }

  if (!HasTokens) {
    // Empty file or comments/whitespace only: valid empty translation unit!
    return Out;
  }

  Out->Root = &pseudo::glrParse(
      pseudo::ParseParams{Out->ParseableStream, Out->Arena, Out->GSS},
      *StartSym, Lang);
  if (!Out->Root)
    return nullptr;

  Out->Disambig = pseudo::disambiguate(Out->Root, {});
  return Out;
}

const pseudo::Token &getOrigToken(const pseudo::Token &T,
                                  const ParseOutput &Out) {
  if (T.OriginalIndex != pseudo::Token::Invalid &&
      T.OriginalIndex < Out.RawStream.tokens().size())
    return Out.RawStream.tokens()[T.OriginalIndex];
  return T;
}

size_t tokenStartOffset(const pseudo::Token &T, const ParseOutput &Out) {
  const auto &Orig = getOrigToken(T, Out);
  if (Orig.Data && Orig.Data >= Out.CodeStorage.data() &&
      Orig.Data <= Out.CodeStorage.data() + Out.CodeStorage.size())
    return Orig.Data - Out.CodeStorage.data();
  return 0;
}

size_t tokenEndOffset(const pseudo::Token &T, const ParseOutput &Out) {
  const auto &Orig = getOrigToken(T, Out);
  size_t Start = tokenStartOffset(T, Out);
  return Start + Orig.Length;
}

Range tokenRange(const pseudo::Token &T, const ParseOutput &Out,
                 llvm::StringRef Code) {
  size_t Start = tokenStartOffset(T, Out);
  size_t End = tokenEndOffset(T, Out);
  return Range{offsetToPosition(Code, Start), offsetToPosition(Code, End)};
}

Range nodeRange(pseudo::Token::Index StartIdx, pseudo::Token::Index EndIdx,
                const ParseOutput &Out, llvm::StringRef Code) {
  if (StartIdx >= EndIdx || StartIdx >= Out.ParseableStream.tokens().size())
    return Range{};
  size_t Start =
      tokenStartOffset(Out.ParseableStream.tokens()[StartIdx], Out);
  size_t End =
      tokenEndOffset(Out.ParseableStream.tokens()[EndIdx - 1], Out);
  return Range{offsetToPosition(Code, Start), offsetToPosition(Code, End)};
}

const pseudo::Token *findTouchedIdentifier(const ParseOutput &Out,
                                           size_t Offset) {
  const pseudo::Token *Best = nullptr;
  for (const auto &T : Out.RawStream.tokens()) {
    if (T.Kind != tok::raw_identifier && T.Kind != tok::identifier)
      continue;
    size_t Start = tokenStartOffset(T, Out);
    size_t End = tokenEndOffset(T, Out);
    if (Offset >= Start && Offset <= End) {
      Best = &T;
      if (Offset < End)
        return Best;
    }
  }
  return Best;
}

const pseudo::Token *getPrevNonComment(size_t Idx, const ParseOutput &Out) {
  while (Idx > 0) {
    --Idx;
    const auto &T = Out.RawStream.tokens()[Idx];
    if (T.Kind != tok::comment)
      return &T;
  }
  return nullptr;
}

const pseudo::Token *getNextNonComment(size_t Idx, const ParseOutput &Out) {
  size_t N = Out.RawStream.tokens().size();
  while (Idx + 1 < N) {
    ++Idx;
    const auto &T = Out.RawStream.tokens()[Idx];
    if (T.Kind != tok::comment)
      return &T;
  }
  return nullptr;
}

bool isTypeContext(const pseudo::Token *Touched, const ParseOutput &Out) {
  if (!Touched)
    return false;
  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Out.RawStream.tokens().size()) {
    RawTok = &Out.RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Out.RawStream.tokens().data() &&
             Touched < Out.RawStream.tokens().data() +
                           Out.RawStream.tokens().size()) {
    RawTok = Touched;
  }
  if (!RawTok)
    return false;
  size_t Idx = Out.RawStream.index(*RawTok);
  const auto *Prev = getPrevNonComment(Idx, Out);
  const auto *Next = getNextNonComment(Idx, Out);

  // 1. If followed by :: (scope resolution), e.g. ClangdServer::optsForTest
  if (Next && Next->Kind == tok::coloncolon)
    return true;

  // 2. If followed by pointer/reference/rvalue reference, e.g. ClangdServer *Server, ParseInputs &Inputs
  if (Next && (Next->Kind == tok::star || Next->Kind == tok::amp ||
               Next->Kind == tok::ampamp))
    return true;

  // 3. If followed by another identifier, e.g. "ClangdServer Server", "PathRef File"
  if (Next && (Next->Kind == tok::raw_identifier || Next->Kind == tok::identifier)) {
    if (!Prev || (Prev->Kind != tok::raw_identifier && Prev->Kind != tok::identifier))
      return true;
  }

  // 4. Preceding type keywords
  if (Prev) {
    if (Prev->Kind == tok::kw_class || Prev->Kind == tok::kw_struct ||
        Prev->Kind == tok::kw_union || Prev->Kind == tok::kw_enum ||
        Prev->Kind == tok::kw_typename || Prev->Kind == tok::kw_using ||
        Prev->Kind == tok::kw_typedef || Prev->Kind == tok::kw_const ||
        Prev->Kind == tok::kw_volatile || Prev->Kind == tok::kw_constexpr ||
        Prev->Kind == tok::kw_new || Prev->Kind == tok::kw_public ||
        Prev->Kind == tok::kw_protected || Prev->Kind == tok::kw_private ||
        Prev->Kind == tok::kw_namespace)
      return true;
  }

  // 5. Inside template argument list < ... >
  int AngleDepth = 0;
  bool InTemplateArgs = false;
  for (size_t I = Idx; I > 0; --I) {
    const auto &T = Out.RawStream.tokens()[I - 1];
    if (T.Kind == tok::semi || T.Kind == tok::l_brace || T.Kind == tok::r_brace)
      break;
    if (T.Kind == tok::greater)
      ++AngleDepth;
    else if (T.Kind == tok::less) {
      if (AngleDepth > 0)
        --AngleDepth;
      else {
        InTemplateArgs = true;
        break;
      }
    }
  }
  if (InTemplateArgs)
    return true;

  // 6. Parameter list type, e.g. [](llvm::StringRef) or void foo(llvm::StringRef)
  if (Next && (Next->Kind == tok::r_paren || Next->Kind == tok::comma)) {
    int ParenDepth = 1;
    for (size_t I = Idx; I > 0; --I) {
      const auto &T = Out.RawStream.tokens()[I - 1];
      if (T.Kind == tok::semi || T.Kind == tok::l_brace || T.Kind == tok::r_brace)
        break;
      if (T.Kind == tok::r_paren)
        ++ParenDepth;
      else if (T.Kind == tok::l_paren) {
        --ParenDepth;
        if (ParenDepth == 0) {
          if (I >= 2) {
            const auto &BeforeParen = Out.RawStream.tokens()[I - 2];
            if (BeforeParen.Kind == tok::r_square ||
                BeforeParen.Kind == tok::kw_void ||
                BeforeParen.Kind == tok::kw_bool ||
                BeforeParen.Kind == tok::kw_int ||
                BeforeParen.Kind == tok::kw_char ||
                BeforeParen.Kind == tok::kw_float ||
                BeforeParen.Kind == tok::kw_double) {
              return true;
            }
          }
          break;
        }
      }
    }
  }

  return false;
}

bool isCtorMemberInitializerName(const pseudo::Token *Touched,
                                 const ParseOutput &Out) {
  if (!Touched)
    return false;
  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Out.RawStream.tokens().size()) {
    RawTok = &Out.RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Out.RawStream.tokens().data() &&
             Touched < Out.RawStream.tokens().data() +
                           Out.RawStream.tokens().size()) {
    RawTok = Touched;
  }
  if (!RawTok)
    return false;

  size_t Idx = Out.RawStream.index(*RawTok);
  const auto *Next = getNextNonComment(Idx, Out);
  if (!Next || (Next->Kind != tok::l_paren && Next->Kind != tok::l_brace))
    return false;

  const auto *Prev = getPrevNonComment(Idx, Out);
  if (!Prev || (Prev->Kind != tok::colon && Prev->Kind != tok::comma))
    return false;

  int Depth = 0;
  bool FoundColon = false;
  size_t PrevIdx = Out.RawStream.index(*Prev);
  if (Prev->Kind == tok::colon) {
    FoundColon = true;
  } else {
    size_t Step = 1;
    while (PrevIdx >= Step) {
      const auto &T = Out.RawStream.tokens()[PrevIdx - Step];
      ++Step;
      if (T.Kind == tok::comment)
        continue;
      if (T.Kind == tok::r_paren || T.Kind == tok::r_brace)
        ++Depth;
      else if (T.Kind == tok::l_paren || T.Kind == tok::l_brace) {
        if (Depth > 0)
          --Depth;
        else
          return false;
      } else if (Depth == 0 && T.Kind == tok::colon) {
        FoundColon = true;
        PrevIdx = PrevIdx - Step + 1;
        break;
      } else if (Depth == 0 && (T.Kind == tok::semi || T.Kind == tok::l_brace)) {
        return false;
      }
    }
  }

  if (!FoundColon)
    return false;

  size_t Step = 1;
  while (PrevIdx >= Step) {
    const auto &T = Out.RawStream.tokens()[PrevIdx - Step];
    ++Step;
    if (T.Kind == tok::comment)
      continue;
    if (T.Kind == tok::r_paren)
      return true;
    if (T.Kind == tok::kw_noexcept || T.Kind == tok::raw_identifier ||
        T.Kind == tok::identifier)
      continue;
    if (T.Kind == tok::semi || T.Kind == tok::l_brace || T.Kind == tok::r_brace)
      return false;
  }
  return false;
}

} // namespace clangd
} // namespace clang
