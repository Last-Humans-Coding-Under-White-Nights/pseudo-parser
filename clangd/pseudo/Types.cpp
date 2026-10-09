//===--- Types.cpp - Pseudo-parser type deduction and resolution ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pseudo/Types.h"
#include "pseudo/Headers.h"
#include "PseudoModule.h"
#include "SourceCode.h"

namespace clang {
namespace clangd {

std::string unwrapType(llvm::StringRef TypeName) {
  llvm::StringRef T = TypeName.trim();
  bool Stripped = true;
  while (Stripped) {
    Stripped = false;
    if (T.starts_with("static ")) {
      T = T.drop_front(7).trim();
      Stripped = true;
    }
    if (T.starts_with("inline ")) {
      T = T.drop_front(7).trim();
      Stripped = true;
    }
    if (T.starts_with("constexpr ")) {
      T = T.drop_front(10).trim();
      Stripped = true;
    }
    if (T.starts_with("consteval ")) {
      T = T.drop_front(10).trim();
      Stripped = true;
    }
    if (T.starts_with("virtual ")) {
      T = T.drop_front(8).trim();
      Stripped = true;
    }
    if (T.starts_with("explicit ")) {
      T = T.drop_front(9).trim();
      Stripped = true;
    }
    if (T.starts_with("friend ")) {
      T = T.drop_front(7).trim();
      Stripped = true;
    }
    if (T.starts_with("const ")) {
      T = T.drop_front(6).trim();
      Stripped = true;
    }
    if (T.starts_with("volatile ")) {
      T = T.drop_front(9).trim();
      Stripped = true;
    }
  }
  while (T.ends_with("*") || T.ends_with("&"))
    T = T.drop_back(1).trim();

  size_t AngleStart = T.find('<');
  size_t AngleEnd = T.rfind('>');
  if (AngleStart != llvm::StringRef::npos && AngleEnd != llvm::StringRef::npos &&
      AngleEnd > AngleStart) {
    llvm::StringRef Outer = T.take_front(AngleStart).trim();
    if (Outer.ends_with("unique_ptr") || Outer.ends_with("shared_ptr") ||
        Outer.ends_with("optional") || Outer.ends_with("IntrusiveRefCntPtr") ||
        Outer.ends_with("auto_ptr") || Outer.ends_with("make_unique") ||
        Outer.ends_with("make_shared")) {
      llvm::StringRef Inner = T.slice(AngleStart + 1, AngleEnd).trim();
      size_t Comma = Inner.find(',');
      if (Comma != llvm::StringRef::npos)
        Inner = Inner.take_front(Comma).trim();
      return unwrapType(Inner);
    }
    T = Outer;
  }

  size_t LastColons = T.rfind("::");
  if (LastColons != llvm::StringRef::npos)
    T = T.drop_front(LastColons + 2);

  return T.str();
}

bool isAutoTypeName(llvm::StringRef T) {
  T = T.trim();
  // Strip leading const/volatile
  if (T.starts_with("const "))
    T = T.drop_front(6).trim();
  if (T.starts_with("volatile "))
    T = T.drop_front(9).trim();
  // Strip trailing & and *
  while (T.ends_with("&") || T.ends_with("*") || T.ends_with(" "))
    T = T.drop_back(1);
  return T == "auto";
}

// Given a resolved container type (e.g. "FeatureModuleSet", "std::vector<FeatureModule>",
// "FeatureModules"), deduce the element type for a range-for loop variable.
std::string deduceElementType(llvm::StringRef ContainerType) {
  llvm::StringRef T = ContainerType.trim();
  // Strip leading const/volatile/& qualifiers
  while (T.starts_with("const ") || T.starts_with("volatile "))
    T = T.split(' ').second.trim();
  while (T.ends_with("&") || T.ends_with("*") || T.ends_with(" "))
    T = T.drop_back(1);

  // If it's a template like vector<T>, set<T>, etc., extract inner type
  size_t AngleStart = T.find('<');
  size_t AngleEnd = T.rfind('>');
  if (AngleStart != llvm::StringRef::npos && AngleEnd != llvm::StringRef::npos &&
      AngleEnd > AngleStart) {
    llvm::StringRef Inner = T.slice(AngleStart + 1, AngleEnd).trim();
    // Handle multi-param templates: take first param
    size_t Comma = Inner.find(',');
    if (Comma != llvm::StringRef::npos)
      Inner = Inner.take_front(Comma).trim();
    // Strip const/& from inner type
    while (Inner.starts_with("const "))
      Inner = Inner.drop_front(6).trim();
    while (Inner.ends_with("&") || Inner.ends_with("*") || Inner.ends_with(" "))
      Inner = Inner.drop_back(1);
    // Strip namespace qualifiers
    size_t LastColons = Inner.rfind("::");
    if (LastColons != llvm::StringRef::npos)
      Inner = Inner.drop_front(LastColons + 2);
    return Inner.str();
  }

  // Strip namespace qualifiers to get base name
  size_t LastColons = T.rfind("::");
  llvm::StringRef BaseName = (LastColons != llvm::StringRef::npos)
                                  ? T.drop_front(LastColons + 2)
                                  : T;

  // Strip common collection suffixes to get element type name
  if (BaseName.ends_with("Set"))
    return BaseName.drop_back(3).str();
  if (BaseName.ends_with("Vector"))
    return BaseName.drop_back(6).str();
  if (BaseName.ends_with("List"))
    return BaseName.drop_back(4).str();
  if (BaseName.ends_with("Map"))
    return BaseName.drop_back(3).str();
  if (BaseName.ends_with("Array"))
    return BaseName.drop_back(5).str();

  return BaseName.str();
}

std::string resolveExprType(
    PseudoModule &Self, llvm::StringRef Pre, size_t CursorOffset,
    size_t BestScope, const std::vector<LexicalScope> &Scopes,
    llvm::StringRef Code, PathRef File, llvm::vfs::FileSystem *FS,
    llvm::StringRef EffectiveEnclosingClass) {
  Pre = Pre.rtrim();
  while (Pre.starts_with("*") || Pre.starts_with("&") || Pre.starts_with(" "))
    Pre = Pre.drop_front(1).trim();
  while (Pre.starts_with("(") && Pre.ends_with(")"))
    Pre = Pre.slice(1, Pre.size() - 1).trim();
  if (Pre.empty())
    return "";

  // Case 1: Pre ends with ')' (function or method call)
  if (Pre.ends_with(")")) {
    int Depth = 0;
    ssize_t LParen = -1;
    for (ssize_t I = (ssize_t)Pre.size() - 1; I >= 0; --I) {
      if (Pre[I] == ')')
        ++Depth;
      else if (Pre[I] == '(') {
        --Depth;
        if (Depth == 0) {
          LParen = I;
          break;
        }
      }
    }
    if (LParen > 0) {
      ssize_t FnEnd = LParen;
      while (FnEnd > 0 && llvm::isSpace(Pre[FnEnd - 1]))
        --FnEnd;
      ssize_t FnStart = FnEnd;
      while (FnStart > 0 &&
             (llvm::isAlnum(Pre[FnStart - 1]) || Pre[FnStart - 1] == '_'))
        --FnStart;
      llvm::StringRef FnName = Pre.slice(FnStart, FnEnd);

      ssize_t DotPos = FnStart;
      while (DotPos > 0 && llvm::isSpace(Pre[DotPos - 1]))
        --DotPos;
      std::string ReceiverType;
      if (DotPos > 0 && Pre[DotPos - 1] == '.') {
        ReceiverType = resolveExprType(
            Self, Pre.take_front(DotPos - 1).rtrim(), CursorOffset, BestScope,
            Scopes, Code, File, FS, EffectiveEnclosingClass);
      } else if (DotPos >= 2 && Pre.slice(DotPos - 2, DotPos) == "->") {
        ReceiverType = resolveExprType(
            Self, Pre.take_front(DotPos - 2).rtrim(), CursorOffset, BestScope,
            Scopes, Code, File, FS, EffectiveEnclosingClass);
      } else if (DotPos >= 2 && Pre.slice(DotPos - 2, DotPos) == "::") {
        ssize_t ScopeEnd = DotPos - 2;
        while (ScopeEnd > 0 && llvm::isSpace(Pre[ScopeEnd - 1]))
          --ScopeEnd;
        ssize_t ScopeStart = ScopeEnd;
        while (ScopeStart > 0 &&
               (llvm::isAlnum(Pre[ScopeStart - 1]) || Pre[ScopeStart - 1] == '_'))
          --ScopeStart;
        ReceiverType = Pre.slice(ScopeStart, ScopeEnd).str();
      }

      // Deduce return type for common methods
      if (FnName == "current" && (ReceiverType == "Context" || ReceiverType.empty()))
        return "Context";
      if (FnName == "clone" && (ReceiverType == "Context" || ReceiverType.empty()))
        return "Context";
      if (FnName == "getName" || FnName == "getDesc")
        return "llvm::StringRef";
      if (FnName == "instantiate") {
        if (ReceiverType.find("FeatureModule") != std::string::npos ||
            ReceiverType.find("entry") != std::string::npos ||
            ReceiverType.find("Registry") != std::string::npos ||
            ReceiverType.empty())
          return "std::unique_ptr<FeatureModule>";
        return "std::unique_ptr<void>";
      }
      if (FnName == "str" || FnName == "string")
        return "std::string";
      if (FnName == "data" || FnName == "c_str")
        return "const char *";
      if (FnName == "size" || FnName == "length" || FnName == "count")
        return "size_t";
      if (FnName == "empty" || FnName == "has_value" || FnName == "blockUntilIdle")
        return "bool";
      if (FnName == "typeId")
        return "void *";
      if (FnName == "facilities")
        return "Facilities";
      if (FnName == "fromRegistry")
        return "FeatureModuleSet";
      if (FnName == "getLangOpts")
        return "LangOptions";
      if (FnName == "getPrintingPolicy")
        return "PrintingPolicy";
      if (FnName == "try_emplace" || FnName == "insert")
        return "std::pair";
      if (FnName == "get" || FnName == "value" || FnName == "front" ||
          FnName == "back")
        return unwrapType(ReceiverType);

      std::string CleanRec = unwrapType(ReceiverType);
      if (!CleanRec.empty()) {
        for (const auto &S : Scopes) {
          if (S.Name == CleanRec || S.EnclosingClass == CleanRec) {
            for (const auto &D : S.Decls) {
              if (D.Name == FnName && !D.TypeName.empty())
                return D.TypeName;
            }
          }
        }
        if (FS) {
          std::string FoundType;
          traverseIncludedHeaders(
              Self, File, Code, *FS,
              [&](const HeaderInfo &Info,
                  llvm::StringRef HeaderPath) {
                for (const auto &D : Info.Decls) {
                  if (D.Name == FnName &&
                      (D.EnclosingClass == CleanRec ||
                       D.EnclosingScope == CleanRec) &&
                      !D.TypeName.empty()) {
                    FoundType = D.TypeName;
                    return true;
                  }
                }
                return false;
              });
          if (!FoundType.empty())
            return FoundType;
        }
      }

      // Free function lookup
      const LocalDecl *FnDecl =
          lookupDecl(BestScope, FnName, CursorOffset, Scopes, Code);
      if (FnDecl && !FnDecl->TypeName.empty())
        return FnDecl->TypeName;
    }
  }

  // Case 2: Pre ends with identifier
  size_t BaseEnd = Pre.size();
  size_t BaseStart = BaseEnd;
  while (BaseStart > 0 &&
         (llvm::isAlnum(Pre[BaseStart - 1]) || Pre[BaseStart - 1] == '_'))
    --BaseStart;
  llvm::StringRef BaseName = Pre.slice(BaseStart, BaseEnd);

  if (BaseName == "this")
    return std::string(EffectiveEnclosingClass);

  if (!BaseName.empty()) {
    // Check if BaseName is preceded by '.' or '->'
    ssize_t DotPos = BaseStart;
    while (DotPos > 0 && llvm::isSpace(Pre[DotPos - 1]))
      --DotPos;
    std::string ReceiverType;
    if (DotPos > 0 && Pre[DotPos - 1] == '.') {
      ReceiverType = resolveExprType(
          Self, Pre.take_front(DotPos - 1).rtrim(), CursorOffset, BestScope,
          Scopes, Code, File, FS, EffectiveEnclosingClass);
    } else if (DotPos >= 2 && Pre.slice(DotPos - 2, DotPos) == "->") {
      ReceiverType = resolveExprType(
          Self, Pre.take_front(DotPos - 2).rtrim(), CursorOffset, BestScope,
          Scopes, Code, File, FS, EffectiveEnclosingClass);
    }

    if (!ReceiverType.empty()) {
      std::string CleanRec = unwrapType(ReceiverType);
      if (CleanRec == "pair") {
        if (BaseName == "first")
          return "first_type";
        if (BaseName == "second")
          return "bool";
      }
      for (const auto &S : Scopes) {
        if (S.Name == CleanRec || S.EnclosingClass == CleanRec) {
          for (const auto &D : S.Decls) {
            if (D.Name == BaseName && !D.TypeName.empty())
              return D.TypeName;
          }
        }
      }
      if (FS) {
        std::string FoundType;
        traverseIncludedHeaders(
            Self, File, Code, *FS,
            [&](const HeaderInfo &Info,
                llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == BaseName &&
                    (D.EnclosingClass == CleanRec ||
                     D.EnclosingScope == CleanRec) &&
                    !D.TypeName.empty()) {
                  FoundType = D.TypeName;
                  return true;
                }
              }
              return false;
            });
        if (!FoundType.empty())
          return FoundType;
      }
    }

    const LocalDecl *BaseDecl =
        lookupDecl(BestScope, BaseName, CursorOffset, Scopes, Code);
    if (BaseDecl && !BaseDecl->TypeName.empty()) {
      // If the declared type is auto (range-for or deduced), try to resolve via InitExpr
      if (isAutoTypeName(BaseDecl->TypeName) && BaseDecl->IsRangeFor &&
          !BaseDecl->InitExpr.empty()) {
        // Strip dereference / address-of from container expression
        llvm::StringRef ContainerExpr = llvm::StringRef(BaseDecl->InitExpr).trim();
        while (ContainerExpr.starts_with("*") || ContainerExpr.starts_with("&") ||
               ContainerExpr.starts_with(" "))
          ContainerExpr = ContainerExpr.drop_front(1).trim();
        while (ContainerExpr.starts_with("(") && ContainerExpr.ends_with(")"))
          ContainerExpr = ContainerExpr.slice(1, ContainerExpr.size() - 1).trim();

        // Resolve the container's type
        std::string ContainerType = resolveExprType(
            Self, ContainerExpr, CursorOffset, BestScope, Scopes, Code, File,
            FS, EffectiveEnclosingClass);

        if (!ContainerType.empty()) {
          std::string ElemType = deduceElementType(ContainerType);
          if (!ElemType.empty() && ElemType != "auto")
            return ElemType;
        }

        // Fallback: try deducing directly from the container expression name
        // e.g. "*Opts.FeatureModules" -> look at FeatureModules member type
        // The resolveExprType call above handles this via header traversal.
        // If still unresolved, try deducing from the container expression identifier
        llvm::StringRef LastPart = ContainerExpr;
        size_t DotPos = ContainerExpr.rfind('.');
        size_t ArrowPos = ContainerExpr.rfind("->");
        size_t SepPos = (ArrowPos != llvm::StringRef::npos &&
                         (DotPos == llvm::StringRef::npos || ArrowPos > DotPos))
                            ? ArrowPos + 2
                            : (DotPos != llvm::StringRef::npos ? DotPos + 1
                                                               : llvm::StringRef::npos);
        if (SepPos != llvm::StringRef::npos)
          LastPart = ContainerExpr.drop_front(SepPos).trim();

        // Check for known suffix patterns directly on the identifier
        std::string Deduced = deduceElementType(LastPart);
        if (!Deduced.empty() && Deduced != LastPart.str())
          return Deduced;
      }
      return BaseDecl->TypeName;
    }

    if (!EffectiveEnclosingClass.empty()) {
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.Name == BaseName &&
              (D.EnclosingClass == EffectiveEnclosingClass ||
               S.Name == EffectiveEnclosingClass) &&
              !D.TypeName.empty())
            return D.TypeName;
        }
      }
      if (FS) {
        std::string FoundType;
        traverseIncludedHeaders(
            Self, File, Code, *FS,
            [&](const HeaderInfo &Info,
                llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == BaseName &&
                    (D.EnclosingClass == EffectiveEnclosingClass ||
                     D.EnclosingScope == EffectiveEnclosingClass) &&
                    !D.TypeName.empty()) {
                  FoundType = D.TypeName;
                  return true;
                }
              }
              return false;
            });
        if (!FoundType.empty())
          return FoundType;
      }
    }
  }

  return "";
}

const LocalDecl *resolveTargetDecl(
    const pseudo::Token *Touched, size_t BestScope,
    const std::vector<LexicalScope> &Scopes, const ParseOutput &Parsed,
    llvm::StringRef Code, bool ExpectsType) {
  std::string TargetName = getOrigToken(*Touched, Parsed).text().str();
  size_t TouchedOffset = tokenStartOffset(*Touched, Parsed);

  if (isCtorMemberInitializerName(Touched, Parsed)) {
    std::string TargetClass = Scopes[BestScope].EnclosingClass;
    if (TargetClass.empty() && Scopes[BestScope].ParentId < Scopes.size()) {
      const auto &ParentScope = Scopes[Scopes[BestScope].ParentId];
      if (ParentScope.Kind == ScopeKind::Class)
        TargetClass = ParentScope.Name;
    }
    for (const auto &S : Scopes) {
      if (S.Kind == ScopeKind::Class &&
          (TargetClass.empty() || S.Name == TargetClass ||
           S.EnclosingClass == TargetClass)) {
        for (const auto &D : S.Decls) {
          if (D.Name == TargetName && D.IsMember && !D.IsParameter &&
              D.Kind != DeclKind::Parameter) {
            return &D;
          }
        }
      }
    }
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Name == TargetName && D.IsMember && !D.IsParameter &&
            D.Kind != DeclKind::Parameter &&
            (TargetClass.empty() || D.EnclosingClass == TargetClass)) {
          return &D;
        }
      }
    }
    return nullptr;
  }

  // If touched token is on the LHS of an init-capture definition, return that capture.
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (D.IsInitCapture && D.DeclOffset == TouchedOffset && D.Name == TargetName)
        return &D;
    }
  }

  const LocalDecl *TargetDecl = nullptr;
  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Parsed.RawStream.tokens().size()) {
    RawTok = &Parsed.RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Parsed.RawStream.tokens().data() &&
             Touched < Parsed.RawStream.tokens().data() + Parsed.RawStream.tokens().size()) {
    RawTok = Touched;
  }
  pseudo::Token::Index TouchedIdx =
      RawTok ? Parsed.RawStream.index(*RawTok) : pseudo::Token::Invalid;
  const pseudo::Token *OpTok = nullptr;
  const pseudo::Token *LhsTok = nullptr;
  size_t Step = 1;
  while (TouchedIdx != pseudo::Token::Invalid && TouchedIdx >= Step) {
    const auto &T = Parsed.RawStream.tokens()[TouchedIdx - Step];
    ++Step;
    if (T.Kind == tok::comment)
      continue;
    if (!OpTok) {
      if (T.Kind == tok::period || T.Kind == tok::arrow ||
          T.Kind == tok::coloncolon)
        OpTok = &T;
      else
        break;
    } else {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
        LhsTok = &T;
      break;
    }
  }
  std::string CleanRec;
  if (OpTok) {
    if (LhsTok) {
      std::string LhsName = getOrigToken(*LhsTok, Parsed).text().str();
      if (LhsName == "this") {
        std::string TargetClass = Scopes[BestScope].EnclosingClass;
        CleanRec = TargetClass;
        if (!TargetClass.empty()) {
          for (const auto &CS : Scopes) {
            if (CS.Kind == ScopeKind::Class && CS.Name == TargetClass) {
              for (const auto &D : CS.Decls) {
                if (D.Name == TargetName) {
                  TargetDecl = &D;
                  break;
                }
              }
            }
            if (TargetDecl)
              break;
          }
        }
      } else if (OpTok->Kind == tok::coloncolon) {
        for (const auto &CS : Scopes) {
          if ((CS.Kind == ScopeKind::Class || CS.Kind == ScopeKind::Namespace) &&
              CS.Name == LhsName) {
            for (const auto &D : CS.Decls) {
              if (D.Name == TargetName) {
                // When resolving Namespace::Name, skip class member decls.
                // E.g. clangd::signatureHelp should not match
                // ClangdServer::signatureHelp stored in the clangd scope.
                if (CS.Kind == ScopeKind::Namespace &&
                    (D.IsMember || !D.EnclosingClass.empty()))
                  continue;
                if (!ExpectsType || isTypeDecl(D.Kind)) {
                  TargetDecl = &D;
                  break;
                }
              }
            }
          }
          if (TargetDecl)
            break;
        }
        if (!TargetDecl) {
          for (const auto &S : Scopes) {
            for (const auto &D : S.Decls) {
              if (D.Name == TargetName && D.EnclosingClass == LhsName) {
                if (D.Kind == DeclKind::Parameter || D.IsParameter)
                  continue;
                if (ExpectsType && !isTypeDecl(D.Kind))
                  continue;
                TargetDecl = &D;
                break;
              }
            }
            if (TargetDecl)
              break;
          }
        }
      } else {
        size_t LhsOffset = tokenStartOffset(*LhsTok, Parsed);
        const LocalDecl *LhsDecl =
            lookupDecl(BestScope, LhsName, LhsOffset, Scopes, Code);
        if (LhsDecl && !LhsDecl->TypeName.empty()) {
          CleanRec = unwrapType(LhsDecl->TypeName);
          for (const auto &CS : Scopes) {
            if (CS.Kind == ScopeKind::Class &&
                (CS.Name == CleanRec || CS.EnclosingClass == CleanRec)) {
              for (const auto &D : CS.Decls) {
                if (D.Name == TargetName) {
                  TargetDecl = &D;
                  break;
                }
              }
            }
            if (TargetDecl)
              break;
          }
        }
      }
    }
  }

  if (!TargetDecl && !OpTok) {
    TargetDecl = lookupDecl(BestScope, TargetName, TouchedOffset, Scopes, Code,
                            ExpectsType);
    if (!TargetDecl)
      TargetDecl = findAnyDecl(TargetName, Scopes, ExpectsType);
  }

  if (!TargetDecl && OpTok &&
      (OpTok->Kind == tok::period || OpTok->Kind == tok::arrow)) {
    if (!CleanRec.empty()) {
      const LocalDecl *Candidate = findAnyDecl(TargetName, Scopes, ExpectsType);
      if (Candidate && Candidate->IsMember && Candidate->EnclosingClass == CleanRec)
        TargetDecl = Candidate;
    }
  }

  return TargetDecl;
}

} // namespace clangd
} // namespace clang
