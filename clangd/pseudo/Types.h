//===--- Types.h - Pseudo-parser type deduction and resolution -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_TYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_TYPES_H

#include "pseudo/Headers.h"
#include "pseudo/Parse.h"
#include "pseudo/Scopes.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <string>
#include <vector>

namespace clang {
namespace clangd {

class PseudoModule;

std::string unwrapType(llvm::StringRef TypeName);

bool isAutoTypeName(llvm::StringRef T);

std::string deduceElementType(llvm::StringRef ContainerType);

std::string resolveExprType(
    PseudoModule &Self, llvm::StringRef Pre, size_t CursorOffset,
    size_t BestScope, const std::vector<LexicalScope> &Scopes,
    llvm::StringRef Code, PathRef File, llvm::vfs::FileSystem *FS,
    llvm::StringRef EffectiveEnclosingClass = "");

const LocalDecl *resolveTargetDecl(
    const pseudo::Token *Touched, size_t BestScope,
    const std::vector<LexicalScope> &Scopes, const ParseOutput &Parsed,
    llvm::StringRef Code, bool ExpectsType = false);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_TYPES_H
