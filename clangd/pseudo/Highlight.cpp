//===--- Highlight.cpp - Pseudo-parser semantic highlighting ------*- C++ -*-===//
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

namespace clang {
namespace clangd {

llvm::Expected<std::vector<HighlightingToken>>
PseudoModule::getSemanticHighlightings(llvm::StringRef Code) {
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<HighlightingToken> Tokens;
  if (!Parsed->Root)
    return Tokens;

  // Build scopes and decls
  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange =
      Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  // Set to avoid duplicate tokens at the exact same location
  llvm::DenseMap<size_t, const LocalDecl *> DeclByOffset;

  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      HighlightingToken Tok;
      Tok.R = D.NameRange;
      if (Tok.R.start.line != Tok.R.end.line ||
          Tok.R.start.character >= Tok.R.end.character)
        continue;

      DeclByOffset[D.DeclOffset] = &D;

      switch (D.Kind) {
      case DeclKind::Class:
        Tok.Kind = HighlightingKind::Class;
        break;
      case DeclKind::Enum:
        Tok.Kind = HighlightingKind::Enum;
        break;
      case DeclKind::EnumValue:
        Tok.Kind = HighlightingKind::EnumConstant;
        break;
      case DeclKind::TypeAlias:
        Tok.Kind = HighlightingKind::Typedef;
        break;
      case DeclKind::TemplateParam:
        Tok.Kind = HighlightingKind::TemplateParameter;
        break;
      case DeclKind::Concept:
        Tok.Kind = HighlightingKind::Concept;
        break;
      case DeclKind::Namespace:
        Tok.Kind = HighlightingKind::Namespace;
        break;
      case DeclKind::Function:
        Tok.Kind =
            D.IsMember ? HighlightingKind::Method : HighlightingKind::Function;
        break;
      case DeclKind::Constructor:
        Tok.Kind = HighlightingKind::Method;
        Tok.addModifier(HighlightingModifier::ConstructorOrDestructor);
        break;
      case DeclKind::Parameter:
        Tok.Kind = HighlightingKind::Parameter;
        Tok.addModifier(HighlightingModifier::FunctionScope);
        break;
      case DeclKind::Variable:
        if (D.IsMember) {
          Tok.Kind = HighlightingKind::Field;
          Tok.addModifier(HighlightingModifier::ClassScope);
        } else if (S.Kind == ScopeKind::Function || S.Kind == ScopeKind::Block) {
          Tok.Kind = HighlightingKind::LocalVariable;
          Tok.addModifier(HighlightingModifier::FunctionScope);
        } else {
          Tok.Kind = HighlightingKind::Variable;
          Tok.addModifier(HighlightingModifier::FileScope);
        }
        break;
      default:
        Tok.Kind = HighlightingKind::Unknown;
        break;
      }

      Tok.addModifier(HighlightingModifier::Declaration);
      if (D.IsDefinition)
        Tok.addModifier(HighlightingModifier::Definition);
      if (D.IsMember)
        Tok.addModifier(HighlightingModifier::ClassScope);

      Tokens.push_back(std::move(Tok));
    }
  }

  // Scan ParseableStream tokens for references, keywords, primitives, modifiers
  for (const auto &T : Parsed->ParseableStream.tokens()) {
    size_t Off = tokenStartOffset(T, *Parsed);
    Range R = tokenRange(T, *Parsed, Code);
    if (R.start.line != R.end.line || R.start.character >= R.end.character)
      continue;

    // If it is a declaration name we already added, skip it
    if (DeclByOffset.count(Off))
      continue;

    // Check if it's an identifier reference
    if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier) {
      llvm::StringRef TokText = getOrigToken(T, *Parsed).text();
      if (TokText == "override" || TokText == "final") {
        HighlightingToken Tok;
        Tok.Kind = HighlightingKind::Modifier;
        Tok.R = R;
        Tokens.push_back(std::move(Tok));
        continue;
      }
      size_t BestScope = 0;
      size_t BestLen = std::numeric_limits<size_t>::max();
      for (const auto &S : Scopes) {
        if (Off >= S.StartOffset && Off <= S.EndOffset) {
          size_t Len = S.EndOffset - S.StartOffset;
          if (Len < BestLen) {
            BestLen = Len;
            BestScope = S.Id;
          }
        }
      }

      bool ExpectsType = isTypeContext(&T, *Parsed);
      const LocalDecl *TargetDecl =
          resolveTargetDecl(&T, BestScope, Scopes, *Parsed, Code, ExpectsType);
      if (TargetDecl) {
        HighlightingToken RefTok;
        RefTok.R = R;
        switch (TargetDecl->Kind) {
        case DeclKind::Class:
          RefTok.Kind = HighlightingKind::Class;
          break;
        case DeclKind::Enum:
          RefTok.Kind = HighlightingKind::Enum;
          break;
        case DeclKind::EnumValue:
          RefTok.Kind = HighlightingKind::EnumConstant;
          break;
        case DeclKind::TypeAlias:
          RefTok.Kind = HighlightingKind::Typedef;
          break;
        case DeclKind::TemplateParam:
          RefTok.Kind = HighlightingKind::TemplateParameter;
          break;
        case DeclKind::Concept:
          RefTok.Kind = HighlightingKind::Concept;
          break;
        case DeclKind::Namespace:
          RefTok.Kind = HighlightingKind::Namespace;
          break;
        case DeclKind::Function:
        case DeclKind::Constructor:
          RefTok.Kind = TargetDecl->IsMember ? HighlightingKind::Method
                                             : HighlightingKind::Function;
          break;
        case DeclKind::Parameter:
          RefTok.Kind = HighlightingKind::Parameter;
          break;
        case DeclKind::Variable:
          RefTok.Kind = TargetDecl->IsMember ? HighlightingKind::Field
                        : (TargetDecl->IsParameter ? HighlightingKind::Parameter
                                                   : HighlightingKind::LocalVariable);
          break;
        default:
          break;
        }
        if (RefTok.Kind != HighlightingKind::Unknown) {
          Tokens.push_back(std::move(RefTok));
          continue;
        }
      } else {
        if (TokText == "std" || TokText == "llvm" || TokText == "clang" ||
            TokText == "clangd") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Namespace;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
        if (TokText == "move" || TokText == "forward" ||
            TokText == "make_unique" || TokText == "make_shared" ||
            TokText == "swap") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Function;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
        if (TokText == "unique_ptr" || TokText == "shared_ptr" ||
            TokText == "weak_ptr" || TokText == "vector" ||
            TokText == "string" || TokText == "string_view" ||
            TokText == "optional" || TokText == "pair" || TokText == "tuple" ||
            TokText == "map" || TokText == "unordered_map" ||
            TokText == "set" || TokText == "unordered_set") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Class;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
        if (TokText == "first" || TokText == "second") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Field;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
      }
    }

    // Primitives
    switch (T.Kind) {
    case tok::kw_int:
    case tok::kw_char:
    case tok::kw_bool:
    case tok::kw_float:
    case tok::kw_double:
    case tok::kw_void:
    case tok::kw_long:
    case tok::kw_short:
    case tok::kw_unsigned:
    case tok::kw_signed:
    case tok::kw_auto:
    case tok::kw_wchar_t:
    case tok::kw_char16_t:
    case tok::kw_char32_t: {
      HighlightingToken Tok;
      Tok.Kind = HighlightingKind::Primitive;
      Tok.R = R;
      Tokens.push_back(std::move(Tok));
      break;
    }
    // Modifiers
    case tok::kw_const:
    case tok::kw_constexpr:
    case tok::kw_static:
    case tok::kw_virtual:
    case tok::kw_inline:
    case tok::kw_explicit:
    case tok::kw_mutable:
    case tok::kw_volatile:
    case tok::kw_noexcept: {
      HighlightingToken Tok;
      Tok.Kind = HighlightingKind::Modifier;
      Tok.R = R;
      Tokens.push_back(std::move(Tok));
      break;
    }
    default:
      break;
    }
  }

  // Sort tokens by start position
  llvm::sort(Tokens, [](const HighlightingToken &A, const HighlightingToken &B) {
    if (A.R.start.line != B.R.start.line)
      return A.R.start.line < B.R.start.line;
    if (A.R.start.character != B.R.start.character)
      return A.R.start.character < B.R.start.character;
    return A.R.end.character > B.R.end.character;
  });

  // Filter out any overlapping tokens
  std::vector<HighlightingToken> NonOverlapping;
  for (const auto &Tok : Tokens) {
    if (Tok.R.start.line > Tok.R.end.line ||
        (Tok.R.start.line == Tok.R.end.line &&
         Tok.R.start.character >= Tok.R.end.character))
      continue;
    if (!NonOverlapping.empty()) {
      const auto &Last = NonOverlapping.back();
      if (Tok.R.start.line < Last.R.end.line)
        continue;
      if (Tok.R.start.line == Last.R.end.line &&
          Tok.R.start.character < Last.R.end.character)
        continue;
    }
    NonOverlapping.push_back(Tok);
  }

  return NonOverlapping;
}

llvm::Expected<SemanticTokens>
PseudoModule::getSemanticTokens(llvm::StringRef Code) {
  auto HT = getSemanticHighlightings(Code);
  if (!HT)
    return HT.takeError();
  SemanticTokens ST;
  ST.tokens = toSemanticTokens(*HT, Code);
  return ST;
}

} // namespace clangd
} // namespace clang
