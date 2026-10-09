//===--- AST.h - Pseudo-parser AST node construction --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_AST_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_AST_H

#include "Protocol.h"
#include "pseudo/Parse.h"
#include "llvm/Support/Error.h"
#include <optional>

namespace clang {
namespace clangd {

llvm::Expected<std::optional<ASTNode>>
getAST(llvm::StringRef File, llvm::StringRef Code, std::optional<Range> R);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_AST_H
