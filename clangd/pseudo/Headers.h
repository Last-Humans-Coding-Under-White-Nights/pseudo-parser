//===--- Headers.h - Header tracking and include resolution -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_HEADERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_HEADERS_H

#include "GlobalCompilationDatabase.h"
#include "Protocol.h"
#include "pseudo/Scopes.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

class PseudoModule;

struct IncludeDirective {
  std::string Written;
  bool IsAngled = false;
  int HashLine = -1;
};

struct HeaderDecl {
  std::string Name;
  Range NameRange;
  Range ScopeRange;
  std::string EnclosingScope;
  std::string EnclosingClass;
  std::string TypeName;
  DeclKind Kind = DeclKind::Unknown;
  bool IsDefinition = false;
};

struct HeaderInfo {
  std::string Path;
  std::vector<HeaderDecl> Decls;
  std::vector<IncludeDirective> Includes;
};

std::vector<IncludeDirective> extractIncludes(llvm::StringRef Code);

void extractIncludeDirsFromArgs(llvm::ArrayRef<std::string> Args,
                                llvm::StringRef WorkingDir,
                                std::vector<std::string> &IncludeDirs);

std::vector<std::string>
getIncludeDirectories(PathRef File, const GlobalCompilationDatabase *CDB);

std::string resolveHeader(const IncludeDirective &Inc,
                          llvm::StringRef CurrentDir,
                          llvm::ArrayRef<std::string> IncludeDirs,
                          llvm::vfs::FileSystem &FS);

std::shared_ptr<const HeaderInfo>
getHeaderInfo(llvm::StringRef HeaderPath, llvm::vfs::FileSystem &FS,
              llvm::StringMap<std::shared_ptr<const HeaderInfo>> &Cache,
              std::mutex &CacheMutex);

void traverseIncludedHeaders(
    PseudoModule &Self, PathRef File, llvm::StringRef Code,
    llvm::vfs::FileSystem &FS,
    llvm::function_ref<bool(const HeaderInfo &Info, llvm::StringRef HeaderPath)>
        Callback,
    size_t MaxHeaders = 60, int MaxDepth = 4);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDO_HEADERS_H
