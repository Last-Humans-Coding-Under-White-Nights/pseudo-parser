//===--- Headers.cpp - Header tracking and include resolution -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pseudo/Headers.h"
#include "pseudo/Parse.h"
#include "pseudo/Scopes.h"
#include "PseudoModule.h"
#include "SourceCode.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <queue>

namespace clang {
namespace clangd {

void extractIncludeDirsFromArgs(
    llvm::ArrayRef<std::string> Args, llvm::StringRef WorkingDir,
    std::vector<std::string> &IncludeDirs) {
  for (size_t I = 0; I < Args.size(); ++I) {
    llvm::StringRef Arg = Args[I];
    llvm::StringRef Dir;
    if (Arg.starts_with("-I") || Arg.starts_with("/I")) {
      if (Arg.size() > 2) {
        Dir = Arg.drop_front(2);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    } else if (Arg.starts_with("-isystem")) {
      if (Arg.size() > 8) {
        Dir = Arg.drop_front(8);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    } else if (Arg.starts_with("-iquote")) {
      if (Arg.size() > 7) {
        Dir = Arg.drop_front(7);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    } else if (Arg.starts_with("-idirafter")) {
      if (Arg.size() > 10) {
        Dir = Arg.drop_front(10);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    }

    if (!Dir.empty()) {
      llvm::SmallString<256> AbsPath(Dir);
      if (!llvm::sys::path::is_absolute(AbsPath) && !WorkingDir.empty()) {
        llvm::sys::path::make_absolute(WorkingDir, AbsPath);
      }
      llvm::sys::path::remove_dots(AbsPath, /*remove_dot_dot=*/true);
      std::string S = AbsPath.str().str();
      if (std::find(IncludeDirs.begin(), IncludeDirs.end(), S) == IncludeDirs.end())
        IncludeDirs.push_back(std::move(S));
    }
  }
}

std::vector<std::string>
getIncludeDirectories(PathRef File, const GlobalCompilationDatabase *CDB) {
  std::vector<std::string> IncludeDirs;
  llvm::StringRef ParentDir = llvm::sys::path::parent_path(File);
  if (!ParentDir.empty())
    IncludeDirs.push_back(ParentDir.str());

  if (CDB) {
    auto Cmd = CDB->getCompileCommand(File);
    if (!Cmd) {
      llvm::SmallString<256> CppCandidate(File);
      llvm::sys::path::replace_extension(CppCandidate, "cpp");
      Cmd = CDB->getCompileCommand(CppCandidate);
      if (!Cmd) {
        CppCandidate = File;
        llvm::sys::path::replace_extension(CppCandidate, "cc");
        Cmd = CDB->getCompileCommand(CppCandidate);
      }
      if (!Cmd) {
        CppCandidate = File;
        llvm::sys::path::replace_extension(CppCandidate, "c");
        Cmd = CDB->getCompileCommand(CppCandidate);
      }
    }

    if (Cmd)
      extractIncludeDirsFromArgs(Cmd->CommandLine, Cmd->Directory, IncludeDirs);
  }

  static const char *SystemDirs[] = {
      "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1",
      "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include",
      "/Library/Developer/CommandLineTools/usr/include/c++/v1",
      "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1",
      "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include",
      "/opt/homebrew/include",
      "/usr/include/c++/v1",
      "/usr/include/c++/14",
      "/usr/include/c++/13",
      "/usr/include/c++/12",
      "/usr/include/c++/11",
      "/usr/include/x86_64-linux-gnu/c++/14",
      "/usr/include/x86_64-linux-gnu/c++/13",
      "/usr/include/x86_64-linux-gnu/c++/12",
      "/usr/include/x86_64-linux-gnu/c++/11",
      "/usr/local/include",
      "/usr/include",
  };
  for (const char *SysDir : SystemDirs) {
    if (std::find(IncludeDirs.begin(), IncludeDirs.end(), SysDir) == IncludeDirs.end())
      IncludeDirs.push_back(SysDir);
  }

  return IncludeDirs;
}

std::vector<IncludeDirective> extractIncludes(llvm::StringRef Code) {
  std::vector<IncludeDirective> Results;
  int LineNo = 0;
  while (!Code.empty()) {
    auto [Line, Rest] = Code.split('\n');
    Code = Rest;

    llvm::StringRef Trimmed = Line.ltrim(" \t");
    if (Trimmed.consume_front("#")) {
      Trimmed = Trimmed.ltrim(" \t");
      if (Trimmed.consume_front("include_next") ||
          Trimmed.consume_front("include")) {
        Trimmed = Trimmed.ltrim(" \t");
        if (Trimmed.consume_front("\"")) {
          auto EndQuote = Trimmed.find('\"');
          if (EndQuote != llvm::StringRef::npos) {
            IncludeDirective Inc;
            Inc.Written = Trimmed.slice(0, EndQuote).str();
            Inc.IsAngled = false;
            Inc.HashLine = LineNo;
            Results.push_back(std::move(Inc));
          }
        } else if (Trimmed.consume_front("<")) {
          auto EndAngle = Trimmed.find('>');
          if (EndAngle != llvm::StringRef::npos) {
            IncludeDirective Inc;
            Inc.Written = Trimmed.slice(0, EndAngle).str();
            Inc.IsAngled = true;
            Inc.HashLine = LineNo;
            Results.push_back(std::move(Inc));
          }
        }
      }
    }
    ++LineNo;
  }
  return Results;
}

std::string resolveHeader(const IncludeDirective &Inc,
                          llvm::StringRef CurrentDir,
                          llvm::ArrayRef<std::string> IncludeDirs,
                          llvm::vfs::FileSystem &FS) {
  auto Exists = [&](llvm::StringRef P) -> bool {
    auto Stat = FS.status(P);
    return Stat && !Stat->isDirectory();
  };

  if (!Inc.IsAngled) {
    llvm::SmallString<256> Cand(CurrentDir);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();
  }

  for (const auto &Dir : IncludeDirs) {
    llvm::SmallString<256> Cand(Dir);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();
  }

  if (Inc.IsAngled) {
    llvm::SmallString<256> Cand(CurrentDir);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();
  }

  // Walk up parent directories of CurrentDir to locate include roots or project roots
  llvm::SmallString<256> Cur(CurrentDir);
  while (!Cur.empty()) {
    llvm::SmallString<256> Cand(Cur);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();

    llvm::SmallString<256> CandInc(Cur);
    llvm::sys::path::append(CandInc, "include", Inc.Written);
    llvm::sys::path::remove_dots(CandInc, /*remove_dot_dot=*/true);
    if (Exists(CandInc))
      return CandInc.str().str();

    llvm::SmallString<256> CandClang(Cur);
    llvm::sys::path::append(CandClang, "clang", "include", Inc.Written);
    llvm::sys::path::remove_dots(CandClang, /*remove_dot_dot=*/true);
    if (Exists(CandClang))
      return CandClang.str().str();

    llvm::SmallString<256> CandLLVM(Cur);
    llvm::sys::path::append(CandLLVM, "llvm", "include", Inc.Written);
    llvm::sys::path::remove_dots(CandLLVM, /*remove_dot_dot=*/true);
    if (Exists(CandLLVM))
      return CandLLVM.str().str();

    std::string Parent = llvm::sys::path::parent_path(Cur).str();
    if (Parent == Cur)
      break;
    Cur = Parent;
  }

  return "";
}

std::shared_ptr<const HeaderInfo>
getHeaderInfo(llvm::StringRef HeaderPath, llvm::vfs::FileSystem &FS,
              llvm::StringMap<std::shared_ptr<const HeaderInfo>> &Cache,
              std::mutex &CacheMutex) {
  {
    std::lock_guard<std::mutex> Lock(CacheMutex);
    auto It = Cache.find(HeaderPath);
    if (It != Cache.end())
      return It->second;
  }

  auto Info = std::make_shared<HeaderInfo>();
  Info->Path = HeaderPath.str();

  auto Buffer = FS.getBufferForFile(HeaderPath);
  if (!Buffer || !*Buffer) {
    std::lock_guard<std::mutex> Lock(CacheMutex);
    Cache[HeaderPath] = Info;
    return Info;
  }

  llvm::StringRef Code = (*Buffer)->getBuffer();
  Info->Includes = extractIncludes(Code);

  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root) {
    std::lock_guard<std::mutex> Lock(CacheMutex);
    Cache[HeaderPath] = Info;
    return Info;
  }

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

  for (const auto &Scope : Scopes) {
    for (const auto &D : Scope.Decls) {
      if (D.Kind == DeclKind::Parameter || D.IsParameter)
        continue;
      HeaderDecl HD;
      HD.Name = D.Name;
      HD.NameRange = D.NameRange;
      HD.ScopeRange = D.DeclRange;
      std::string EnclNs;
      for (size_t Cur = Scope.Id; Cur != 0; Cur = Scopes[Cur].ParentId) {
        if (Scopes[Cur].Kind == ScopeKind::Namespace && !Scopes[Cur].Name.empty()) {
          EnclNs = Scopes[Cur].Name;
          break;
        }
      }
      HD.EnclosingScope =
          (Scope.Kind == ScopeKind::Namespace && !Scope.Name.empty())
              ? Scope.Name
              : EnclNs;
      HD.EnclosingClass =
          !D.EnclosingClass.empty()
              ? D.EnclosingClass
              : (Scope.Kind == ScopeKind::Class ? Scope.Name : "");
      HD.TypeName = D.TypeName;
      HD.Kind = D.Kind;
      HD.IsDefinition = D.IsDefinition;
      Info->Decls.push_back(std::move(HD));
    }
  }

  {
    std::lock_guard<std::mutex> Lock(CacheMutex);
    Cache[HeaderPath] = Info;
  }
  return Info;
}

void traverseIncludedHeaders(
    PseudoModule &Self, PathRef File, llvm::StringRef Code,
    llvm::vfs::FileSystem &FS,
    llvm::function_ref<bool(const HeaderInfo &Info, llvm::StringRef HeaderPath)>
        Callback,
    size_t MaxHeaders, int MaxDepth) {
  auto Includes = extractIncludes(Code);
  auto IncludeDirs = Self.getIncludeDirectories(File);
  std::string CurrentDir = llvm::sys::path::parent_path(File).str();

  std::queue<std::pair<std::string, int>> Queue;
  llvm::StringSet<> Visited;

  for (const auto &Inc : Includes) {
    if (!Inc.IsAngled) {
      std::string Resolved =
          Self.resolveHeader(Inc, CurrentDir, IncludeDirs, FS);
      if (!Resolved.empty() && Visited.insert(Resolved).second)
        Queue.push({Resolved, 1});
    }
  }
  for (const auto &Inc : Includes) {
    if (Inc.IsAngled) {
      std::string Resolved =
          Self.resolveHeader(Inc, CurrentDir, IncludeDirs, FS);
      if (!Resolved.empty() && Visited.insert(Resolved).second)
        Queue.push({Resolved, 1});
    }
  }

  size_t HeadersVisited = 0;
  while (!Queue.empty() && HeadersVisited < MaxHeaders) {
    auto [HeaderPath, Depth] = Queue.front();
    Queue.pop();
    ++HeadersVisited;

    auto Info = Self.getHeaderInfo(HeaderPath, FS);
    if (!Info)
      continue;

    if (Callback(*Info, HeaderPath))
      break;

    bool IsSys = HeaderPath.rfind("/usr/", 0) == 0 ||
                 HeaderPath.rfind("/Library/Developer/", 0) == 0 ||
                 HeaderPath.rfind("/Applications/Xcode.app/", 0) == 0 ||
                 HeaderPath.rfind("/opt/homebrew/", 0) == 0;
    if (Depth < MaxDepth && !IsSys) {
      std::string HDir = llvm::sys::path::parent_path(HeaderPath).str();
      for (const auto &SubInc : Info->Includes) {
        if (!SubInc.IsAngled) {
          std::string SubResolved =
              Self.resolveHeader(SubInc, HDir, IncludeDirs, FS);
          if (!SubResolved.empty() && Visited.insert(SubResolved).second)
            Queue.push({SubResolved, Depth + 1});
        }
      }
      for (const auto &SubInc : Info->Includes) {
        if (SubInc.IsAngled) {
          std::string SubResolved =
              Self.resolveHeader(SubInc, HDir, IncludeDirs, FS);
          if (!SubResolved.empty() && Visited.insert(SubResolved).second)
            Queue.push({SubResolved, Depth + 1});
        }
      }
    }
  }
}

} // namespace clangd
} // namespace clang
