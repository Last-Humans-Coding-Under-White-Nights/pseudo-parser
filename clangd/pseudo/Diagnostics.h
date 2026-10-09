//===--- Diagnostics.h - Pseudo-parser diagnostics generation -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_DIAGNOSTICS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_DIAGNOSTICS_H

#include "Protocol.h"
#include "pseudo/Parse.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace clang {
namespace clangd {

/// Analyzes the parse output of the pseudo-parser and generates diagnostics
/// for bracket mismatches (unclosed/unmatched braces, parentheses, brackets)
/// and syntax errors that do not fit the C++ grammar (opaque recovery nodes).
std::vector<Diagnostic> getDiagnostics(const ParseOutput &Parsed,
                                       llvm::StringRef Code);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_DIAGNOSTICS_H
