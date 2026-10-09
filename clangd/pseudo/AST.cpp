//===--- AST.cpp - Pseudo-parser AST node construction ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pseudo/AST.h"
#include "pseudo/Parse.h"
#include "pseudo/Scopes.h"
#include "SourceCode.h"

namespace clang {
namespace clangd {

ASTNode buildTypeNode(llvm::StringRef TypeStr, Range R) {
  TypeStr = TypeStr.trim();
  if (TypeStr.empty()) {
    ASTNode T;
    T.role = "type";
    T.kind = "Builtin";
    T.detail = "void";
    T.arcana = "QualType 'void'";
    T.range = R;
    return T;
  }

  if (TypeStr.starts_with("struct "))
    TypeStr = TypeStr.drop_front(7).trim();
  else if (TypeStr.starts_with("class "))
    TypeStr = TypeStr.drop_front(6).trim();
  else if (TypeStr.starts_with("enum "))
    TypeStr = TypeStr.drop_front(5).trim();
  else if (TypeStr.starts_with("typename "))
    TypeStr = TypeStr.drop_front(9).trim();

  while (TypeStr.ends_with("::"))
    TypeStr = TypeStr.drop_back(2).trim();

  if (TypeStr.empty()) {
    ASTNode T;
    T.role = "type";
    T.kind = "Builtin";
    T.detail = "void";
    T.arcana = "QualType 'void'";
    T.range = R;
    return T;
  }

  if (TypeStr.ends_with("&&")) {
    ASTNode T;
    T.role = "type";
    T.kind = "RValueReference";
    T.arcana = "RValueReferenceType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(2), R));
    return T;
  }
  if (TypeStr.ends_with("&")) {
    ASTNode T;
    T.role = "type";
    T.kind = "LValueReference";
    T.arcana = "LValueReferenceType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(1), R));
    return T;
  }
  if (TypeStr.ends_with("*")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Pointer";
    T.arcana = "PointerType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(1), R));
    return T;
  }
  if (TypeStr.starts_with("const ") || TypeStr.starts_with("const\t")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Qualified";
    T.detail = "const";
    T.arcana = "QualType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_front(5), R));
    return T;
  }
  if (TypeStr.ends_with(" const")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Qualified";
    T.detail = "const";
    T.arcana = "QualType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(6), R));
    return T;
  }
  if (TypeStr.starts_with("volatile ") || TypeStr.starts_with("volatile\t")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Qualified";
    T.detail = "volatile";
    T.arcana = "QualType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_front(8), R));
    return T;
  }

  static const llvm::StringSet<> Builtins = {
      "void", "bool", "char", "signed char", "unsigned char",
      "short", "unsigned short", "int", "unsigned int", "signed int",
      "long", "unsigned long", "long long", "unsigned long long",
      "float", "double", "long double", "wchar_t", "char8_t",
      "char16_t", "char32_t", "size_t", "int8_t", "uint8_t",
      "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
      "nullptr_t", "ptrdiff_t", "intptr_t", "uintptr_t"};

  if (Builtins.contains(TypeStr)) {
    ASTNode T;
    T.role = "type";
    T.kind = "Builtin";
    T.detail = TypeStr.str();
    T.arcana = "BuiltinType '" + TypeStr.str() + "'";
    T.range = R;
    return T;
  }

  if (TypeStr == "auto") {
    ASTNode T;
    T.role = "type";
    T.kind = "Auto";
    T.detail = "auto";
    T.arcana = "AutoType";
    T.range = R;
    return T;
  }

  ASTNode T;
  T.role = "type";
  T.kind = "Record";
  T.detail = TypeStr.str();
  T.arcana = "RecordType '" + TypeStr.str() + "'";
  T.range = R;
  if (TypeStr.contains("::")) {
    size_t Colons = TypeStr.rfind("::");
    llvm::StringRef Name = TypeStr.drop_front(Colons + 2).trim();
    if (!Name.empty()) {
      std::string Ns = TypeStr.take_front(Colons + 2).str();
      T.detail = Name.str();
      ASTNode Spec;
      Spec.role = "specifier";
      Spec.kind = "Namespace";
      Spec.detail = Ns;
      Spec.range = R;
      T.children.push_back(std::move(Spec));
    }
  }
  if (T.detail.empty())
    T.detail = "(anonymous)";
  return T;
}

struct ExtractedParam {
  std::string Name;
  std::string Type;
  Range ParamRange;
  Range TypeRange;
  Range NameRange;
};

static std::vector<ExtractedParam>
extractParameters(llvm::ArrayRef<pseudo::Token> Tokens, const ParseOutput &Out,
                  llvm::StringRef Code) {
  std::vector<ExtractedParam> Params;
  size_t LParen = Tokens.size();
  for (size_t I = 0; I < Tokens.size(); ++I) {
    if (Tokens[I].Kind == tok::l_paren) {
      LParen = I;
      break;
    }
  }
  if (LParen == Tokens.size())
    return Params;

  int Depth = 1;
  size_t RParen = LParen + 1;
  while (RParen < Tokens.size() && Depth > 0) {
    if (Tokens[RParen].Kind == tok::l_paren)
      ++Depth;
    else if (Tokens[RParen].Kind == tok::r_paren)
      --Depth;
    if (Depth == 0)
      break;
    ++RParen;
  }

  size_t Cur = LParen + 1;
  while (Cur < RParen) {
    if (Tokens[Cur].Kind == tok::comma) {
      ++Cur;
      continue;
    }
    size_t ParamStart = Cur;
    int PDepth = 0;
    int ADepth = 0;
    while (Cur < RParen) {
      if (Tokens[Cur].Kind == tok::l_paren)
        ++PDepth;
      else if (Tokens[Cur].Kind == tok::r_paren)
        --PDepth;
      else if (Tokens[Cur].Kind == tok::less)
        ++ADepth;
      else if (Tokens[Cur].Kind == tok::greater)
        --ADepth;
      else if (Tokens[Cur].Kind == tok::comma && PDepth == 0 && ADepth <= 0)
        break;
      ++Cur;
    }
    size_t ParamEnd = Cur;
    if (ParamStart >= ParamEnd)
      continue;

    // Ignore default arguments after '='
    size_t RealEnd = ParamEnd;
    for (size_t I = ParamStart; I < ParamEnd; ++I) {
      if (Tokens[I].Kind == tok::equal) {
        RealEnd = I;
        break;
      }
    }

    std::string ParamName;
    std::string ParamType;
    size_t TypeStartIdx = ParamStart;
    size_t TypeEndIdx = RealEnd;
    size_t NameTokIdx = RealEnd;

    if (RealEnd == ParamStart + 1) {
      size_t TStart = tokenStartOffset(Tokens[ParamStart], Out);
      size_t TEnd = tokenEndOffset(Tokens[ParamStart], Out);
      if (TStart < TEnd && TEnd <= Code.size())
        ParamType = Code.slice(TStart, TEnd).trim().str();
      TypeEndIdx = ParamStart + 1;
    } else if (RealEnd > ParamStart + 1) {
      size_t LastIdx = RealEnd - 1;
      if (Tokens[LastIdx].Kind == tok::raw_identifier ||
          Tokens[LastIdx].Kind == tok::identifier) {
        ParamName = getOrigToken(Tokens[LastIdx], Out).text().str();
        NameTokIdx = LastIdx;
        TypeEndIdx = LastIdx;
        size_t TStart = tokenStartOffset(Tokens[ParamStart], Out);
        size_t TEnd = tokenStartOffset(Tokens[LastIdx], Out);
        if (TStart < TEnd && TEnd <= Code.size())
          ParamType = Code.slice(TStart, TEnd).trim().str();
      } else {
        size_t TStart = tokenStartOffset(Tokens[ParamStart], Out);
        size_t TEnd = tokenEndOffset(Tokens[RealEnd - 1], Out);
        if (TStart < TEnd && TEnd <= Code.size())
          ParamType = Code.slice(TStart, TEnd).trim().str();
      }
    }

    if (ParamType.empty())
      ParamType = "int";
    if (ParamType != "void" || !ParamName.empty()) {
      ExtractedParam EP;
      EP.Name = std::move(ParamName);
      EP.Type = std::move(ParamType);

      size_t PStartOff = tokenStartOffset(Tokens[ParamStart], Out);
      size_t PEndOff = tokenEndOffset(Tokens[ParamEnd - 1], Out);
      EP.ParamRange = Range{offsetToPosition(Code, PStartOff),
                            offsetToPosition(Code, PEndOff)};

      size_t TStartOff = tokenStartOffset(Tokens[TypeStartIdx], Out);
      size_t TEndOff = (TypeEndIdx > TypeStartIdx)
                           ? tokenEndOffset(Tokens[TypeEndIdx - 1], Out)
                           : PEndOff;
      EP.TypeRange = Range{offsetToPosition(Code, TStartOff),
                           offsetToPosition(Code, TEndOff)};

      if (NameTokIdx < RealEnd) {
        EP.NameRange = tokenRange(Tokens[NameTokIdx], Out, Code);
      } else {
        EP.NameRange = EP.ParamRange;
      }

      Params.push_back(std::move(EP));
    }
  }
  return Params;
}

static ASTNode
buildFunctionProto(llvm::StringRef ReturnType,
                   llvm::ArrayRef<ExtractedParam> Params,
                   Range ProtoRange, Range ReturnTypeRange) {
  ASTNode Proto;
  Proto.role = "type";
  Proto.kind = "FunctionProto";
  Proto.range = ProtoRange;
  std::string ParamSummary;
  for (size_t I = 0; I < Params.size(); ++I) {
    if (I > 0)
      ParamSummary += ", ";
    ParamSummary += Params[I].Type;
    if (!Params[I].Name.empty()) {
      ParamSummary += " ";
      ParamSummary += Params[I].Name;
    }
  }
  std::string Sig = ReturnType.empty()
                        ? "(" + ParamSummary + ")"
                        : ReturnType.str() + " (" + ParamSummary + ")";
  Proto.arcana = "QualType '" + Sig + "'";
  if (!ReturnType.empty())
    Proto.children.push_back(buildTypeNode(ReturnType, ReturnTypeRange));
  for (const auto &P : Params) {
    ASTNode Parm;
    Parm.role = "declaration";
    Parm.kind = "ParmVar";
    Parm.detail = P.Name.empty() ? "(anonymous)" : P.Name;
    Parm.range = P.ParamRange;
    Parm.arcana = "ParmVarDecl " + Parm.detail + " '" + P.Type + "'";
    Parm.children.push_back(buildTypeNode(P.Type, P.TypeRange));
    Proto.children.push_back(std::move(Parm));
  }
  return Proto;
}

static bool containsSymbol(const pseudo::ForestNode *N, pseudo::SymbolID Target) {
  if (!N)
    return false;
  if (N->symbol() == Target)
    return true;
  if (N->kind() == pseudo::ForestNode::Sequence) {
    for (const auto *Child : N->elements()) {
      if (containsSymbol(Child, Target))
        return true;
    }
  } else if (N->kind() == pseudo::ForestNode::Ambiguous) {
    for (const auto *Alt : N->alternatives()) {
      if (containsSymbol(Alt, Target))
        return true;
    }
  }
  return false;
}

static bool isLikelyFunctionDeclaration(llvm::ArrayRef<pseudo::Token> Tokens,
                                        size_t LParenIdx,
                                        bool InsideFunctionBody) {
  if (LParenIdx >= Tokens.size())
    return false;

  size_t RParenIdx = LParenIdx + 1;
  int Depth = 1;
  while (RParenIdx < Tokens.size() && Depth > 0) {
    if (Tokens[RParenIdx].Kind == tok::l_paren)
      ++Depth;
    else if (Tokens[RParenIdx].Kind == tok::r_paren)
      --Depth;
    if (Depth == 0)
      break;
    ++RParenIdx;
  }
  if (RParenIdx >= Tokens.size() && Depth > 0)
    return false;

  // Empty parameter list: ()
  if (RParenIdx == LParenIdx + 1)
    return !InsideFunctionBody;

  // If any token inside (...) is a literal, member access, call, or operator,
  // then this is direct-initialization of a variable (constructor arguments)!
  for (size_t I = LParenIdx + 1; I < RParenIdx; ++I) {
    auto K = Tokens[I].Kind;
    if (K == tok::string_literal || K == tok::wide_string_literal ||
        K == tok::utf8_string_literal || K == tok::utf16_string_literal ||
        K == tok::utf32_string_literal || K == tok::numeric_constant ||
        K == tok::char_constant || K == tok::kw_true || K == tok::kw_false ||
        K == tok::kw_nullptr || K == tok::kw_this || K == tok::period ||
        K == tok::arrow || K == tok::question || K == tok::plusplus ||
        K == tok::minusminus || K == tok::exclaim || K == tok::tilde)
      return false;
  }

  if (InsideFunctionBody) {
    bool HasTypeKeywordInParams = false;
    for (size_t I = LParenIdx + 1; I < RParenIdx; ++I) {
      auto K = Tokens[I].Kind;
      if (K == tok::kw_int || K == tok::kw_char || K == tok::kw_bool ||
          K == tok::kw_void || K == tok::kw_float || K == tok::kw_double ||
          K == tok::kw_auto) {
        HasTypeKeywordInParams = true;
        break;
      }
    }
    if (!HasTypeKeywordInParams)
      return false;
  }

  return true;
}

static void buildASTNodes(const pseudo::ForestNode *N, pseudo::Token::Index End,
                          const ParseOutput &Out, llvm::StringRef Code,
                          std::vector<ASTNode> &Nodes, bool InsideClass = false,
                          llvm::StringRef EnclosingClass = "",
                          llvm::StringRef DeclaredType = "",
                          std::optional<Range> DeclaredTypeRange = std::nullopt) {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    const pseudo::ForestNode *DeclAlt = nullptr;
    const pseudo::ForestNode *ExprAlt = nullptr;
    for (const auto *Alt : Alts) {
      if (containsSymbol(Alt, pseudo::cxx::Symbol::declaration_statement) ||
          containsSymbol(Alt, pseudo::cxx::Symbol::simple_declaration))
        DeclAlt = Alt;
      else if (containsSymbol(Alt, pseudo::cxx::Symbol::expression_statement))
        ExprAlt = Alt;
    }
    if (DeclAlt && ExprAlt) {
      auto StartTok = N->startTokenIndex();
      auto NodeTokens =
          Out.ParseableStream.tokens().slice(StartTok, End - StartTok);
      size_t SemiIdx = NodeTokens.size();
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::semi) {
          SemiIdx = I;
          break;
        }
      }
      bool HasTypeKeyword = false;
      int IdentCountBeforeParenOrEqual = 0;
      for (size_t I = 0; I < SemiIdx; ++I) {
        const auto &T = NodeTokens[I];
        if (T.Kind == tok::l_paren || T.Kind == tok::equal)
          break;
        if (T.Kind == tok::kw_auto || T.Kind == tok::kw_void ||
            T.Kind == tok::kw_int || T.Kind == tok::kw_char ||
            T.Kind == tok::kw_bool || T.Kind == tok::kw_float ||
            T.Kind == tok::kw_double || T.Kind == tok::kw_class ||
            T.Kind == tok::kw_struct || T.Kind == tok::kw_static ||
            T.Kind == tok::kw_constexpr || T.Kind == tok::kw_const) {
          HasTypeKeyword = true;
        }
        if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
          ++IdentCountBeforeParenOrEqual;
      }
      const pseudo::ForestNode *Chosen =
          (HasTypeKeyword || IdentCountBeforeParenOrEqual >= 2) ? DeclAlt
                                                                : ExprAlt;
      buildASTNodes(Chosen, End, Out, Code, Nodes, InsideClass, EnclosingClass,
                    DeclaredType, DeclaredTypeRange);
      return;
    }

    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    buildASTNodes(Alts[AltIdx], End, Out, Code, Nodes, InsideClass,
                  EnclosingClass, DeclaredType, DeclaredTypeRange);
    return;
  }

  auto StartTok = N->startTokenIndex();
  auto EndTok = End;
  if (StartTok >= EndTok || StartTok >= Out.ParseableStream.tokens().size())
    return;

  auto NodeTokens =
      Out.ParseableStream.tokens().slice(StartTok, EndTok - StartTok);
  Range NRange = nodeRange(StartTok, EndTok, Out, Code);

  if (N->kind() == pseudo::ForestNode::Opaque) {
    std::vector<LexicalScope> TempScopes;
    size_t TempScopeId = 0;
    LexicalScope RootS;
    RootS.Id = 0;
    TempScopes.push_back(std::move(RootS));
    scanOpaqueDeclarations(StartTok, End, Out, Code, TempScopes, TempScopeId,
                           EnclosingClass);
    for (const auto &D : TempScopes[0].Decls) {
      ASTNode DN;
      DN.role = "declaration";
      DN.kind = D.IsMember ? "Field"
                           : (D.Kind == DeclKind::Function
                                  ? "Function"
                                  : "Var");
      DN.detail = D.Name;
      DN.range = D.DeclRange;
      DN.arcana = DN.kind + "Decl " + D.Name + " '" + D.TypeName + "'";
      DN.children.push_back(buildTypeNode(D.TypeName, D.NameRange));
      Nodes.push_back(std::move(DN));
    }
    return;
  }

  if (N->kind() != pseudo::ForestNode::Sequence)
    return;

  pseudo::SymbolID Sym = N->symbol();

  // 1. Namespaces
  if (Sym == pseudo::cxx::Symbol::named_namespace_definition ||
      Sym == pseudo::cxx::Symbol::nested_namespace_definition ||
      Sym == pseudo::cxx::Symbol::unnamed_namespace_definition) {
    ASTNode NS;
    NS.role = "declaration";
    NS.kind = "Namespace";
    std::string NsName;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (!NsName.empty() && I > 0 &&
            NodeTokens[I - 1].Kind == tok::coloncolon)
          NsName += "::";
        NsName += getOrigToken(NodeTokens[I], Out).text().str();
      }
    }
    NS.detail = NsName.empty() ? "(anonymous)" : NsName;
    NS.arcana = "NamespaceDecl " + NS.detail;
    NS.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, NS.children, false, "");
    }
    Nodes.push_back(std::move(NS));
    return;
  }

  // 2. Class specifier
  if (Sym == pseudo::cxx::Symbol::class_specifier) {
    ASTNode CR;
    CR.role = "declaration";
    CR.kind = "CXXRecord";
    std::string ClassName;
    bool IsStruct = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace ||
          NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::kw_struct)
        IsStruct = true;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        ClassName = getOrigToken(NodeTokens[I], Out).text().str();
    }
    if (ClassName.empty())
      ClassName = IsStruct ? "(anonymous struct)" : "(anonymous class)";
    CR.detail = ClassName;
    CR.arcana = "CXXRecordDecl " + ClassName;
    CR.range = NRange;

    // Base specifiers
    bool InBase = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::colon) {
        InBase = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (InBase && (NodeTokens[I].Kind == tok::raw_identifier ||
                     NodeTokens[I].Kind == tok::identifier)) {
        std::string BaseName = getOrigToken(NodeTokens[I], Out).text().str();
        ASTNode Base;
        Base.role = "base";
        Base.kind = "public";
        Base.range = tokenRange(NodeTokens[I], Out, Code);
        Base.children.push_back(
            buildTypeNode(BaseName, Base.range.value_or(Range{})));
        CR.children.push_back(std::move(Base));
      }
    }

    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, CR.children, true,
                    ClassName);
    }
    Nodes.push_back(std::move(CR));
    return;
  }

  // 3. Enum specifier
  if (Sym == pseudo::cxx::Symbol::enum_specifier ||
      Sym == pseudo::cxx::Symbol::opaque_enum_declaration) {
    ASTNode EN;
    EN.role = "declaration";
    EN.kind = "Enum";
    std::string EnumName;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace ||
          NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        EnumName = getOrigToken(NodeTokens[I], Out).text().str();
    }
    if (EnumName.empty())
      EnumName = "(anonymous enum)";
    EN.detail = EnumName;
    EN.arcana = "EnumDecl " + EnumName;
    EN.range = NRange;

    // Enumerator members
    bool InBraces = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace) {
        InBraces = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::r_brace)
        break;
      if (InBraces && (NodeTokens[I].Kind == tok::raw_identifier ||
                       NodeTokens[I].Kind == tok::identifier)) {
        std::string MemName = getOrigToken(NodeTokens[I], Out).text().str();
        ASTNode EC;
        EC.role = "declaration";
        EC.kind = "EnumConstant";
        EC.detail = MemName;
        EC.arcana = "EnumConstantDecl " + MemName;
        EC.range = tokenRange(NodeTokens[I], Out, Code);
        EN.children.push_back(std::move(EC));
        while (I + 1 < NodeTokens.size() &&
               NodeTokens[I + 1].Kind != tok::comma &&
               NodeTokens[I + 1].Kind != tok::r_brace)
          ++I;
      }
    }
    Nodes.push_back(std::move(EN));
    return;
  }

  // Access specifier (public:, protected:, private:)
  if (Sym == pseudo::cxx::Symbol::access_specifier) {
    ASTNode AN;
    AN.role = "declaration";
    AN.kind = "AccessSpec";
    AN.detail = "";
    AN.arcana = "AccessSpecDecl";
    AN.range = NRange;
    Nodes.push_back(std::move(AN));
    return;
  }

  // 4. Function definition
  if (Sym == pseudo::cxx::Symbol::function_definition) {
    auto Children = N->elements();

    // Scan NodeTokens before '{' or ':' (ctor initializer) to find the parameter list '('
    size_t LParenIdx = NodeTokens.size();
    int AngleDepth = 0;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::less)
        ++AngleDepth;
      else if (NodeTokens[I].Kind == tok::greater) {
        if (AngleDepth > 0)
          --AngleDepth;
      } else if (NodeTokens[I].Kind == tok::l_paren && AngleDepth == 0) {
        LParenIdx = I;
        break;
      }
    }

    std::string FuncName;
    const pseudo::Token *NameTok = nullptr;
    const pseudo::Token *FirstNameTok = nullptr;
    if (LParenIdx < NodeTokens.size()) {
      int OpTokIdx = -1;
      for (int I = 0; I < static_cast<int>(LParenIdx); ++I) {
        if (NodeTokens[I].Kind == tok::kw_operator) {
          OpTokIdx = I;
          break;
        }
      }
      if (OpTokIdx >= 0) {
        int EndNameIdx = static_cast<int>(LParenIdx) - 1;
        while (EndNameIdx > OpTokIdx &&
               (NodeTokens[EndNameIdx].Kind == tok::r_paren ||
                NodeTokens[EndNameIdx].Kind == tok::l_paren))
          --EndNameIdx;
        int StartNameIdx = OpTokIdx;
        while (StartNameIdx >= 2 &&
               NodeTokens[StartNameIdx - 1].Kind == tok::coloncolon &&
               (NodeTokens[StartNameIdx - 2].Kind == tok::raw_identifier ||
                NodeTokens[StartNameIdx - 2].Kind == tok::identifier)) {
          StartNameIdx -= 2;
        }
        FirstNameTok = &NodeTokens[StartNameIdx];
        NameTok = &NodeTokens[EndNameIdx];
        size_t SOff = tokenStartOffset(*FirstNameTok, Out);
        size_t EOff = tokenEndOffset(*NameTok, Out);
        if (SOff < EOff && EOff <= Code.size())
          FuncName = Code.slice(SOff, EOff).trim().str();
      } else {
        int EndNameIdx = static_cast<int>(LParenIdx) - 1;
        while (EndNameIdx >= 0 &&
               NodeTokens[EndNameIdx].Kind != tok::raw_identifier &&
               NodeTokens[EndNameIdx].Kind != tok::identifier)
          --EndNameIdx;

        if (EndNameIdx >= 0) {
          NameTok = &NodeTokens[EndNameIdx];
          int StartNameIdx = EndNameIdx;
          while (StartNameIdx > 0) {
            if (NodeTokens[StartNameIdx - 1].Kind == tok::coloncolon &&
                StartNameIdx >= 2 &&
                (NodeTokens[StartNameIdx - 2].Kind == tok::raw_identifier ||
                 NodeTokens[StartNameIdx - 2].Kind == tok::identifier)) {
              StartNameIdx -= 2;
            } else if (NodeTokens[StartNameIdx - 1].Kind == tok::tilde) {
              StartNameIdx -= 1;
            } else {
              break;
            }
          }
          FirstNameTok = &NodeTokens[StartNameIdx];
          size_t SOff = tokenStartOffset(*FirstNameTok, Out);
          size_t EOff = tokenEndOffset(*NameTok, Out);
          if (SOff < EOff && EOff <= Code.size())
            FuncName = Code.slice(SOff, EOff).trim().str();
        }
      }
    }

    std::string ReturnType = "void";
    Range ReturnTypeRange = NRange;
    if (FirstNameTok && FirstNameTok > NodeTokens.data()) {
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*FirstNameTok, Out);
      if (TStart < TEnd && TEnd <= Code.size()) {
        std::string RT = Code.slice(TStart, TEnd).trim().str();
        static const char *const Specifiers[] = {
            "inline", "static", "virtual", "constexpr", "consteval",
            "constinit", "friend", "explicit"};
        bool Stripped = true;
        while (Stripped) {
          Stripped = false;
          for (const char *Spec : Specifiers) {
            llvm::StringRef RRef(RT);
            if (RRef.starts_with(Spec) &&
                (RRef.size() == strlen(Spec) || isspace(RRef[strlen(Spec)]))) {
              RT = RRef.drop_front(strlen(Spec)).trim().str();
              Stripped = true;
            }
          }
        }
        if (!RT.empty())
          ReturnType = RT;
        pseudo::Token::Index FIdx =
            FirstNameTok - Out.ParseableStream.tokens().data();
        if (FIdx > StartTok)
          ReturnTypeRange = nodeRange(StartTok, FIdx, Out, Code);
      }
    } else {
      ReturnType = "";
    }

    if (ReturnType == "auto") {
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::arrow && I + 1 < NodeTokens.size()) {
          size_t RStart = tokenStartOffset(NodeTokens[I + 1], Out);
          size_t REnd = tokenEndOffset(NodeTokens.back(), Out);
          for (size_t J = I + 1; J < NodeTokens.size(); ++J) {
            if (NodeTokens[J].Kind == tok::l_brace ||
                NodeTokens[J].Kind == tok::semi) {
              REnd = tokenStartOffset(NodeTokens[J], Out);
              break;
            }
          }
          if (RStart < REnd && REnd <= Code.size()) {
            std::string Trailing = Code.slice(RStart, REnd).trim().str();
            if (!Trailing.empty())
              ReturnType = Trailing;
          }
          break;
        }
      }
    }

    size_t RParenIdx = LParenIdx;
    if (LParenIdx < NodeTokens.size()) {
      int Depth = 1;
      RParenIdx = LParenIdx + 1;
      while (RParenIdx < NodeTokens.size() && Depth > 0) {
        if (NodeTokens[RParenIdx].Kind == tok::l_paren)
          ++Depth;
        else if (NodeTokens[RParenIdx].Kind == tok::r_paren)
          --Depth;
        if (Depth == 0)
          break;
        ++RParenIdx;
      }
    }

    Range ProtoRange = (RParenIdx < NodeTokens.size())
                           ? nodeRange(StartTok, StartTok + RParenIdx + 1, Out, Code)
                           : NRange;

    auto Params = extractParameters(NodeTokens, Out, Code);

    std::string Kind = "Function";
    std::string Detail = FuncName;
    if (FuncName.find("::") != std::string::npos) {
      Kind = "CXXMethod";
      Detail = FuncName.substr(FuncName.rfind("::") + 2);
    }

    // Check for operator overload: operator(), operator[], operator=, conversion operator
    if (FuncName.find("operator") != std::string::npos) {
      size_t OpPos = FuncName.find("operator");
      std::string AfterOp = FuncName.substr(OpPos + 8);
      std::string AfterTrimmed = llvm::StringRef(AfterOp).trim().str();
      if (!AfterTrimmed.empty() && AfterTrimmed.front() != '(' &&
          AfterTrimmed.front() != '[' && AfterTrimmed.front() != '=' &&
          AfterTrimmed.front() != '!' && AfterTrimmed.front() != '+' &&
          AfterTrimmed.front() != '-' && AfterTrimmed.front() != '*' &&
          AfterTrimmed.front() != '<' && AfterTrimmed.front() != '>') {
        Kind = "CXXConversion";
        std::string ConvType = AfterTrimmed;
        if (ConvType.rfind("::") != std::string::npos)
          ConvType = ConvType.substr(ConvType.rfind("::") + 2);
        Detail = "operator " + ConvType;
      } else {
        if (AfterTrimmed.empty())
          Detail = "operator()";
        else
          Detail = "operator" + AfterTrimmed;
      }
    }

    // Check for constructor / destructor
    if (llvm::StringRef(Detail).starts_with("~")) {
      Kind = "CXXDestructor";
      Detail = "";
    } else if (FuncName.find("::") != std::string::npos) {
      std::string Qualifier = FuncName.substr(0, FuncName.rfind("::"));
      std::string ClassPart =
          Qualifier.substr(Qualifier.rfind("::") == std::string::npos
                               ? 0
                               : Qualifier.rfind("::") + 2);
      if (ClassPart == Detail) {
        Kind = "CXXConstructor";
        Detail = "";
      }
    } else if (InsideClass && Detail == EnclosingClass) {
      Kind = "CXXConstructor";
      Detail = "";
    } else if (InsideClass && Kind != "CXXConversion") {
      Kind = "CXXMethod";
    }

    if (Kind == "CXXConstructor" || Kind == "CXXDestructor") {
      ReturnType = "";
    } else if (Kind == "CXXConversion") {
      size_t OpPos = FuncName.find("operator");
      if (OpPos != std::string::npos) {
        std::string AfterOp = FuncName.substr(OpPos + 8);
        ReturnType = llvm::StringRef(AfterOp).trim().str();
      }
      int OpTokIdx = -1;
      for (size_t I = 0; I < LParenIdx; ++I) {
        if (NodeTokens[I].Kind == tok::kw_operator) {
          OpTokIdx = static_cast<int>(I);
          break;
        }
      }
      if (OpTokIdx >= 0 && LParenIdx > static_cast<size_t>(OpTokIdx + 1)) {
        ReturnTypeRange =
            nodeRange(StartTok + OpTokIdx + 1, StartTok + LParenIdx, Out, Code);
      }
    }

    ASTNode FN;
    FN.role = "declaration";
    FN.kind = Kind;
    FN.detail = Detail;
    FN.range = NRange;
    FN.arcana = Kind + "Decl " + Detail;

    FN.children.push_back(
        buildFunctionProto(ReturnType, Params, ProtoRange, ReturnTypeRange));

    for (size_t I = 0; I < Children.size(); ++I) {
      if (Children[I]->symbol() == pseudo::cxx::Symbol::function_body ||
          Children[I]->symbol() == pseudo::cxx::Symbol::compound_statement) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, FN.children,
                      /*InsideClass=*/false, /*EnclosingClass=*/"");
      }
    }
    Nodes.push_back(std::move(FN));
    return;
  }

  // 5. Simple declaration
  if (Sym == pseudo::cxx::Symbol::simple_declaration ||
      Sym == pseudo::cxx::Symbol::member_declaration) {
    auto Children = N->elements();

    // Check if decl-specifier-seq contains a class, struct, union, or enum definition
    bool HasClassOrEnumDef = false;
    for (size_t I = 0; I < Children.size(); ++I) {
      if (containsSymbol(Children[I], pseudo::cxx::Symbol::class_specifier) ||
          containsSymbol(Children[I], pseudo::cxx::Symbol::enum_specifier)) {
        HasClassOrEnumDef = true;
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                      EnclosingClass, DeclaredType);
      }
    }

    // Check if there is an init-declarator-list (e.g. variables declared)
    const pseudo::ForestNode *InitDeclList = nullptr;
    for (size_t I = 0; I < Children.size(); ++I) {
      if (Children[I]->symbol() == pseudo::cxx::Symbol::init_declarator_list ||
          Children[I]->symbol() == pseudo::cxx::Symbol::member_declarator_list) {
        InitDeclList = Children[I];
        break;
      }
    }

    // If it's a class/struct/enum definition without declarators (e.g. struct Foo { ... };),
    // then we are done! No variable should be created.
    if (HasClassOrEnumDef && !InitDeclList)
      return;

    // Check if it's a forward declaration (e.g. struct Foo; or class Bar;)
    if (!InitDeclList && !HasClassOrEnumDef) {
      bool HasElaboratedType = false;
      for (size_t I = 0; I < Children.size(); ++I) {
        if (containsSymbol(Children[I],
                           pseudo::cxx::Symbol::elaborated_type_specifier)) {
          HasElaboratedType = true;
          break;
        }
      }
      if (HasElaboratedType) {
        std::string FwdName;
        for (const auto &T : NodeTokens) {
          if (T.Kind == tok::semi)
            break;
          if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
            FwdName = getOrigToken(T, Out).text().str();
        }
        if (!FwdName.empty()) {
          ASTNode CR;
          CR.role = "declaration";
          CR.kind = "CXXRecord";
          CR.detail = FwdName;
          CR.arcana = "CXXRecordDecl " + FwdName;
          CR.range = NRange;
          Nodes.push_back(std::move(CR));
          return;
        }
      }
    }

    // Extract the declared type from decl-specifier-seq (Children[0])
    std::string ExtractedType;
    std::optional<Range> ExtractedTypeRange;
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart = tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd = tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size()) {
        ExtractedType = Code.slice(OffStart, OffEnd).trim().str();
        ExtractedTypeRange = nodeRange(S0, S1, Out, Code);
      }
    }
    if (ExtractedType.empty()) {
      size_t FirstIdentIdx = NodeTokens.size();
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
            NodeTokens[I].Kind == tok::l_brace)
          break;
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          FirstIdentIdx = I;
          break;
        }
      }
      if (FirstIdentIdx < NodeTokens.size()) {
        ExtractedType = getOrigToken(NodeTokens[FirstIdentIdx], Out).text().str();
        ExtractedTypeRange =
            nodeRange(StartTok, StartTok + FirstIdentIdx + 1, Out, Code);
      }
    }
    if (ExtractedType.empty())
      ExtractedType = "int";

    size_t BeforeCount = Nodes.size();
    for (size_t I = 0; I < Children.size(); ++I) {
      if (Children[I]->symbol() == pseudo::cxx::Symbol::decl_specifier_seq)
        continue;
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                    EnclosingClass, ExtractedType, ExtractedTypeRange);
    }
    if (Nodes.size() > BeforeCount)
      return;

    if (HasClassOrEnumDef)
      return;

    // Fallback: parse as single variable or function prototype
    size_t SemiIdx = NodeTokens.size();
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi) {
        SemiIdx = I;
        break;
      }
    }

    size_t TypeStartTok = 0;
    while (TypeStartTok < SemiIdx &&
           (NodeTokens[TypeStartTok].Kind == tok::kw_static ||
            NodeTokens[TypeStartTok].Kind == tok::kw_const ||
            NodeTokens[TypeStartTok].Kind == tok::kw_inline ||
            NodeTokens[TypeStartTok].Kind == tok::kw_constexpr ||
            NodeTokens[TypeStartTok].Kind == tok::kw_extern ||
            NodeTokens[TypeStartTok].Kind == tok::kw_mutable ||
            NodeTokens[TypeStartTok].Kind == tok::kw_friend))
      ++TypeStartTok;

    bool HasTypeKeyword = (TypeStartTok > 0);
    int IdentCountBeforeParenOrEqual = 0;
    for (size_t I = TypeStartTok; I < SemiIdx; ++I) {
      if (NodeTokens[I].Kind == tok::l_paren || NodeTokens[I].Kind == tok::equal)
        break;
      if (NodeTokens[I].Kind == tok::kw_auto || NodeTokens[I].Kind == tok::kw_void ||
          NodeTokens[I].Kind == tok::kw_int || NodeTokens[I].Kind == tok::kw_char ||
          NodeTokens[I].Kind == tok::kw_bool || NodeTokens[I].Kind == tok::kw_float ||
          NodeTokens[I].Kind == tok::kw_double || NodeTokens[I].Kind == tok::kw_class ||
          NodeTokens[I].Kind == tok::kw_struct) {
        HasTypeKeyword = true;
      }
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        ++IdentCountBeforeParenOrEqual;
    }

    size_t NameTokIdx = SemiIdx;
    for (size_t I = TypeStartTok; I < SemiIdx; ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTokIdx = I;
      }
      if (NodeTokens[I].Kind == tok::l_paren || NodeTokens[I].Kind == tok::equal)
        break;
    }

    if (NameTokIdx < SemiIdx) {
      std::string VarName =
          getOrigToken(NodeTokens[NameTokIdx], Out).text().str();

      // If no type keyword and only 1 identifier before '(', it's a function call expression!
      if (!HasTypeKeyword && IdentCountBeforeParenOrEqual <= 1 &&
          NameTokIdx + 1 < SemiIdx && NodeTokens[NameTokIdx + 1].Kind == tok::l_paren) {
        ASTNode CallNode;
        CallNode.role = "expression";
        CallNode.kind = "Call";
        CallNode.arcana = "CallExpr";
        CallNode.range = NRange;
        ASTNode Callee;
        Callee.role = "expression";
        Callee.kind = "DeclRef";
        Callee.detail = VarName;
        Callee.arcana = "DeclRefExpr '" + VarName + "'";
        Callee.range = tokenRange(NodeTokens[NameTokIdx], Out, Code);
        CallNode.children.push_back(std::move(Callee));
        Nodes.push_back(std::move(CallNode));
        return;
      }

      bool IsFuncProto = false;
      if (NameTokIdx + 1 < SemiIdx &&
          NodeTokens[NameTokIdx + 1].Kind == tok::l_paren) {
        IsFuncProto = isLikelyFunctionDeclaration(
            NodeTokens, NameTokIdx + 1, /*InsideFunctionBody=*/!InsideClass);
      }

      if (IsFuncProto) {
        auto Params = extractParameters(NodeTokens, Out, Code);
        std::string Kind = InsideClass ? "CXXMethod" : "Function";
        std::string Detail = VarName;
        if (llvm::StringRef(Detail).starts_with("~")) {
          Kind = "CXXDestructor";
          Detail = "";
        } else if (InsideClass && Detail == EnclosingClass) {
          Kind = "CXXConstructor";
          Detail = "";
        } else if (llvm::StringRef(Detail).starts_with("operator") &&
                   Detail != "operator()") {
          Kind = "CXXConversion";
        }
        if (Kind == "CXXConstructor" || Kind == "CXXDestructor") {
          ExtractedType = "";
        } else if (Kind == "CXXConversion") {
          if (llvm::StringRef(Detail).starts_with("operator "))
            ExtractedType = Detail.substr(strlen("operator "));
        }
        ASTNode FD;
        FD.role = "declaration";
        FD.kind = Kind;
        FD.detail = Detail;
        FD.arcana = Kind + "Decl " + Detail;
        FD.range = NRange;
        FD.children.push_back(
            buildFunctionProto(ExtractedType, Params, NRange, NRange));
        Nodes.push_back(std::move(FD));
        return;
      }

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = InsideClass ? "Field" : "Var";
      VN.detail = VarName;
      VN.range = NRange;
      VN.arcana = VN.kind + "Decl " + VarName + " '" + ExtractedType + "'";
      VN.children.push_back(buildTypeNode(ExtractedType, NRange));

      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        if (Children[I]->symbol() != pseudo::cxx::Symbol::decl_specifier_seq)
          buildASTNodes(Children[I], ChildEnd, Out, Code, VN.children,
                        /*InsideClass=*/false, EnclosingClass, DeclaredType);
      }
      Nodes.push_back(std::move(VN));
      return;
    }
    return;
  }

  // 5b. Init declarator & Member declarator
  if (Sym == pseudo::cxx::Symbol::init_declarator ||
      Sym == pseudo::cxx::Symbol::member_declarator) {
    const pseudo::Token *NameTok = nullptr;
    size_t LParenIdx = NodeTokens.size();
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
          NodeTokens[I].Kind == tok::colon || NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::l_paren) {
        LParenIdx = I;
        break;
      }
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      std::string VarName = getOrigToken(*NameTok, Out).text().str();
      std::string EffectiveType =
          DeclaredType.empty() ? "int" : DeclaredType.str();
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (&NodeTokens[I] == NameTok)
          break;
        if (NodeTokens[I].Kind == tok::star)
          EffectiveType += " *";
        else if (NodeTokens[I].Kind == tok::amp)
          EffectiveType += " &";
        else if (NodeTokens[I].Kind == tok::ampamp)
          EffectiveType += " &&";
      }

      bool IsFunc =
          (LParenIdx < NodeTokens.size()) &&
          isLikelyFunctionDeclaration(NodeTokens, LParenIdx,
                                      /*InsideFunctionBody=*/!InsideClass);
      if (IsFunc) {
        auto Params = extractParameters(NodeTokens, Out, Code);
        std::string Kind = InsideClass ? "CXXMethod" : "Function";
        std::string Detail = VarName;
        if (llvm::StringRef(Detail).starts_with("~")) {
          Kind = "CXXDestructor";
          Detail = "";
        } else if (InsideClass && Detail == EnclosingClass) {
          Kind = "CXXConstructor";
          Detail = "";
        } else if (llvm::StringRef(Detail).starts_with("operator") &&
                   Detail != "operator()") {
          Kind = "CXXConversion";
        }
        if (Kind == "CXXConstructor" || Kind == "CXXDestructor") {
          EffectiveType = "";
        } else if (Kind == "CXXConversion") {
          if (llvm::StringRef(Detail).starts_with("operator "))
            EffectiveType = Detail.substr(strlen("operator "));
        }
        ASTNode FD;
        FD.role = "declaration";
        FD.kind = Kind;
        FD.detail = Detail;
        FD.arcana = Kind + "Decl " + Detail;
        FD.range = NRange;
        if (DeclaredTypeRange && FD.range)
          FD.range->start = DeclaredTypeRange->start;
        Range ReturnTypeRange = DeclaredTypeRange.value_or(NRange);
        FD.children.push_back(
            buildFunctionProto(EffectiveType, Params, NRange, ReturnTypeRange));
        Nodes.push_back(std::move(FD));
        return;
      }

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = InsideClass ? "Field" : "Var";
      VN.detail = VarName;
      VN.range = NRange;
      if (DeclaredTypeRange && VN.range)
        VN.range->start = DeclaredTypeRange->start;
      VN.arcana = VN.kind + "Decl " + VarName + " '" + EffectiveType + "'";
      VN.children.push_back(
          buildTypeNode(EffectiveType, DeclaredTypeRange.value_or(NRange)));

      auto Children = N->elements();
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        if (Children[I]->symbol() != pseudo::cxx::Symbol::declarator &&
            Children[I]->symbol() != pseudo::cxx::Symbol::noptr_declarator) {
          buildASTNodes(Children[I], ChildEnd, Out, Code, VN.children,
                        /*InsideClass=*/false, EnclosingClass, DeclaredType);
        }
      }
      Nodes.push_back(std::move(VN));
      return;
    }
    return;
  }

  // 6. Parameter declaration
  if (Sym == pseudo::cxx::Symbol::parameter_declaration) {
    auto Params = extractParameters(NodeTokens, Out, Code);
    if (!Params.empty()) {
      for (const auto &P : Params) {
        ASTNode Parm;
        Parm.role = "declaration";
        Parm.kind = "ParmVar";
        Parm.detail = P.Name.empty() ? "(anonymous)" : P.Name;
        Parm.range = P.ParamRange;
        Parm.arcana = "ParmVarDecl " + Parm.detail + " '" + P.Type + "'";
        Parm.children.push_back(buildTypeNode(P.Type, P.TypeRange));
        Nodes.push_back(std::move(Parm));
      }
      return;
    }
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    std::string ParmName = NameTok ? getOrigToken(*NameTok, Out).text().str() : "";
    Range TypeRange = NRange;
    std::string TypeStr = "int";
    if (NameTok && NameTok > NodeTokens.data()) {
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      if (TStart < TEnd && TEnd <= Code.size()) {
        TypeStr = Code.slice(TStart, TEnd).trim().str();
        size_t NIdx = NameTok - NodeTokens.data();
        TypeRange = nodeRange(StartTok, StartTok + NIdx, Out, Code);
      }
    }
    ASTNode Parm;
    Parm.role = "declaration";
    Parm.kind = "ParmVar";
    Parm.detail = ParmName.empty() ? "(anonymous)" : ParmName;
    Parm.range = NRange;
    Parm.arcana = "ParmVarDecl " + Parm.detail + " '" + TypeStr + "'";
    Parm.children.push_back(buildTypeNode(TypeStr, TypeRange));
    Nodes.push_back(std::move(Parm));
    return;
  }

  // 7. Compound statement
  if (Sym == pseudo::cxx::Symbol::compound_statement) {
    ASTNode CS;
    CS.role = "statement";
    CS.kind = "Compound";
    CS.arcana = "CompoundStmt";
    CS.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, CS.children,
                    /*InsideClass=*/false, /*EnclosingClass=*/"");
    }
    Nodes.push_back(std::move(CS));
    return;
  }

  // 8. Declaration statement
  if (Sym == pseudo::cxx::Symbol::declaration_statement) {
    ASTNode DS;
    DS.role = "statement";
    DS.kind = "Decl";
    DS.arcana = "DeclStmt";
    DS.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, DS.children,
                    /*InsideClass=*/false, /*EnclosingClass=*/"");
    }
    Nodes.push_back(std::move(DS));
    return;
  }

  // 9. Jump statement (return, break, continue, goto)
  if (Sym == pseudo::cxx::Symbol::jump_statement) {
    if (!NodeTokens.empty()) {
      if (NodeTokens.front().Kind == tok::kw_return) {
        ASTNode Ret;
        Ret.role = "statement";
        Ret.kind = "Return";
        Ret.arcana = "ReturnStmt";
        Ret.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Ret.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Ret));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_break) {
        ASTNode Brk;
        Brk.role = "statement";
        Brk.kind = "Break";
        Brk.arcana = "BreakStmt";
        Brk.range = NRange;
        Nodes.push_back(std::move(Brk));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_continue) {
        ASTNode Cont;
        Cont.role = "statement";
        Cont.kind = "Continue";
        Cont.arcana = "ContinueStmt";
        Cont.range = NRange;
        Nodes.push_back(std::move(Cont));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_goto) {
        std::string Label;
        if (NodeTokens.size() > 1)
          Label = getOrigToken(NodeTokens[1], Out).text().str();
        ASTNode Gt;
        Gt.role = "statement";
        Gt.kind = "Goto";
        Gt.detail = Label;
        Gt.arcana = "GotoStmt " + Label;
        Gt.range = NRange;
        Nodes.push_back(std::move(Gt));
        return;
      }
    }
  }

  // 10. Selection statement (if, switch)
  if (Sym == pseudo::cxx::Symbol::selection_statement) {
    if (!NodeTokens.empty() && NodeTokens.front().Kind == tok::kw_switch) {
      ASTNode Sw;
      Sw.role = "statement";
      Sw.kind = "Switch";
      Sw.arcana = "SwitchStmt";
      Sw.range = NRange;
      auto Children = N->elements();
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, Sw.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(Sw));
      return;
    }
    ASTNode IfNode;
    IfNode.role = "statement";
    IfNode.kind = "If";
    IfNode.arcana = "IfStmt";
    IfNode.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, IfNode.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(IfNode));
    return;
  }

  // 11. Iteration statement (while, do, for, range-for)
  if (Sym == pseudo::cxx::Symbol::iteration_statement) {
    if (!NodeTokens.empty()) {
      if (NodeTokens.front().Kind == tok::kw_while) {
        ASTNode Wh;
        Wh.role = "statement";
        Wh.kind = "While";
        Wh.arcana = "WhileStmt";
        Wh.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Wh.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Wh));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_do) {
        ASTNode DoNode;
        DoNode.role = "statement";
        DoNode.kind = "Do";
        DoNode.arcana = "DoStmt";
        DoNode.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, DoNode.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(DoNode));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_for) {
        bool IsRangeFor = false;
        int PDepth = 0;
        for (size_t I = 0; I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind == tok::l_paren)
            ++PDepth;
          else if (NodeTokens[I].Kind == tok::r_paren) {
            --PDepth;
            if (PDepth == 0)
              break;
          } else if (NodeTokens[I].Kind == tok::colon && PDepth == 1) {
            IsRangeFor = true;
            break;
          }
        }
        ASTNode ForNode;
        ForNode.role = "statement";
        ForNode.kind = IsRangeFor ? "CXXForRange" : "For";
        ForNode.arcana = ForNode.kind + "Stmt";
        ForNode.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, ForNode.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(ForNode));
        return;
      }
    }
  }

  // 12. Labeled statement (case, default, label)
  if (Sym == pseudo::cxx::Symbol::labeled_statement) {
    if (!NodeTokens.empty()) {
      if (NodeTokens.front().Kind == tok::kw_case) {
        ASTNode Cs;
        Cs.role = "statement";
        Cs.kind = "Case";
        Cs.arcana = "CaseStmt";
        Cs.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Cs.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Cs));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_default) {
        ASTNode Df;
        Df.role = "statement";
        Df.kind = "Default";
        Df.arcana = "DefaultStmt";
        Df.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Df.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Df));
        return;
      }
      if (NodeTokens.front().Kind == tok::raw_identifier ||
          NodeTokens.front().Kind == tok::identifier) {
        std::string Name = getOrigToken(NodeTokens.front(), Out).text().str();
        ASTNode Lb;
        Lb.role = "statement";
        Lb.kind = "Label";
        Lb.detail = Name;
        Lb.arcana = "LabelStmt " + Name;
        Lb.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Lb.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Lb));
        return;
      }
    }
  }

  // 13. Expression statement
  if (Sym == pseudo::cxx::Symbol::expression_statement) {
    if (NodeTokens.size() == 1 && NodeTokens.front().Kind == tok::semi) {
      ASTNode Nl;
      Nl.role = "statement";
      Nl.kind = "Null";
      Nl.arcana = "NullStmt";
      Nl.range = NRange;
      Nodes.push_back(std::move(Nl));
      return;
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
    }
    return;
  }

  // 14. Binary Expressions
  if (Sym == pseudo::cxx::Symbol::additive_expression ||
      Sym == pseudo::cxx::Symbol::multiplicative_expression ||
      Sym == pseudo::cxx::Symbol::shift_expression ||
      Sym == pseudo::cxx::Symbol::relational_expression ||
      Sym == pseudo::cxx::Symbol::equality_expression ||
      Sym == pseudo::cxx::Symbol::and_expression ||
      Sym == pseudo::cxx::Symbol::exclusive_or_expression ||
      Sym == pseudo::cxx::Symbol::inclusive_or_expression ||
      Sym == pseudo::cxx::Symbol::logical_and_expression ||
      Sym == pseudo::cxx::Symbol::logical_or_expression ||
      Sym == pseudo::cxx::Symbol::assignment_expression ||
      Sym == pseudo::cxx::Symbol::compare_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    if (Children.size() >= 3) {
      auto OpTokIdx = Children[1]->startTokenIndex();
      std::string OpStr =
          getOrigToken(Out.ParseableStream.tokens()[OpTokIdx], Out)
              .text()
              .str();
      ASTNode BO;
      BO.role = "expression";
      BO.kind = "BinaryOperator";
      BO.detail = OpStr;
      BO.arcana = "BinaryOperator '" + OpStr + "'";
      BO.range = NRange;

      pseudo::Token::Index LHSEnd = Children[1]->startTokenIndex();
      buildASTNodes(Children.front(), LHSEnd, Out, Code, BO.children,
                    InsideClass, EnclosingClass);
      buildASTNodes(Children.back(), End, Out, Code, BO.children, InsideClass,
                    EnclosingClass);
      Nodes.push_back(std::move(BO));
      return;
    }
  }

  // 15. Unary Expressions
  if (Sym == pseudo::cxx::Symbol::unary_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    if (Children.size() == 2) {
      if (!NodeTokens.empty() && NodeTokens.front().Kind == tok::kw_sizeof) {
        ASTNode SO;
        SO.role = "expression";
        SO.kind = "UnaryExprOrTypeTrait";
        SO.detail = "sizeof";
        SO.arcana = "UnaryExprOrTypeTraitExpr sizeof";
        SO.range = NRange;
        buildASTNodes(Children[1], End, Out, Code, SO.children, InsideClass,
                      EnclosingClass);
        Nodes.push_back(std::move(SO));
        return;
      }
      auto OpTokIdx = Children[0]->startTokenIndex();
      std::string OpStr =
          getOrigToken(Out.ParseableStream.tokens()[OpTokIdx], Out)
              .text()
              .str();
      ASTNode UO;
      UO.role = "expression";
      UO.kind = "UnaryOperator";
      UO.detail = OpStr;
      UO.arcana = "UnaryOperator '" + OpStr + "'";
      UO.range = NRange;
      buildASTNodes(Children[1], End, Out, Code, UO.children, InsideClass,
                    EnclosingClass);
      Nodes.push_back(std::move(UO));
      return;
    }
  }

  // 16. Postfix Expressions
  if (Sym == pseudo::cxx::Symbol::postfix_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }

    if (NodeTokens.back().Kind == tok::plusplus ||
        NodeTokens.back().Kind == tok::minusminus) {
      std::string OpStr =
          NodeTokens.back().Kind == tok::plusplus ? "++" : "--";
      ASTNode UO;
      UO.role = "expression";
      UO.kind = "UnaryOperator";
      UO.detail = OpStr;
      UO.arcana = "UnaryOperator '" + OpStr + "'";
      UO.range = NRange;
      buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                    UO.children, InsideClass, EnclosingClass);
      Nodes.push_back(std::move(UO));
      return;
    }

    if (NodeTokens.back().Kind == tok::r_paren) {
      ASTNode CallNode;
      CallNode.role = "expression";
      CallNode.kind = "Call";
      CallNode.arcana = "CallExpr";
      CallNode.range = NRange;

      buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                    CallNode.children, InsideClass, EnclosingClass);
      for (size_t I = 1; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, CallNode.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(CallNode));
      return;
    }

    if (NodeTokens.back().Kind == tok::r_square) {
      ASTNode AS;
      AS.role = "expression";
      AS.kind = "ArraySubscript";
      AS.arcana = "ArraySubscriptExpr";
      AS.range = NRange;
      buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                    AS.children, InsideClass, EnclosingClass);
      for (size_t I = 1; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, AS.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(AS));
      return;
    }

    bool HasPeriod = false;
    bool HasArrow = false;
    size_t AccessIdx = 0;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::period) {
        HasPeriod = true;
        AccessIdx = I;
        break;
      }
      if (NodeTokens[I].Kind == tok::arrow) {
        HasArrow = true;
        AccessIdx = I;
        break;
      }
    }
    if (HasPeriod || HasArrow) {
      std::string MemberName;
      for (size_t I = AccessIdx + 1; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          MemberName = getOrigToken(NodeTokens[I], Out).text().str();
          break;
        }
      }
      if (!MemberName.empty()) {
        ASTNode ME;
        ME.role = "expression";
        ME.kind = "Member";
        ME.detail = MemberName;
        ME.arcana = "MemberExpr '" + std::string(HasArrow ? "->" : ".") +
                    MemberName + "'";
        ME.range = NRange;
        buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                      ME.children, InsideClass, EnclosingClass);
        Nodes.push_back(std::move(ME));
        return;
      }
    }
  }

  // 17. Primary Expressions
  if (Sym == pseudo::cxx::Symbol::primary_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    if (!NodeTokens.empty() && NodeTokens.front().Kind == tok::kw_this) {
      ASTNode Th;
      Th.role = "expression";
      Th.kind = "CXXThis";
      Th.detail = "implicit";
      Th.arcana = "CXXThisExpr";
      Th.range = NRange;
      Nodes.push_back(std::move(Th));
      return;
    }
    if (NodeTokens.front().Kind == tok::l_paren &&
        NodeTokens.back().Kind == tok::r_paren) {
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                      EnclosingClass);
      }
      return;
    }
  }

  // 18. Id Expression / Unqualified Id
  if (Sym == pseudo::cxx::Symbol::id_expression ||
      Sym == pseudo::cxx::Symbol::unqualified_id) {
    std::string IdName;
    for (const auto &T : NodeTokens) {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier) {
        if (!IdName.empty())
          IdName += "::";
        IdName += getOrigToken(T, Out).text().str();
      }
    }
    if (!IdName.empty()) {
      ASTNode DR;
      DR.role = "expression";
      DR.kind = "DeclRef";
      DR.detail = IdName;
      DR.arcana = "DeclRefExpr '" + IdName + "'";
      DR.range = NRange;
      if (IdName.find("::") != std::string::npos) {
        size_t Colons = IdName.rfind("::");
        std::string Ns = IdName.substr(0, Colons + 2);
        DR.detail = IdName.substr(Colons + 2);
        ASTNode Spec;
        Spec.role = "specifier";
        Spec.kind = "Namespace";
        Spec.detail = Ns;
        Spec.range = NRange;
        DR.children.push_back(std::move(Spec));
      }
      Nodes.push_back(std::move(DR));
      return;
    }
  }

  // 19. Literals
  if (Sym == pseudo::cxx::Symbol::integer_literal ||
      Sym == pseudo::cxx::Symbol::character_literal ||
      Sym == pseudo::cxx::Symbol::floating_point_literal ||
      Sym == pseudo::cxx::Symbol::string_literal ||
      Sym == pseudo::cxx::Symbol::boolean_literal ||
      Sym == pseudo::cxx::Symbol::pointer_literal) {
    size_t SOff = tokenStartOffset(NodeTokens.front(), Out);
    size_t EOff = tokenEndOffset(NodeTokens.back(), Out);
    std::string Text = (SOff < EOff && EOff <= Code.size())
                           ? Code.slice(SOff, EOff).trim().str()
                           : "";
    std::string Kind = "IntegerLiteral";
    if (Sym == pseudo::cxx::Symbol::floating_point_literal)
      Kind = "FloatingLiteral";
    else if (Sym == pseudo::cxx::Symbol::string_literal)
      Kind = "StringLiteral";
    else if (Sym == pseudo::cxx::Symbol::character_literal)
      Kind = "CharacterLiteral";
    else if (Sym == pseudo::cxx::Symbol::boolean_literal)
      Kind = "CXXBoolLiteralExpr";
    else if (Sym == pseudo::cxx::Symbol::pointer_literal) {
      Kind = "CXXNullPtrLiteralExpr";
      Text = "nullptr";
    }
    ASTNode Lit;
    Lit.role = "expression";
    Lit.kind = Kind;
    Lit.detail = Text;
    Lit.arcana = Kind + " " + Text;
    Lit.range = NRange;
    Nodes.push_back(std::move(Lit));
    return;
  }

  // 20. Lambda expression
  if (Sym == pseudo::cxx::Symbol::lambda_expression) {
    ASTNode LE;
    LE.role = "expression";
    LE.kind = "Lambda";
    LE.arcana = "LambdaExpr";
    LE.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, LE.children,
                    /*InsideClass=*/false, /*EnclosingClass=*/"");
    }
    Nodes.push_back(std::move(LE));
    return;
  }

  // 21. Braced init list
  if (Sym == pseudo::cxx::Symbol::braced_init_list) {
    ASTNode IL;
    IL.role = "expression";
    IL.kind = "InitList";
    IL.arcana = "InitListExpr";
    IL.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, IL.children, InsideClass,
                    EnclosingClass);
    }
    Nodes.push_back(std::move(IL));
    return;
  }

  // 22. Conditional expression (? :)
  if (Sym == pseudo::cxx::Symbol::conditional_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    ASTNode CondNode;
    CondNode.role = "expression";
    CondNode.kind = "ConditionalOperator";
    CondNode.arcana = "ConditionalOperator";
    CondNode.range = NRange;
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, CondNode.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(CondNode));
    return;
  }

  // 23. Cast expression
  if (Sym == pseudo::cxx::Symbol::cast_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    ASTNode Cast;
    Cast.role = "expression";
    Cast.kind = "CStyleCast";
    Cast.arcana = "CStyleCastExpr";
    Cast.range = NRange;
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, Cast.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(Cast));
    return;
  }

  // 24. Alias declaration
  if (Sym == pseudo::cxx::Symbol::alias_declaration) {
    std::string AliasName;
    std::string TargetType;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_using && I + 1 < NodeTokens.size()) {
        if (NodeTokens[I + 1].Kind == tok::raw_identifier ||
            NodeTokens[I + 1].Kind == tok::identifier)
          AliasName = getOrigToken(NodeTokens[I + 1], Out).text().str();
      }
      if (NodeTokens[I].Kind == tok::equal && I + 1 < NodeTokens.size()) {
        size_t TStart = tokenStartOffset(NodeTokens[I + 1], Out);
        size_t TEnd = tokenEndOffset(NodeTokens.back(), Out);
        if (NodeTokens.back().Kind == tok::semi && NodeTokens.size() > 1)
          TEnd = tokenEndOffset(NodeTokens[NodeTokens.size() - 2], Out);
        if (TStart < TEnd && TEnd <= Code.size())
          TargetType = Code.slice(TStart, TEnd).trim().str();
      }
    }
    if (!AliasName.empty()) {
      ASTNode AD;
      AD.role = "declaration";
      AD.kind = "TypeAlias";
      AD.detail = AliasName;
      AD.arcana = "TypeAliasDecl " + AliasName;
      AD.range = NRange;
      AD.children.push_back(buildTypeNode(TargetType, NRange));
      Nodes.push_back(std::move(AD));
      return;
    }
  }

  // 25. Using declaration / directive
  if (Sym == pseudo::cxx::Symbol::using_declaration ||
      Sym == pseudo::cxx::Symbol::using_directive) {
    std::string Name;
    for (const auto &T : NodeTokens) {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier) {
        if (!Name.empty())
          Name += "::";
        Name += getOrigToken(T, Out).text().str();
      }
    }
    ASTNode UD;
    UD.role = "declaration";
    UD.kind = (Sym == pseudo::cxx::Symbol::using_directive) ? "UsingDirective"
                                                            : "Using";
    UD.detail = Name;
    UD.arcana = UD.kind + "Decl " + Name;
    UD.range = NRange;
    Nodes.push_back(std::move(UD));
    return;
  }

  // 26. Static assert
  if (Sym == pseudo::cxx::Symbol::static_assert_declaration) {
    ASTNode SA;
    SA.role = "declaration";
    SA.kind = "StaticAssert";
    SA.arcana = "StaticAssertDecl";
    SA.range = NRange;
    Nodes.push_back(std::move(SA));
    return;
  }

  // 26b. Constructor initializer (e.g. : Base(Base), DirtyFiles(Drafts))
  if (Sym == pseudo::cxx::Symbol::mem_initializer) {
    std::string MemberName;
    const pseudo::Token *IdTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_paren ||
          NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        IdTok = &NodeTokens[I];
      }
    }
    if (IdTok)
      MemberName = getOrigToken(*IdTok, Out).text().str();

    ASTNode InitNode;
    InitNode.role = "constructor initializer";
    InitNode.kind = "MemberInitializer";
    InitNode.detail = MemberName;
    InitNode.arcana = "CXXCtorInitializer '" + MemberName + "'";
    InitNode.range = NRange;

    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, InitNode.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(InitNode));
    return;
  }

  // 26c. Condition with variable declaration: if (auto Loc = ...)
  if (Sym == pseudo::cxx::Symbol::condition) {
    const pseudo::Token *EqTok = nullptr;
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal) {
        EqTok = &NodeTokens[I];
        break;
      }
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (EqTok && NameTok) {
      std::string VarName = getOrigToken(*NameTok, Out).text().str();
      size_t NameIdx = NameTok - NodeTokens.data();
      Range TypeRange = nodeRange(StartTok, StartTok + NameIdx, Out, Code);
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      std::string TypeStr = (TStart < TEnd && TEnd <= Code.size())
                                ? Code.slice(TStart, TEnd).trim().str()
                                : "auto";

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = "Var";
      VN.detail = VarName;
      VN.range = NRange;
      VN.arcana = "VarDecl " + VarName + " '" + TypeStr + "'";
      VN.children.push_back(buildTypeNode(TypeStr, TypeRange));

      auto Children = N->elements();
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, VN.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(VN));
      return;
    }
  }

  // 26d. Range-for declaration: for (const auto &Selection : *Selections)
  if (Sym == pseudo::cxx::Symbol::for_range_declaration) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      std::string VarName = getOrigToken(*NameTok, Out).text().str();
      size_t NameIdx = NameTok - NodeTokens.data();
      Range TypeRange = nodeRange(StartTok, StartTok + NameIdx, Out, Code);
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      std::string TypeStr = (TStart < TEnd && TEnd <= Code.size())
                                ? Code.slice(TStart, TEnd).trim().str()
                                : "auto";

      ASTNode DS;
      DS.role = "statement";
      DS.kind = "Decl";
      DS.arcana = "DeclStmt";
      DS.range = NRange;

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = "Var";
      VN.detail = VarName;
      VN.range = NRange;
      VN.arcana = "VarDecl " + VarName + " '" + TypeStr + "'";
      VN.children.push_back(buildTypeNode(TypeStr, TypeRange));

      DS.children.push_back(std::move(VN));
      Nodes.push_back(std::move(DS));
      return;
    }
  }

  // 27. Template declaration
  if (Sym == pseudo::cxx::Symbol::template_declaration) {
    std::vector<std::string> TemplateParams;
    bool InAngles = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::less) {
        InAngles = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::greater)
        break;
      if (InAngles && (NodeTokens[I].Kind == tok::kw_typename ||
                       NodeTokens[I].Kind == tok::kw_class)) {
        if (I + 1 < NodeTokens.size() &&
            (NodeTokens[I + 1].Kind == tok::raw_identifier ||
             NodeTokens[I + 1].Kind == tok::identifier)) {
          TemplateParams.push_back(
              getOrigToken(NodeTokens[I + 1], Out).text().str());
        }
      }
    }

    std::vector<ASTNode> InnerNodes;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, InnerNodes, InsideClass,
                    EnclosingClass);
    }

    if (!InnerNodes.empty()) {
      auto &Inner = InnerNodes.front();
      ASTNode TD;
      TD.role = "declaration";
      TD.kind = (Inner.kind == "Function" || Inner.kind == "CXXMethod")
                    ? "FunctionTemplate"
                    : "ClassTemplate";
      TD.detail = Inner.detail;
      TD.arcana = TD.kind + "Decl " + Inner.detail;
      TD.range = NRange;
      for (const auto &P : TemplateParams) {
        ASTNode TP;
        TP.role = "declaration";
        TP.kind = "TemplateTypeParm";
        TP.detail = P;
        TP.range = NRange;
        TP.arcana = "TemplateTypeParmDecl " + P;
        TD.children.push_back(std::move(TP));
      }
      for (auto &IN : InnerNodes)
        TD.children.push_back(std::move(IN));
      Nodes.push_back(std::move(TD));
      return;
    }
  }

  // Default: recurse on all elements
  auto Children = N->elements();
  for (size_t I = 0; I < Children.size(); ++I) {
    pseudo::Token::Index ChildEnd =
        (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
    buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                  EnclosingClass, DeclaredType, DeclaredTypeRange);
  }
}

static bool posLessEq(Position A, Position B) {
  return std::tie(A.line, A.character) <= std::tie(B.line, B.character);
}

static bool containsRange(Range Outer, Range Inner) {
  return posLessEq(Outer.start, Inner.start) && posLessEq(Inner.end, Outer.end);
}

static int64_t rangeSpan(Range R) {
  int64_t Lines = R.end.line - R.start.line;
  int64_t Chars = R.end.character - R.start.character;
  return Lines * 100000LL + Chars;
}

static const ASTNode *findDeepestContaining(const ASTNode &Node, Range R) {
  if (posLessEq(R.end, R.start))
    std::swap(R.start, R.end);

  if (Node.range && !containsRange(*Node.range, R))
    return nullptr;

  const ASTNode *Tightest = nullptr;
  int64_t SmallestSpan = -1;

  for (const auto &C : Node.children) {
    if (const ASTNode *Descendant = findDeepestContaining(C, R)) {
      int64_t Span = Descendant->range ? rangeSpan(*Descendant->range) : 0;
      if (!Tightest || Span <= SmallestSpan) {
        Tightest = Descendant;
        SmallestSpan = Span;
      }
    }
  }

  if (Tightest)
    return Tightest;

  if (Node.range && containsRange(*Node.range, R))
    return &Node;

  return nullptr;
}


llvm::Expected<std::optional<ASTNode>>
getAST(llvm::StringRef File, llvm::StringRef Code,
       std::optional<Range> R) {
  auto Parsed = parseCode(Code);
  if (!Parsed)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  ASTNode TU;
  TU.role = "declaration";
  TU.kind = "TranslationUnit";
  TU.arcana = "TranslationUnitDecl";
  TU.range = Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  if (Parsed->Root) {
    pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
    buildASTNodes(Parsed->Root, NumTokens, *Parsed, Code, TU.children,
                  /*InsideClass=*/false, /*EnclosingClass=*/"");
  }

  if (!R)
    return std::optional<ASTNode>(std::move(TU));

  const ASTNode *Best = findDeepestContaining(TU, *R);
  if (!Best)
    return std::optional<ASTNode>(std::nullopt);
  return std::optional<ASTNode>(*Best);
}


} // namespace clangd
} // namespace clang
