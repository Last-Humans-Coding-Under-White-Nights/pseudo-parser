//===--- Scopes.h - Lexical scoping and declaration lookup ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_SCOPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_SCOPES_H

#include "Protocol.h"
#include "pseudo/Parse.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace clang {
namespace clangd {

enum class DeclKind : uint8_t {
  Unknown,
  Variable,
  Parameter,
  Function,
  Constructor,
  Class,
  Enum,
  EnumValue,
  TypeAlias,
  Namespace,
  TemplateParam,
  Concept,
};

inline bool isTypeDecl(DeclKind K) {
  return K == DeclKind::Class || K == DeclKind::Enum ||
         K == DeclKind::TypeAlias || K == DeclKind::TemplateParam ||
         K == DeclKind::Namespace || K == DeclKind::Concept;
}

inline bool isValueDecl(DeclKind K) {
  return K == DeclKind::Variable || K == DeclKind::Parameter ||
         K == DeclKind::EnumValue;
}

inline bool isFunctionDecl(DeclKind K) {
  return K == DeclKind::Function || K == DeclKind::Constructor;
}

enum class ScopeKind {
  File,
  Namespace,
  Class,
  Function,
  Block,
  Template,
};

struct LocalDecl {
  std::string Name;
  std::string TypeName;
  Range NameRange;
  Range DeclRange;
  size_t DeclOffset = 0;
  size_t ScopeId = 0;
  std::string EnclosingClass;
  bool IsParameter = false;
  bool IsMember = false;
  bool IsDefinition = false;
  bool IsCapture = false;
  bool IsInitCapture = false;
  size_t CapturedDeclOffset = 0;
  DeclKind Kind = DeclKind::Unknown;
  std::string InitExpr;
  bool IsRangeFor = false;
};

struct LexicalScope {
  size_t Id = 0;
  size_t ParentId = 0;
  ScopeKind Kind = ScopeKind::File;
  std::string Name;
  Range ScopeRange;
  size_t StartOffset = 0;
  size_t EndOffset = std::numeric_limits<size_t>::max();
  std::string EnclosingClass;
  std::vector<LocalDecl> Decls;
  std::vector<size_t> Children;
};

void buildScopes(const pseudo::ForestNode *N, pseudo::Token::Index End,
                 const ParseOutput &Out, llvm::StringRef Code,
                 std::vector<LexicalScope> &Scopes, size_t &CurrentScopeId,
                 llvm::StringRef EnclosingClass = "",
                 llvm::StringRef DeclaredType = "");


const LocalDecl *lookupDecl(size_t ScopeId, llvm::StringRef Name,
                            size_t AtOffset,
                            const std::vector<LexicalScope> &Scopes,
                            llvm::StringRef Code,
                            bool ExpectsType = false);

const LocalDecl *findAnyDecl(llvm::StringRef Name,
                             const std::vector<LexicalScope> &Scopes,
                             bool ExpectsType = false);

bool isSameEntity(const LocalDecl *A, const LocalDecl *B);

const LocalDecl *
findMatchingDefinition(const LocalDecl *Decl,
                       const std::vector<LexicalScope> &Scopes);

const LocalDecl *
findMatchingDeclaration(const LocalDecl *Def,
                        const std::vector<LexicalScope> &Scopes);

void scanOpaqueDeclarations(pseudo::Token::Index StartTok,
                            pseudo::Token::Index EndTok,
                            const ParseOutput &Out, llvm::StringRef Code,
                            std::vector<LexicalScope> &Scopes,
                            size_t CurrentScopeId,
                            llvm::StringRef EnclosingClass = "");

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_SCOPES_H
