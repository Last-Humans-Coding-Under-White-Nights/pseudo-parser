//===--- Completion.cpp - Pseudo-parser code completion -----------*- C++ -*-===//
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
#include "llvm/Support/Path.h"

namespace clang {
namespace clangd {

llvm::Expected<CompletionList>
PseudoModule::getCompletions(PathRef File, llvm::StringRef Code, Position Pos) {
  auto LineStart = positionToOffset(Code, Position{Pos.line, 0});
  auto CursorOffset = positionToOffset(Code, Pos);
  if (!LineStart || !CursorOffset || *CursorOffset < *LineStart)
    return CompletionList{};

  llvm::StringRef LinePrefix = Code.slice(*LineStart, *CursorOffset);

  // 1. Preprocessor directives
  llvm::StringRef LTrim = LinePrefix.ltrim();
  if (LTrim.starts_with("#")) {
    llvm::StringRef Typed = LTrim.drop_front(1).ltrim();
    static const char *Directives[] = {
        "include", "define", "ifdef", "ifndef", "if",
        "elif",    "else",   "endif", "pragma", "undef"};
    CompletionList List;
    for (const char *Dir : Directives) {
      if (llvm::StringRef(Dir).starts_with_insensitive(Typed)) {
        CompletionItem Item;
        Item.label = Dir;
        Item.kind = CompletionItemKind::Keyword;
        Item.filterText = Dir;
        Item.insertText = Dir;
        Item.sortText = "0_" + std::string(Dir);
        List.items.push_back(std::move(Item));
      }
    }
    return List;
  }

  // 2. Analyze line prefix context
  size_t Idx = LinePrefix.size();
  while (Idx > 0 &&
         (llvm::isAlnum(LinePrefix[Idx - 1]) || LinePrefix[Idx - 1] == '_'))
    --Idx;
  llvm::StringRef PartialWord = LinePrefix.substr(Idx);
  llvm::StringRef Pre = LinePrefix.take_front(Idx).rtrim();

  bool IsMemberAccess = false;
  bool IsScopeResolution = false;
  size_t OpOffset = LinePrefix.size();
  if (Pre.ends_with(".")) {
    OpOffset = Pre.size() - 1;
    Pre = Pre.drop_back(1).rtrim();
    IsMemberAccess = true;
  } else if (Pre.ends_with("->")) {
    OpOffset = Pre.size() - 2;
    Pre = Pre.drop_back(2).rtrim();
    IsMemberAccess = true;
  } else if (Pre.ends_with("::")) {
    OpOffset = Pre.size() - 2;
    Pre = Pre.drop_back(2).rtrim();
    IsScopeResolution = true;
  }

  // Sanitize trailing incomplete operator so GLR parser parses the enclosing block
  std::string CodeToParse = Code.str();
  if (IsMemberAccess || IsScopeResolution) {
    size_t AbsOpOffset = *LineStart + OpOffset;
    if (AbsOpOffset < CodeToParse.size()) {
      size_t ParenCount = 0;
      for (size_t I = 0; I < AbsOpOffset; ++I) {
        if (CodeToParse[I] == '(' || CodeToParse[I] == '[')
          ++ParenCount;
        else if (CodeToParse[I] == ')' || CodeToParse[I] == ']') {
          if (ParenCount > 0)
            --ParenCount;
        }
      }
      if (ParenCount > 0) {
        for (size_t I = AbsOpOffset;
             I < *CursorOffset && I < CodeToParse.size(); ++I)
          CodeToParse[I] = ' ';
      } else {
        CodeToParse[AbsOpOffset] = ';';
        for (size_t I = AbsOpOffset + 1;
             I < *CursorOffset && I < CodeToParse.size(); ++I)
          CodeToParse[I] = ' ';
      }
    }
  }

  // 3. Parse code and build scopes
  auto Parsed = parseCode(CodeToParse);
  if (!Parsed || !Parsed->Root) {
    Parsed = parseCode(Code);
    if (!Parsed || !Parsed->Root) {
      std::string SemicolonCode = Code.str();
      if (*CursorOffset <= SemicolonCode.size()) {
        SemicolonCode.insert(*CursorOffset, ";");
        Parsed = parseCode(SemicolonCode);
      }
    }
  }

  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = CodeToParse.size();
  Scopes.back().ScopeRange =
      Range{Position{0, 0}, offsetToPosition(CodeToParse, CodeToParse.size())};

  if (Parsed && Parsed->Root) {
    size_t CurScope = 0;
    pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
    buildScopes(Parsed->Root, NumTokens, *Parsed, CodeToParse, Scopes, CurScope);
  }

  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (*CursorOffset >= S.StartOffset && *CursorOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  std::string EffectiveEnclosingClass;
  size_t SCheck = BestScope;
  while (true) {
    if (!Scopes[SCheck].EnclosingClass.empty()) {
      EffectiveEnclosingClass = Scopes[SCheck].EnclosingClass;
      break;
    }
    if (SCheck == 0)
      break;
    SCheck = Scopes[SCheck].ParentId;
  }

  CompletionList List;
  llvm::StringSet<> SeenLabels;

  if (IsMemberAccess) {
    std::string TypeName = resolveExprType(
        *this, Pre, *CursorOffset, BestScope, Scopes, Code, File, getFS().get(),
        EffectiveEnclosingClass);

    std::string CleanType = unwrapType(TypeName);

    // 1. Members from current file scopes
    if (!CleanType.empty()) {
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.EnclosingClass == CleanType || S.Name == CleanType) {
            if (PartialWord.empty() ||
                llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
              if (SeenLabels.insert(D.Name).second) {
                CompletionItem Item;
                Item.label = D.Name;
                Item.kind = (D.Kind == DeclKind::Function ||
                             D.Kind == DeclKind::Constructor)
                                ? CompletionItemKind::Method
                                : CompletionItemKind::Field;
                Item.detail = D.TypeName;
                Item.sortText = "0_" + D.Name;
                Item.filterText = D.Name;
                Item.insertText = D.Name;
                List.items.push_back(std::move(Item));
              }
            }
          }
        }
      }

      // 2. Members from included headers (direct and transitive)
      auto FS = getFS();
      if (FS) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.EnclosingClass == CleanType ||
                    D.EnclosingScope == CleanType) {
                  if (PartialWord.empty() ||
                      llvm::StringRef(D.Name).starts_with_insensitive(
                          PartialWord)) {
                    if (SeenLabels.insert(D.Name).second) {
                      CompletionItem Item;
                      Item.label = D.Name;
                      Item.kind = (D.Kind == DeclKind::Function ||
                                   D.Kind == DeclKind::Constructor)
                                      ? CompletionItemKind::Method
                                      : CompletionItemKind::Field;
                      Item.detail = D.TypeName;
                      Item.sortText = "0_" + D.Name;
                      Item.filterText = D.Name;
                      Item.insertText = D.Name;
                      List.items.push_back(std::move(Item));
                    }
                  }
                }
              }
              return false;
            });
      }
    }

    // 3. Known methods for common standard/llvm types
    if (CleanType == "entry" || TypeName.find("Registry") != std::string::npos) {
      static const char *EntryMethods[] = {"getName", "getDesc", "instantiate"};
      for (const char *M : EntryMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (CleanType == "StringRef" || CleanType == "string" ||
        CleanType == "string_view" || CleanType == "basic_string" ||
        CleanType == "basic_string_view" ||
        TypeName.find("StringRef") != std::string::npos ||
        TypeName.find("string") != std::string::npos) {
      static const char *StrMethods[] = {
          "data",         "size",         "length",       "empty",
          "str",          "bytes",        "begin",        "end",
          "front",        "back",         "substr",       "slice",
          "starts_with",  "ends_with",    "contains",     "find",
          "rfind",        "count",        "split",        "trim",
          "ltrim",        "rtrim",        "take_front",   "take_back",
          "drop_front",   "drop_back",    "consume_front","consume_back",
          "equals",       "compare",      "c_str",        "clear"};
      for (const char *M : StrMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (CleanType == "pair" || TypeName.find("pair") != std::string::npos) {
      static const char *PairMembers[] = {"first", "second"};
      for (const char *M : PairMembers) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Field;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("vector") != std::string::npos ||
        CleanType == "vector") {
      static const char *VecMethods[] = {
          "push_back", "emplace_back", "pop_back", "size",  "empty",
          "clear",     "begin",        "end",      "front", "back",
          "data",      "resize",       "reserve"};
      for (const char *M : VecMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("DenseMap") != std::string::npos ||
        TypeName.find("map") != std::string::npos || CleanType == "DenseMap") {
      static const char *MapMethods[] = {
          "try_emplace", "insert", "find",  "lookup", "count",
          "empty",       "size",   "clear", "begin",  "end", "erase"};
      for (const char *M : MapMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("optional") != std::string::npos) {
      static const char *OptMethods[] = {"emplace", "has_value", "value", "reset"};
      for (const char *M : OptMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("unique_ptr") != std::string::npos ||
        TypeName.find("shared_ptr") != std::string::npos) {
      static const char *PtrMethods[] = {"get", "reset", "release"};
      for (const char *M : PtrMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }

    // 4. Fallback if still empty (e.g. unknown auto type on M->)
    if (List.items.empty()) {
      auto FS = getFS();
      if (FS) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (!D.EnclosingClass.empty() &&
                    (D.Kind == DeclKind::Function ||
                     D.Kind == DeclKind::Constructor ||
                     D.Kind == DeclKind::Variable)) {
                  if (PartialWord.empty() ||
                      llvm::StringRef(D.Name).starts_with_insensitive(
                          PartialWord)) {
                    if (SeenLabels.insert(D.Name).second) {
                      CompletionItem Item;
                      Item.label = D.Name;
                      Item.kind = (D.Kind == DeclKind::Function ||
                                   D.Kind == DeclKind::Constructor)
                                      ? CompletionItemKind::Method
                                      : CompletionItemKind::Field;
                      Item.detail = D.TypeName;
                      Item.sortText = "0_" + D.Name;
                      Item.filterText = D.Name;
                      Item.insertText = D.Name;
                      List.items.push_back(std::move(Item));
                    }
                  }
                }
              }
              return false;
            });
      }
    }
    return List;
  }

  if (IsScopeResolution) {
    size_t QEnd = Pre.size();
    size_t QStart = QEnd;
    while (QStart > 0 &&
           (llvm::isAlnum(Pre[QStart - 1]) || Pre[QStart - 1] == '_'))
      --QStart;
    llvm::StringRef Qualifier = Pre.slice(QStart, QEnd);
    if (!Qualifier.empty()) {
      // 1. Current file scopes
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.EnclosingClass == Qualifier ||
              (S.Kind == ScopeKind::Namespace && S.Name == Qualifier)) {
            if (PartialWord.empty() ||
                llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
              if (SeenLabels.insert(D.Name).second) {
                CompletionItem Item;
                Item.label = D.Name;
                if (D.Kind == DeclKind::Function ||
                    D.Kind == DeclKind::Constructor)
                  Item.kind = CompletionItemKind::Method;
                else if (D.Kind == DeclKind::Class)
                  Item.kind = CompletionItemKind::Class;
                else if (D.Kind == DeclKind::EnumValue)
                  Item.kind = CompletionItemKind::EnumMember;
                else if (D.Kind == DeclKind::Enum)
                  Item.kind = CompletionItemKind::Enum;
                else
                  Item.kind = CompletionItemKind::Field;
                Item.detail = D.TypeName;
                Item.sortText = "0_" + D.Name;
                Item.filterText = D.Name;
                Item.insertText = D.Name;
                List.items.push_back(std::move(Item));
              }
            }
          }
        }
      }

      if (Qualifier == "std") {
        struct StdCompItem {
          const char *Name;
          CompletionItemKind Kind;
          const char *Detail;
        };
        static const StdCompItem StdItems[] = {
            {"move", CompletionItemKind::Function,
             "template <typename T> constexpr remove_reference_t<T>&& move(T&& t) noexcept"},
            {"forward", CompletionItemKind::Function,
             "template <typename T> constexpr T&& forward(remove_reference_t<T>& t) noexcept"},
            {"make_unique", CompletionItemKind::Function,
             "template <typename T, typename... Args> unique_ptr<T> make_unique(Args&&... args)"},
            {"make_shared", CompletionItemKind::Function,
             "template <typename T, typename... Args> shared_ptr<T> make_shared(Args&&... args)"},
            {"make_pair", CompletionItemKind::Function,
             "template <typename T1, typename T2> constexpr pair<T1, T2> make_pair(T1&& t1, T2&& t2)"},
            {"make_tuple", CompletionItemKind::Function,
             "template <typename... Args> constexpr tuple<Args...> make_tuple(Args&&... args)"},
            {"swap", CompletionItemKind::Function,
             "template <typename T> void swap(T& a, T& b) noexcept"},
            {"to_string", CompletionItemKind::Function,
             "string to_string(int val)"},
            {"min", CompletionItemKind::Function,
             "template <typename T> constexpr const T& min(const T& a, const T& b)"},
            {"max", CompletionItemKind::Function,
             "template <typename T> constexpr const T& max(const T& a, const T& b)"},
            {"clamp", CompletionItemKind::Function,
             "template <typename T> constexpr const T& clamp(const T& v, const T& lo, const T& hi)"},
            {"find", CompletionItemKind::Function,
             "template <typename InputIt, typename T> InputIt find(InputIt first, InputIt last, const T& value)"},
            {"sort", CompletionItemKind::Function,
             "template <typename RandomIt> void sort(RandomIt first, RandomIt last)"},
            {"unique_ptr", CompletionItemKind::Class,
             "template <typename T, typename Deleter = default_delete<T>> class unique_ptr"},
            {"shared_ptr", CompletionItemKind::Class,
             "template <typename T> class shared_ptr"},
            {"weak_ptr", CompletionItemKind::Class,
             "template <typename T> class weak_ptr"},
            {"vector", CompletionItemKind::Class,
             "template <typename T, typename Allocator = allocator<T>> class vector"},
            {"string", CompletionItemKind::Class,
             "using string = basic_string<char>"},
            {"string_view", CompletionItemKind::Class,
             "using string_view = basic_string_view<char>"},
            {"pair", CompletionItemKind::Class,
             "template <typename T1, typename T2> struct pair"},
            {"tuple", CompletionItemKind::Class,
             "template <typename... Types> class tuple"},
            {"optional", CompletionItemKind::Class,
             "template <typename T> class optional"},
            {"variant", CompletionItemKind::Class,
             "template <typename... Types> class variant"},
            {"any", CompletionItemKind::Class,
             "class any"},
            {"function", CompletionItemKind::Class,
             "template <typename> class function"},
            {"map", CompletionItemKind::Class,
             "template <typename Key, typename T, typename Compare = less<Key>> class map"},
            {"unordered_map", CompletionItemKind::Class,
             "template <typename Key, typename T, typename Hash = hash<Key>> class unordered_map"},
            {"set", CompletionItemKind::Class,
             "template <typename Key, typename Compare = less<Key>> class set"},
            {"unordered_set", CompletionItemKind::Class,
             "template <typename Key, typename Hash = hash<Key>> class unordered_set"},
            {"array", CompletionItemKind::Class,
             "template <typename T, size_t N> struct array"},
            {"deque", CompletionItemKind::Class,
             "template <typename T> class deque"},
            {"list", CompletionItemKind::Class,
             "template <typename T> class list"},
            {"queue", CompletionItemKind::Class,
             "template <typename T> class queue"},
            {"stack", CompletionItemKind::Class,
             "template <typename T> class stack"},
            {"span", CompletionItemKind::Class,
             "template <typename T, size_t Extent = dynamic_extent> class span"},
            {"bitset", CompletionItemKind::Class,
             "template <size_t N> class bitset"},
            {"size_t", CompletionItemKind::Interface,
             "using size_t = unsigned long"},
            {"ptrdiff_t", CompletionItemKind::Interface,
             "using ptrdiff_t = long"},
            {"nullptr_t", CompletionItemKind::Interface,
             "using nullptr_t = decltype(nullptr)"},
            {"nullopt", CompletionItemKind::Constant,
             "constexpr nullopt_t nullopt"},
            {"nullopt_t", CompletionItemKind::Class,
             "struct nullopt_t"},
            {"initializer_list", CompletionItemKind::Class,
             "template <typename T> class initializer_list"},
            {"atomic", CompletionItemKind::Class,
             "template <typename T> struct atomic"},
            {"mutex", CompletionItemKind::Class,
             "class mutex"},
            {"lock_guard", CompletionItemKind::Class,
             "template <typename Mutex> class lock_guard"},
            {"unique_lock", CompletionItemKind::Class,
             "template <typename Mutex> class unique_lock"},
            {"thread", CompletionItemKind::Class,
             "class thread"},
        };
        for (const auto &SI : StdItems) {
          if (PartialWord.empty() ||
              llvm::StringRef(SI.Name).starts_with_insensitive(PartialWord)) {
            if (SeenLabels.insert(SI.Name).second) {
              CompletionItem Item;
              Item.label = SI.Name;
              Item.kind = SI.Kind;
              Item.detail = SI.Detail;
              Item.sortText = "0_" + std::string(SI.Name);
              Item.filterText = SI.Name;
              Item.insertText = SI.Name;
              List.items.push_back(std::move(Item));
            }
          }
        }
      }

      // 2. Included headers
      auto FS = getFS();
      if (FS) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.EnclosingClass == Qualifier ||
                    D.EnclosingScope == Qualifier ||
                    (Qualifier == "std" && (D.EnclosingScope == "__1" ||
                                            D.EnclosingScope == "std"))) {
                  if (PartialWord.empty() ||
                      llvm::StringRef(D.Name).starts_with_insensitive(
                          PartialWord)) {
                    if (SeenLabels.insert(D.Name).second) {
                      CompletionItem Item;
                      Item.label = D.Name;
                      if (D.Kind == DeclKind::Function ||
                          D.Kind == DeclKind::Constructor)
                        Item.kind = CompletionItemKind::Method;
                      else if (D.Kind == DeclKind::Class)
                        Item.kind = CompletionItemKind::Class;
                      else if (D.Kind == DeclKind::EnumValue)
                        Item.kind = CompletionItemKind::EnumMember;
                      else if (D.Kind == DeclKind::Enum)
                        Item.kind = CompletionItemKind::Enum;
                      else
                        Item.kind = CompletionItemKind::Field;
                      Item.detail = D.TypeName;
                      Item.sortText = "0_" + D.Name;
                      Item.filterText = D.Name;
                      Item.insertText = D.Name;
                      List.items.push_back(std::move(Item));
                    }
                  }
                }
              }
              return false;
            },
            /*MaxHeaders=*/60, /*MaxDepth=*/3);
      }

      // 3. Known registry methods if qualifier contains Registry
      if (Qualifier.contains("Registry")) {
        static const char *RegItems[] = {"entry", "entries", "add"};
        for (const char *R : RegItems) {
          if (PartialWord.empty() ||
              llvm::StringRef(R).starts_with_insensitive(PartialWord)) {
            if (SeenLabels.insert(R).second) {
              CompletionItem Item;
              Item.label = R;
              Item.kind = (llvm::StringRef(R) == "entry")
                              ? CompletionItemKind::Class
                              : CompletionItemKind::Method;
              Item.sortText = "0_" + std::string(R);
              Item.filterText = R;
              Item.insertText = R;
              List.items.push_back(std::move(Item));
            }
          }
        }
      }

      return List;
    }
  }

  // General scope completion
  // 1. Current file scopes
  size_t Cur = BestScope;
  while (true) {
    const auto &S = Scopes[Cur];
    for (const auto &D : S.Decls) {
      if (PartialWord.empty() ||
          llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
        if (SeenLabels.insert(D.Name).second) {
          CompletionItem Item;
          Item.label = D.Name;
          std::string PriorityPrefix;
          if (D.Kind == DeclKind::Parameter || D.Kind == DeclKind::Variable) {
            Item.kind = CompletionItemKind::Variable;
            PriorityPrefix = (Cur == BestScope) ? "0_" : "1_";
          } else if (D.Kind == DeclKind::Function ||
                     D.Kind == DeclKind::Constructor) {
            Item.kind = D.IsMember ? CompletionItemKind::Method
                                   : CompletionItemKind::Function;
            PriorityPrefix = "2_";
          } else if (D.Kind == DeclKind::Class) {
            Item.kind = CompletionItemKind::Class;
            PriorityPrefix = "2_";
          } else if (D.Kind == DeclKind::Enum ||
                     D.Kind == DeclKind::EnumValue) {
            Item.kind = (D.Kind == DeclKind::Enum)
                            ? CompletionItemKind::Enum
                            : CompletionItemKind::EnumMember;
            PriorityPrefix = "2_";
          } else {
            Item.kind = CompletionItemKind::Text;
            PriorityPrefix = "2_";
          }
          Item.detail = D.TypeName;
          Item.sortText = PriorityPrefix + D.Name;
          Item.filterText = D.Name;
          Item.insertText = D.Name;
          List.items.push_back(std::move(Item));
        }
      }
    }
    if (Cur == 0)
      break;
    Cur = S.ParentId;
  }

  // 2. Members of enclosing class (if inside a member function of EnclosingClass)
  if (!EffectiveEnclosingClass.empty()) {
    llvm::StringRef EnclosingClass = EffectiveEnclosingClass;
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.EnclosingClass == EnclosingClass) {
          if (PartialWord.empty() ||
              llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
            if (SeenLabels.insert(D.Name).second) {
              CompletionItem Item;
              Item.label = D.Name;
              Item.kind = (D.Kind == DeclKind::Function ||
                           D.Kind == DeclKind::Constructor)
                              ? CompletionItemKind::Method
                              : CompletionItemKind::Field;
              Item.detail = D.TypeName;
              Item.sortText = "1_" + D.Name;
              Item.filterText = D.Name;
              Item.insertText = D.Name;
              List.items.push_back(std::move(Item));
            }
          }
        }
      }
    }
  }

  // 3. Known namespaces and declarations from included headers
  for (llvm::StringRef NS : {"std", "llvm", "clang", "clangd"}) {
    if (PartialWord.empty() || NS.starts_with_insensitive(PartialWord)) {
      if (SeenLabels.insert(NS).second) {
        CompletionItem Item;
        Item.label = NS.str();
        Item.kind = CompletionItemKind::Module;
        Item.detail = "namespace " + NS.str();
        Item.sortText = "0_" + NS.str();
        Item.filterText = NS.str();
        Item.insertText = NS.str();
        List.items.push_back(std::move(Item));
      }
    }
  }

  auto FS = getFS();
  if (FS) {
    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          for (const auto &D : Info.Decls) {
            bool IsEnclosingMember =
                !EffectiveEnclosingClass.empty() &&
                (D.EnclosingClass == EffectiveEnclosingClass ||
                 D.EnclosingScope == EffectiveEnclosingClass);
            bool IsTopLevelOrType =
                D.EnclosingClass.empty() || D.Kind == DeclKind::Class ||
                D.Kind == DeclKind::Enum || D.Kind == DeclKind::TypeAlias;
            if (IsEnclosingMember || IsTopLevelOrType) {
              if (PartialWord.empty() ||
                  llvm::StringRef(D.Name).starts_with_insensitive(
                      PartialWord)) {
                if (SeenLabels.insert(D.Name).second) {
                  CompletionItem Item;
                  Item.label = D.Name;
                  if (D.Kind == DeclKind::Class)
                    Item.kind = CompletionItemKind::Class;
                  else if (D.Kind == DeclKind::Enum)
                    Item.kind = CompletionItemKind::Enum;
                  else if (D.Kind == DeclKind::EnumValue)
                    Item.kind = CompletionItemKind::EnumMember;
                  else if (D.Kind == DeclKind::TypeAlias)
                    Item.kind = CompletionItemKind::Interface;
                  else if (D.Kind == DeclKind::Function ||
                           D.Kind == DeclKind::Constructor)
                    Item.kind = IsEnclosingMember
                                    ? CompletionItemKind::Method
                                    : CompletionItemKind::Function;
                  else
                    Item.kind = IsEnclosingMember
                                    ? CompletionItemKind::Field
                                    : CompletionItemKind::Variable;
                  Item.detail = D.TypeName;
                  Item.sortText = IsEnclosingMember ? ("1_" + D.Name)
                                                   : ("2_" + D.Name);
                  Item.filterText = D.Name;
                  Item.insertText = D.Name;
                  List.items.push_back(std::move(Item));
                }
              }
            }
          }
          return false;
        },
        /*MaxHeaders=*/60, /*MaxDepth=*/3);
  }

  // 4. Identifiers from the document token stream (document word fallback)
  for (const auto &Tok : Parsed->ParseableStream.tokens()) {
    if (Tok.Kind == tok::raw_identifier || Tok.Kind == tok::identifier) {
      llvm::StringRef Word = getOrigToken(Tok, *Parsed).text();
      if (Word.size() >= 2) {
        if (PartialWord.empty() || Word.starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(Word).second) {
            CompletionItem Item;
            Item.label = Word.str();
            Item.kind = CompletionItemKind::Text;
            Item.sortText = "3_" + Word.str();
            Item.filterText = Word.str();
            Item.insertText = Word.str();
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
  }

  // 5. C++ Keywords
  static const char *Keywords[] = {
      "alignas", "alignof", "auto", "bool", "break", "case", "catch",
      "char", "class", "concept", "const", "constexpr", "const_cast",
      "continue", "decltype", "default", "delete", "do", "double",
      "dynamic_cast", "else", "enum", "explicit", "export", "extern",
      "false", "final", "float", "for", "friend", "goto", "if",
      "inline", "int", "long", "mutable", "namespace", "new",
      "noexcept", "nullptr", "operator", "override", "private",
      "protected", "public", "reinterpret_cast", "requires", "return",
      "short", "signed", "sizeof", "static", "static_cast", "struct",
      "switch", "template", "this", "throw", "true", "try",
      "typedef", "typeid", "typename", "union", "unsigned", "using",
      "virtual", "void", "volatile", "while"};

  for (const char *Kw : Keywords) {
    if (PartialWord.empty() ||
        llvm::StringRef(Kw).starts_with_insensitive(PartialWord)) {
      if (SeenLabels.insert(Kw).second) {
        CompletionItem Item;
        Item.label = Kw;
        Item.kind = CompletionItemKind::Keyword;
        Item.sortText = "4_" + std::string(Kw);
        Item.filterText = Kw;
        Item.insertText = Kw;
        List.items.push_back(std::move(Item));
      }
    }
  }

  llvm::sort(List.items, [](const CompletionItem &A, const CompletionItem &B) {
    return A.sortText < B.sortText;
  });
  return List;
}

} // namespace clangd
} // namespace clang
