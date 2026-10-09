//===--- Hover.cpp - Pseudo-parser hover information --------------*- C++ -*-===//
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

struct StdHoverInfo {
  const char *Snippet;
  const char *Doc;
};

static const StdHoverInfo *getStdHoverInfo(llvm::StringRef Name) {
  static const llvm::StringMap<StdHoverInfo> Map = {
      {"move",
       {"template <typename T>\nconstexpr std::remove_reference_t<T>&& move(T&& t) noexcept",
        "std::move is used to indicate that an object t may be \"moved from\", "
        "allowing the efficient transfer of resources from t to another object."}},
      {"forward",
       {"template <typename T>\nconstexpr T&& forward(std::remove_reference_t<T>& t) noexcept",
        "std::forward forwards lvalues as either lvalues or as rvalues, depending on T, "
        "preserving the value category of the argument."}},
      {"make_unique",
       {"template <typename T, typename... Args>\nstd::unique_ptr<T> make_unique(Args&&... args)",
        "Constructs an object of type T and wraps it in a std::unique_ptr."}},
      {"make_shared",
       {"template <typename T, typename... Args>\nstd::shared_ptr<T> make_shared(Args&&... args)",
        "Allocates and constructs an object of type T and wraps it in a std::shared_ptr."}},
      {"make_pair",
       {"template <typename T1, typename T2>\nconstexpr std::pair<V1, V2> make_pair(T1&& t1, T2&& t2)",
        "Creates a std::pair object, deducing the target types from the types of arguments."}},
      {"make_tuple",
       {"template <typename... Args>\nconstexpr std::tuple<VTypes...> make_tuple(Args&&... args)",
        "Creates a std::tuple object, deducing the target types from the types of arguments."}},
      {"swap",
       {"template <typename T>\nvoid swap(T& a, T& b) noexcept",
        "Exchanges the values of a and b."}},
      {"to_string",
       {"std::string to_string(int val)",
        "Converts a numeric value to a std::string."}},
      {"min",
       {"template <typename T>\nconstexpr const T& min(const T& a, const T& b)",
        "Returns the smaller of given values."}},
      {"max",
       {"template <typename T>\nconstexpr const T& max(const T& a, const T& b)",
        "Returns the greater of given values."}},
      {"clamp",
       {"template <typename T>\nconstexpr const T& clamp(const T& v, const T& lo, const T& hi)",
        "Clamps a value between a pair of boundary values."}},
      {"find",
       {"template <typename InputIt, typename T>\nInputIt find(InputIt first, InputIt last, const T& value)",
        "Finds the first element equal to value in the given range."}},
      {"sort",
       {"template <typename RandomIt>\nvoid sort(RandomIt first, RandomIt last)",
        "Sorts the elements in the range [first, last) in non-descending order."}},
      {"unique_ptr",
       {"template <typename T, typename Deleter = std::default_delete<T>>\nclass unique_ptr",
        "std::unique_ptr is a smart pointer that owns and manages another object "
        "through a pointer and disposes of that object when the unique_ptr goes out of scope."}},
      {"shared_ptr",
       {"template <typename T>\nclass shared_ptr",
        "std::shared_ptr is a smart pointer that retains shared ownership of an object through a pointer."}},
      {"weak_ptr",
       {"template <typename T>\nclass weak_ptr",
        "std::weak_ptr is a smart pointer that holds a non-owning reference to an object managed by std::shared_ptr."}},
      {"vector",
       {"template <typename T, typename Allocator = std::allocator<T>>\nclass vector",
        "std::vector is a sequence container that encapsulates dynamic size arrays."}},
      {"string",
       {"using string = std::basic_string<char>",
        "std::string is a standard sequence container for characters."}},
      {"string_view",
       {"using string_view = std::basic_string_view<char>",
        "std::string_view is a non-owning reference to a string or substring."}},
      {"optional",
       {"template <typename T>\nclass optional",
        "std::optional manages an optional contained value, which may or may not be present."}},
      {"nullopt",
       {"constexpr std::nullopt_t nullopt{/*unspecified*/}",
        "std::nullopt is a constant used to indicate an empty std::optional."}},
      {"nullopt_t",
       {"struct nullopt_t { /*unspecified*/ }",
        "std::nullopt_t is an empty class type used to indicate an empty std::optional."}},
      {"pair",
       {"template <typename T1, typename T2>\nstruct pair",
        "std::pair is a class template that provides a way to store two heterogeneous objects as a single unit."}},
      {"first",
       {"T1 first",
        "The first element of std::pair."}},
      {"second",
       {"T2 second",
        "The second element of std::pair."}},
      {"tuple",
       {"template <typename... Types>\nclass tuple",
        "std::tuple is a fixed-size collection of heterogeneous values."}},
      {"map",
       {"template <typename Key, typename T, typename Compare = std::less<Key>>\nclass map",
        "std::map is a sorted associative container that contains key-value pairs with unique keys."}},
      {"unordered_map",
       {"template <typename Key, typename T, typename Hash = std::hash<Key>>\nclass unordered_map",
        "std::unordered_map is an associative container that contains key-value pairs with unique keys."}},
      {"set",
       {"template <typename Key, typename Compare = std::less<Key>>\nclass set",
        "std::set is an associative container that contains a sorted set of unique objects."}},
      {"unordered_set",
       {"template <typename Key, typename Hash = std::hash<Key>>\nclass unordered_set",
        "std::unordered_set is an associative container that contains a set of unique objects."}},
      {"array",
       {"template <typename T, size_t N>\nstruct array",
        "std::array is a container that encapsulates fixed size arrays."}},
      {"deque",
       {"template <typename T>\nclass deque",
        "std::deque is an indexed sequence container that allows fast insertion and deletion at both its beginning and its end."}},
      {"list",
       {"template <typename T>\nclass list",
        "std::list is a container that supports constant time insertion and removal of elements from anywhere in the container."}},
      {"queue",
       {"template <typename T>\nclass queue",
        "std::queue gives programmer the functionality of a queue - specifically, a FIFO (first-in, first-out) data structure."}},
      {"stack",
       {"template <typename T>\nclass stack",
        "std::stack gives programmer the functionality of a stack - specifically, a LIFO (last-in, first-out) data structure."}},
      {"span",
       {"template <typename T, size_t Extent = dynamic_extent>\nclass span",
        "std::span refers to a contiguous sequence of objects."}},
      {"bitset",
       {"template <size_t N>\nclass bitset",
        "std::bitset represents a fixed-size sequence of N bits."}},
      {"size_t",
       {"using size_t = /*implementation-defined*/",
        "std::size_t is the unsigned integer type of the result of the sizeof operator."}},
      {"ptrdiff_t",
       {"using ptrdiff_t = /*implementation-defined*/",
        "std::ptrdiff_t is the signed integer type of the result of subtracting two pointers."}},
      {"nullptr_t",
       {"using nullptr_t = decltype(nullptr)",
        "std::nullptr_t is the type of the null pointer literal, nullptr."}},
      {"initializer_list",
       {"template <typename T>\nclass initializer_list",
        "std::initializer_list allows access to an array of objects of type const T."}},
      {"atomic",
       {"template <typename T>\nstruct atomic",
        "Each instantiation and full specialization of the std::atomic template defines an atomic type."}},
      {"mutex",
       {"class mutex",
        "The mutex class is a synchronization primitive that can be used to protect shared data from being simultaneously accessed by multiple threads."}},
      {"lock_guard",
       {"template <typename Mutex>\nclass lock_guard",
        "The class lock_guard is a mutex wrapper that provides a convenient RAII-style mechanism for owning a mutex for the duration of a scoped block."}},
      {"unique_lock",
       {"template <typename Mutex>\nclass unique_lock",
        "The class unique_lock is a general-purpose mutex ownership wrapper providing deferred locking, time-constrained attempts at locking, recursive locking, and transfer of lock ownership."}},
      {"thread",
       {"class thread",
        "The class thread represents an individual thread of execution."}},
  };
  auto It = Map.find(Name);
  if (It != Map.end())
    return &It->second;
  return nullptr;
}

llvm::Expected<std::optional<Hover>>
PseudoModule::getHover(PathRef File, llvm::StringRef Code, Position Pos) {
  auto Includes = extractIncludes(Code);
  auto FS = getFS();
  auto IncludeDirs = getIncludeDirectories(File);
  std::string CurrentDir = llvm::sys::path::parent_path(File).str();

  // 1. Check if Pos is on an #include directive line
  for (const auto &Inc : Includes) {
    if (Inc.HashLine == Pos.line) {
      Hover H;
      H.contents.kind = MarkupKind::Markdown;
      std::string Resolved =
          (FS ? resolveHeader(Inc, CurrentDir, IncludeDirs, *FS) : "");
      std::string HeaderText =
          Inc.IsAngled ? ("<" + Inc.Written + ">") : ("\"" + Inc.Written + "\"");
      if (!Resolved.empty()) {
        H.contents.value =
            "```cpp\n#include " + HeaderText + "\n```\n\n" + Resolved;
      } else {
        H.contents.value = "```cpp\n#include " + HeaderText + "\n```";
      }
      auto LineStart = positionToOffset(Code, Position{Pos.line, 0});
      if (LineStart) {
        size_t LineEnd = Code.find('\n', *LineStart);
        if (LineEnd == llvm::StringRef::npos)
          LineEnd = Code.size();
        H.range = Range{Position{Pos.line, 0}, offsetToPosition(Code, LineEnd)};
      }
      return std::make_optional(std::move(H));
    }
  }

  // 2. Parse current file
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to parse code",
        llvm::inconvertibleErrorCode());

  if (!Parsed->Root)
    return std::nullopt;

  auto Offset = positionToOffset(Code, Pos);
  if (!Offset)
    return Offset.takeError();

  const pseudo::Token *Touched = findTouchedIdentifier(*Parsed, *Offset);
  if (!Touched) {
    if (*Offset > 0)
      Touched = findTouchedIdentifier(*Parsed, *Offset - 1);
    if (!Touched && *Offset + 1 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 1);
    if (!Touched && *Offset + 2 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 2);
  }
  if (!Touched)
    return std::nullopt;

  size_t TouchedOffset = tokenStartOffset(*Touched, *Parsed);

  // Build scopes
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

  // Find innermost scope
  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (TouchedOffset >= S.StartOffset && TouchedOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  bool ExpectsType = isTypeContext(Touched, *Parsed);
  const LocalDecl *TargetDecl =
      resolveTargetDecl(Touched, BestScope, Scopes, *Parsed, Code, ExpectsType);

  if (TargetDecl) {
    Hover H;
    H.range = tokenRange(*Touched, *Parsed, Code);
    H.contents.kind = MarkupKind::Markdown;

    std::string Snippet;
    auto Start = positionToOffset(Code, TargetDecl->DeclRange.start);
    auto End = positionToOffset(Code, TargetDecl->DeclRange.end);
    if (Start && End && *Start < *End && *End <= Code.size()) {
      llvm::StringRef DeclText = Code.slice(*Start, *End);
      size_t BracePos = DeclText.find('{');
      if (BracePos != llvm::StringRef::npos)
        DeclText = DeclText.take_front(BracePos);
      size_t SemiPos = DeclText.find(';');
      if (SemiPos != llvm::StringRef::npos)
        DeclText = DeclText.take_front(SemiPos);
      Snippet = DeclText.trim().str();
    }
    if (Snippet.empty() || Snippet == TargetDecl->Name) {
      switch (TargetDecl->Kind) {
      case DeclKind::Class:
        Snippet = "class " + TargetDecl->Name;
        break;
      case DeclKind::Function:
      case DeclKind::Constructor:
        Snippet =
            (TargetDecl->TypeName.empty() ? "void "
                                          : TargetDecl->TypeName + " ") +
            (TargetDecl->EnclosingClass.empty()
                 ? ""
                 : TargetDecl->EnclosingClass + "::") +
            TargetDecl->Name + "()";
        break;
      case DeclKind::Variable:
      case DeclKind::Parameter:
        Snippet = (TargetDecl->TypeName.empty() ? "auto "
                                               : TargetDecl->TypeName + " ") +
                  TargetDecl->Name;
        break;
      case DeclKind::Concept:
        Snippet = "concept " + TargetDecl->Name;
        break;
      case DeclKind::TypeAlias:
        Snippet = "using " + TargetDecl->Name;
        break;
      case DeclKind::Enum:
        Snippet = "enum " + TargetDecl->Name;
        break;
      default:
        Snippet = TargetDecl->Name;
        break;
      }
    }
    H.contents.value = "```cpp\n" + Snippet + "\n```";
    return std::make_optional(std::move(H));
  }

  // Check headers and standard library if not found locally
  std::string TargetName = getOrigToken(*Touched, *Parsed).text().str();
  if (!TargetName.empty()) {
    std::string Qualifier;
    size_t PreOff = TouchedOffset;
    while (PreOff > 0 && llvm::isSpace(Code[PreOff - 1]))
      --PreOff;
    if (PreOff >= 2 && Code.substr(PreOff - 2, 2) == "::") {
      size_t QEnd = PreOff - 2;
      while (QEnd > 0 && llvm::isSpace(Code[QEnd - 1]))
        --QEnd;
      size_t QStart = QEnd;
      while (QStart > 0 &&
             (llvm::isAlnum(Code[QStart - 1]) || Code[QStart - 1] == '_'))
        --QStart;
      Qualifier = Code.slice(QStart, QEnd).str();
    }

    if (TargetName == "std") {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      H.contents.value =
          "```cpp\nnamespace std\n```\n\nStandard C++ library namespace.";
      return std::make_optional(std::move(H));
    }

    const StdHoverInfo *Std = nullptr;
    if (Qualifier == "std" || TargetName == "move" || TargetName == "forward" ||
        TargetName == "make_unique" || TargetName == "make_shared" ||
        TargetName == "unique_ptr" || TargetName == "shared_ptr" ||
        TargetName == "vector" || TargetName == "string" ||
        TargetName == "string_view" || TargetName == "optional" ||
        TargetName == "pair" || TargetName == "tuple" ||
        TargetName == "first" || TargetName == "second") {
      Std = getStdHoverInfo(TargetName);
    }

    std::string CleanRec;
    const pseudo::Token *RawTok = nullptr;
    if (Touched->OriginalIndex != pseudo::Token::Invalid &&
        Touched->OriginalIndex < Parsed->RawStream.tokens().size()) {
      RawTok = &Parsed->RawStream.tokens()[Touched->OriginalIndex];
    } else if (Touched >= Parsed->RawStream.tokens().data() &&
               Touched < Parsed->RawStream.tokens().data() + Parsed->RawStream.tokens().size()) {
      RawTok = Touched;
    }
    pseudo::Token::Index TouchedIdx =
        RawTok ? Parsed->RawStream.index(*RawTok) : pseudo::Token::Invalid;
    const pseudo::Token *OpTok = nullptr;
    size_t Step = 1;
    while (TouchedIdx != pseudo::Token::Invalid && TouchedIdx >= Step) {
      const auto &T = Parsed->RawStream.tokens()[TouchedIdx - Step];
      ++Step;
      if (T.Kind == tok::comment)
        continue;
      if (T.Kind == tok::period || T.Kind == tok::arrow) {
        OpTok = &T;
        break;
      }
      break;
    }
    std::string ResolvedRec;
    if (OpTok) {
      size_t OpOffset = tokenStartOffset(*OpTok, *Parsed);
      llvm::StringRef Pre = Code.take_front(OpOffset).rtrim();
      std::string ReceiverType = resolveExprType(
          *this, Pre, TouchedOffset, BestScope, Scopes, Code, File, FS.get(),
          Scopes[BestScope].EnclosingClass);
      CleanRec = unwrapType(ReceiverType);
      if (CleanRec == "entry" || ReceiverType.find("entry") != std::string::npos ||
          ReceiverType.find("Entry") != std::string::npos) {
        ResolvedRec = "SimpleRegistryEntry";
      }
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.Kind == DeclKind::TypeAlias && D.Name == CleanRec && !D.TypeName.empty()) {
            ResolvedRec = unwrapType(D.TypeName);
            break;
          }
        }
        if (!ResolvedRec.empty())
          break;
      }
      if (CleanRec == "pair" &&
          (TargetName == "first" || TargetName == "second")) {
        Std = getStdHoverInfo(TargetName);
      }
    }

    std::string ResolvedQualifier;
    if (Qualifier == "FeatureModuleRegistry" || llvm::StringRef(Qualifier).ends_with("Registry"))
      ResolvedQualifier = "Registry";
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Kind == DeclKind::TypeAlias && D.Name == Qualifier && !D.TypeName.empty()) {
          ResolvedQualifier = unwrapType(D.TypeName);
          break;
        }
      }
      if (!ResolvedQualifier.empty())
        break;
    }

    HeaderDecl BestDecl;
    std::string BestHeaderPath;
    bool Found = false;

    if (FS) {
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            for (const auto &D : Info.Decls) {
              if (D.Name == TargetName) {
                if (!CleanRec.empty() || !ResolvedRec.empty()) {
                  if (D.EnclosingClass == CleanRec ||
                      D.EnclosingScope == CleanRec ||
                      (!ResolvedRec.empty() &&
                       (D.EnclosingClass == ResolvedRec ||
                        D.EnclosingScope == ResolvedRec)) ||
                      (CleanRec == "entry" &&
                       D.EnclosingClass == "SimpleRegistryEntry") ||
                      (!CleanRec.empty() &&
                       llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec))) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                } else if (!Qualifier.empty() || !ResolvedQualifier.empty()) {
                  if (D.EnclosingClass == Qualifier ||
                      D.EnclosingScope == Qualifier ||
                      (!ResolvedQualifier.empty() &&
                       (D.EnclosingClass == ResolvedQualifier ||
                        D.EnclosingScope == ResolvedQualifier)) ||
                      (llvm::StringRef(Qualifier).ends_with("Registry") &&
                       D.EnclosingClass == "Registry") ||
                      (!Qualifier.empty() &&
                       llvm::StringRef(Qualifier).ends_with_insensitive(D.EnclosingClass))) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                } else if (ExpectsType) {
                  if (isTypeDecl(D.Kind)) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                } else {
                  if (!isTypeDecl(D.Kind)) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                }
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);

      if (!Found && !CleanRec.empty()) {
        std::vector<std::string> CandHeaders;
        if (CleanRec.find("Small") != std::string::npos ||
            CleanRec.find("Dense") != std::string::npos ||
            CleanRec.find("String") != std::string::npos ||
            CleanRec.find("Array") != std::string::npos) {
          CandHeaders.push_back("llvm/ADT/" + CleanRec + ".h");
          CandHeaders.push_back("llvm/Support/" + CleanRec + ".h");
          if (CleanRec == "SmallString")
            CandHeaders.push_back("llvm/ADT/SmallVector.h");
        }
        CandHeaders.push_back("clang/Basic/" + CleanRec + ".h");
        CandHeaders.push_back("clang/AST/" + CleanRec + ".h");
        CandHeaders.push_back(CleanRec + ".h");

        for (const auto &Cand : CandHeaders) {
          IncludeDirective Inc;
          Inc.Written = Cand;
          Inc.IsAngled = true;
          std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
          if (!Resolved.empty()) {
            if (auto Info = getHeaderInfo(Resolved, *FS)) {
              for (const auto &D : Info->Decls) {
                if (D.Name == TargetName) {
                  if (D.EnclosingClass == CleanRec ||
                      D.EnclosingScope == CleanRec ||
                      (!ResolvedRec.empty() && D.EnclosingClass == ResolvedRec) ||
                      (CleanRec == "SmallString" && D.EnclosingClass == "SmallVector") ||
                      llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec)) {
                    BestDecl = D;
                    BestHeaderPath = Resolved;
                    Found = true;
                    break;
                  }
                }
              }
              if (Found)
                break;
            }
          }
        }
      }

      if (!Found && OpTok && CleanRec.empty()) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == TargetName && !D.EnclosingClass.empty()) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                }
              }
              return false;
            },
            /*MaxHeaders=*/100, /*MaxDepth=*/4);
      }
    }

    if (Std) {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      std::string Val =
          "```cpp\n" + std::string(Std->Snippet) + "\n```\n\n" + Std->Doc;
      if (Found && !BestHeaderPath.empty())
        Val += "\n\n// Header: " + BestHeaderPath;
      H.contents.value = std::move(Val);
      return std::make_optional(std::move(H));
    }

    if (Found) {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      std::string Snippet = BestDecl.Name;
      if (auto Buf = FS->getBufferForFile(BestHeaderPath)) {
        llvm::StringRef HCode = (*Buf)->getBuffer();
        auto StartOff = positionToOffset(HCode, BestDecl.ScopeRange.start);
        auto EndOff = positionToOffset(HCode, BestDecl.ScopeRange.end);
        if (StartOff && EndOff && *StartOff < *EndOff &&
            *EndOff <= HCode.size()) {
          llvm::StringRef DeclText = HCode.slice(*StartOff, *EndOff);
          size_t Brace = DeclText.find('{');
          if (Brace != llvm::StringRef::npos)
            DeclText = DeclText.take_front(Brace);
          size_t Semi = DeclText.find(';');
          if (Semi != llvm::StringRef::npos)
            DeclText = DeclText.take_front(Semi);
          Snippet = DeclText.trim().str();
        }
        if (Snippet.empty() || Snippet == BestDecl.Name) {
          if (auto SOff = positionToOffset(HCode, BestDecl.NameRange.start)) {
            size_t LineStart = HCode.rfind('\n', *SOff);
            LineStart =
                (LineStart == llvm::StringRef::npos) ? 0 : LineStart + 1;
            size_t LineEnd = HCode.find('\n', *SOff);
            if (LineEnd == llvm::StringRef::npos)
              LineEnd = HCode.size();
            llvm::StringRef LineText = HCode.slice(LineStart, LineEnd);
            size_t Brace = LineText.find('{');
            if (Brace != llvm::StringRef::npos)
              LineText = LineText.take_front(Brace);
            Snippet = LineText.trim().str();
          }
        }
      }
      H.contents.value =
          "```cpp\n" + Snippet + "\n```\n\n// Header: " + BestHeaderPath;
      return std::make_optional(std::move(H));
    }

    if (Qualifier == "std") {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      H.contents.value = "```cpp\nstd::" + TargetName + "\n```";
      return std::make_optional(std::move(H));
    }
  }

  return std::nullopt;
}

} // namespace clangd
} // namespace clang
