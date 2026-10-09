//===--- Parse.h - Pseudo-parser parsing utilities -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_PARSE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_PARSE_H

#include "Protocol.h"
#include "clang-pseudo/Bracket.h"
#include "clang-pseudo/DirectiveTree.h"
#include "clang-pseudo/Disambiguate.h"
#include "clang-pseudo/Forest.h"
#include "clang-pseudo/GLR.h"
#include "clang-pseudo/Language.h"
#include "clang-pseudo/Token.h"
#include "clang-pseudo/cxx/CXX.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TokenKinds.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <string>

namespace clang {
namespace clangd {

struct ParseOutput {
  std::string CodeStorage;
  pseudo::ForestArena Arena;
  pseudo::GSS GSS;
  const pseudo::ForestNode *Root = nullptr;
  pseudo::Disambiguation Disambig;
  pseudo::TokenStream RawStream;
  pseudo::TokenStream ParseableStream;
  pseudo::DirectiveTree Directives;
};

std::unique_ptr<ParseOutput> parseCode(llvm::StringRef Code);

const pseudo::Token &getOrigToken(const pseudo::Token &T,
                                  const ParseOutput &Out);

size_t tokenStartOffset(const pseudo::Token &T, const ParseOutput &Out);
size_t tokenEndOffset(const pseudo::Token &T, const ParseOutput &Out);

Range tokenRange(const pseudo::Token &T, const ParseOutput &Out,
                 llvm::StringRef Code);

Range nodeRange(pseudo::Token::Index StartIdx, pseudo::Token::Index EndIdx,
                const ParseOutput &Out, llvm::StringRef Code);

const pseudo::Token *findTouchedIdentifier(const ParseOutput &Out,
                                           size_t Offset);

const pseudo::Token *getPrevNonComment(size_t Idx, const ParseOutput &Out);
const pseudo::Token *getNextNonComment(size_t Idx, const ParseOutput &Out);

bool isTypeContext(const pseudo::Token *Touched, const ParseOutput &Out);
bool isCtorMemberInitializerName(const pseudo::Token *Touched,
                                 const ParseOutput &Out);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_PARSE_H
