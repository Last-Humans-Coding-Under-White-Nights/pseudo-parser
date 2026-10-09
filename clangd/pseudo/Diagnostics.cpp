//===--- Diagnostics.cpp - Pseudo-parser diagnostics generation ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pseudo/Diagnostics.h"
#include "SourceCode.h"
#include "clang-pseudo/GLR.h"
#include "clang-pseudo/Token.h"
#include "clang-pseudo/cxx/CXX.h"
#include "llvm/ADT/DenseSet.h"
#include <algorithm>
#include <set>
#include <tuple>

namespace clang {
namespace clangd {

namespace {

constexpr int DiagnosticSeverityError = 1;

void collectOpaqueNodes(const pseudo::ForestNode *N,
                        const pseudo::Disambiguation &Disambig,
                        std::vector<const pseudo::ForestNode *> &Opaques,
                        llvm::DenseSet<const pseudo::ForestNode *> &Visited) {
  if (!N || !Visited.insert(N).second)
    return;
  if (N->kind() == pseudo::ForestNode::Opaque) {
    Opaques.push_back(N);
    return;
  }
  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (!Alts.empty()) {
      auto It = Disambig.find(N);
      unsigned AltIdx =
          (It != Disambig.end() && It->second < Alts.size()) ? It->second : 0;
      collectOpaqueNodes(Alts[AltIdx], Disambig, Opaques, Visited);
    }
    return;
  }
  for (const auto *Child : N->children())
    collectOpaqueNodes(Child, Disambig, Opaques, Visited);
}

void checkBracketDiagnostics(const pseudo::TokenStream &Stream,
                             const ParseOutput &Parsed,
                             llvm::StringRef Code,
                             llvm::function_ref<void(Diagnostic)> AddDiag) {
  for (const auto &Tok : Stream.tokens()) {
    if (Tok.Kind == tok::l_brace && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "missing-brace";
      D.message = "unclosed '{', expected closing '}'";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::r_brace && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "unmatched-brace";
      D.message = "unmatched '}', extraneous closing brace";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::l_paren && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "missing-paren";
      D.message = "unclosed '(', expected closing ')'";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::r_paren && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "unmatched-paren";
      D.message = "unmatched ')', extraneous closing parenthesis";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::l_square && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "missing-bracket";
      D.message = "unclosed '[', expected closing ']'";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::r_square && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "unmatched-bracket";
      D.message = "unmatched ']', extraneous closing bracket";
      AddDiag(std::move(D));
    }
  }
}

void checkPreprocessorDiagnostics(
    const pseudo::DirectiveTree &Tree, const ParseOutput &Parsed,
    llvm::StringRef Code, llvm::function_ref<void(Diagnostic)> AddDiag,
    bool TopLevel = true) {
  auto DirectiveRange = [&](const pseudo::DirectiveTree::Directive &Dir) -> Range {
    if (Dir.Tokens.size() == 0 ||
        Dir.Tokens.Begin >= Parsed.RawStream.tokens().size()) {
      Position Pos = offsetToPosition(Code, Code.size());
      return Range{Pos, Pos};
    }
    size_t StartTokIdx = Dir.Tokens.Begin;
    size_t EndTokIdx = Dir.Tokens.End > Dir.Tokens.Begin ? Dir.Tokens.End - 1
                                                        : Dir.Tokens.Begin;
    if (EndTokIdx >= Parsed.RawStream.tokens().size())
      EndTokIdx = Parsed.RawStream.tokens().size() - 1;
    size_t StartOff =
        tokenStartOffset(Parsed.RawStream.tokens()[StartTokIdx], Parsed);
    size_t EndOff =
        tokenEndOffset(Parsed.RawStream.tokens()[EndTokIdx], Parsed);
    return Range{offsetToPosition(Code, StartOff),
                 offsetToPosition(Code, EndOff)};
  };

  for (const auto &Chunk : Tree.Chunks) {
    if (const auto *Dir = std::get_if<pseudo::DirectiveTree::Directive>(&Chunk)) {
      if (TopLevel) {
        if (Dir->Kind == tok::pp_endif) {
          Diagnostic D;
          D.range = DirectiveRange(*Dir);
          D.severity = DiagnosticSeverityError;
          D.source = "pseudo-parser";
          D.code = "unmatched-endif";
          D.message = "'#endif' without '#if'";
          AddDiag(std::move(D));
        } else if (Dir->Kind == tok::pp_else) {
          Diagnostic D;
          D.range = DirectiveRange(*Dir);
          D.severity = DiagnosticSeverityError;
          D.source = "pseudo-parser";
          D.code = "unmatched-else";
          D.message = "'#else' without '#if'";
          AddDiag(std::move(D));
        } else if (Dir->Kind == tok::pp_elif ||
                   Dir->Kind == tok::pp_elifdef ||
                   Dir->Kind == tok::pp_elifndef) {
          Diagnostic D;
          D.range = DirectiveRange(*Dir);
          D.severity = DiagnosticSeverityError;
          D.source = "pseudo-parser";
          D.code = "unmatched-elif";
          D.message = "'#elif' without '#if'";
          AddDiag(std::move(D));
        }
      }
    } else if (const auto *Cond =
                   std::get_if<pseudo::DirectiveTree::Conditional>(&Chunk)) {
      // Check if conditional is unterminated (missing #endif)
      if (Cond->End.Kind != tok::pp_endif || Cond->End.Tokens.size() == 0) {
        Diagnostic D;
        if (!Cond->Branches.empty()) {
          const auto &OpeningDir = Cond->Branches.front().first;
          D.range = DirectiveRange(OpeningDir);
          llvm::StringRef DirName = "#if";
          switch (OpeningDir.Kind) {
          case tok::pp_ifdef:
            DirName = "#ifdef";
            break;
          case tok::pp_ifndef:
            DirName = "#ifndef";
            break;
          case tok::pp_if:
            DirName = "#if";
            break;
          default:
            break;
          }
          D.message =
              ("unterminated '" + DirName + "'; expected matching '#endif'").str();
        } else {
          Position Pos = offsetToPosition(Code, Code.size());
          D.range = Range{Pos, Pos};
          D.message =
              "unterminated conditional directive; expected matching '#endif'";
        }
        D.severity = DiagnosticSeverityError;
        D.source = "pseudo-parser";
        D.code = "missing-endif";
        AddDiag(std::move(D));
      }

      // Check for multiple #else or #elif after #else
      bool SeenElse = false;
      for (const auto &Branch : Cond->Branches) {
        if (SeenElse) {
          Diagnostic D;
          D.range = DirectiveRange(Branch.first);
          D.severity = DiagnosticSeverityError;
          D.source = "pseudo-parser";
          D.code = "unexpected-directive";
          D.message = Branch.first.Kind == tok::pp_else
                          ? "'#else' after '#else'"
                          : "'#elif' after '#else'";
          AddDiag(std::move(D));
        }
        if (Branch.first.Kind == tok::pp_else)
          SeenElse = true;

        // Recursively check nested directive tree within the branch
        checkPreprocessorDiagnostics(Branch.second, Parsed, Code, AddDiag,
                                     /*TopLevel=*/false);
      }
    }
  }
}

void collectConditionals(
    pseudo::DirectiveTree &Tree,
    std::vector<pseudo::DirectiveTree::Conditional *> &Conds) {
  for (auto &Chunk : Tree.Chunks) {
    if (auto *C = std::get_if<pseudo::DirectiveTree::Conditional>(&Chunk)) {
      Conds.push_back(C);
      for (auto &Branch : C->Branches)
        collectConditionals(Branch.second, Conds);
    }
  }
}

void collectBranchTokens(const pseudo::DirectiveTree &Tree,
                         const pseudo::TokenStream &RawStream,
                         std::vector<pseudo::Token> &Tokens) {
  for (const auto &Chunk : Tree.Chunks) {
    if (const auto *CodeChunk =
            std::get_if<pseudo::DirectiveTree::Code>(&Chunk)) {
      if (CodeChunk->Tokens.Begin <= RawStream.tokens().size() &&
          CodeChunk->Tokens.End <= RawStream.tokens().size() &&
          CodeChunk->Tokens.Begin <= CodeChunk->Tokens.End) {
        for (const auto &Tok : RawStream.tokens(CodeChunk->Tokens))
          Tokens.push_back(Tok);
      }
    } else if (const auto *CondChunk =
                   std::get_if<pseudo::DirectiveTree::Conditional>(&Chunk)) {
      for (const auto &Branch : CondChunk->Branches)
        collectBranchTokens(Branch.second, RawStream, Tokens);
    }
  }
}

struct BranchBrackets {
  int UnclosedBraces = 0;
  int UnclosedParens = 0;
  int UnclosedSquares = 0;
  std::optional<pseudo::Token> LastCodeToken;
};

BranchBrackets analyzeBranchBrackets(const pseudo::DirectiveTree &BranchTree,
                                     const pseudo::TokenStream &RawStream) {
  BranchBrackets BB;
  std::vector<tok::TokenKind> Stack;
  std::vector<pseudo::Token> Tokens;
  collectBranchTokens(BranchTree, RawStream, Tokens);

  for (const auto &Tok : Tokens) {
    if (Tok.Kind == tok::comment)
      continue;
    BB.LastCodeToken = Tok;
    if (Tok.Kind == tok::l_brace || Tok.Kind == tok::l_paren ||
        Tok.Kind == tok::l_square) {
      Stack.push_back(Tok.Kind);
    } else if (Tok.Kind == tok::r_brace) {
      if (!Stack.empty() && Stack.back() == tok::l_brace)
        Stack.pop_back();
    } else if (Tok.Kind == tok::r_paren) {
      if (!Stack.empty() && Stack.back() == tok::l_paren)
        Stack.pop_back();
    } else if (Tok.Kind == tok::r_square) {
      if (!Stack.empty() && Stack.back() == tok::l_square)
        Stack.pop_back();
    }
  }

  for (auto K : Stack) {
    if (K == tok::l_brace)
      ++BB.UnclosedBraces;
    else if (K == tok::l_paren)
      ++BB.UnclosedParens;
    else if (K == tok::l_square)
      ++BB.UnclosedSquares;
  }
  return BB;
}

} // namespace

std::vector<Diagnostic> getDiagnostics(const ParseOutput &Parsed,
                                       llvm::StringRef Code) {
  std::vector<Diagnostic> Diags;
  std::set<std::tuple<int, int, std::string>> Seen;

  auto AddDiag = [&](Diagnostic D) {
    auto Key = std::make_tuple(D.range.start.line, D.range.start.character,
                               D.code);
    if (Seen.insert(Key).second)
      Diags.push_back(std::move(D));
  };

  // 1. Preprocessor condition checks (missing #endif, unmatched #endif/#else/#elif, etc.)
  checkPreprocessorDiagnostics(Parsed.Directives, Parsed, Code, AddDiag);

  // 2. Bracket / brace mismatches on primary ParseableStream
  checkBracketDiagnostics(Parsed.ParseableStream, Parsed, Code, AddDiag);

  // If the file is empty or only whitespace/comments, no syntax diagnostics
  bool HasCodeTokens = false;
  for (const auto &Tok : Parsed.ParseableStream.tokens()) {
    if (Tok.Kind != tok::eof && Tok.Kind != tok::comment) {
      HasCodeTokens = true;
      break;
    }
  }
  if (!HasCodeTokens)
    return Diags;

  // 2. Grammar syntax errors: opaque recovery nodes in the primary forest
  if (!Parsed.Root || Parsed.Root->kind() == pseudo::ForestNode::Opaque) {
    if (Diags.empty()) {
      Diagnostic D;
      pseudo::Token::Index StartIdx =
          Parsed.Root ? Parsed.Root->startTokenIndex() : 0;
      if (StartIdx < Parsed.ParseableStream.tokens().size()) {
        const auto &Tok = Parsed.ParseableStream.tokens()[StartIdx];
        if (Tok.Kind != tok::eof) {
          D.range = tokenRange(Tok, Parsed, Code);
          D.message =
              "syntax error: unexpected token '" + Tok.text().str() + "'";
        } else {
          D.range = Range{offsetToPosition(Code, Code.size()),
                          offsetToPosition(Code, Code.size())};
          D.message = "syntax error: unexpected end of file";
        }
      } else {
        D.range = Range{offsetToPosition(Code, Code.size()),
                        offsetToPosition(Code, Code.size())};
        D.message = "syntax error: unexpected token";
      }
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "syntax-error";
      AddDiag(std::move(D));
    }
    return Diags;
  }

  std::vector<const pseudo::ForestNode *> Opaques;
  llvm::DenseSet<const pseudo::ForestNode *> Visited;
  collectOpaqueNodes(Parsed.Root, Parsed.Disambig, Opaques, Visited);

  const auto &Lang = pseudo::cxx::getLanguage();
  for (const auto *Node : Opaques) {
    auto StartIdx = Node->startTokenIndex();
    if (StartIdx < Parsed.ParseableStream.tokens().size()) {
      const auto &Tok = Parsed.ParseableStream.tokens()[StartIdx];
      if (Tok.Kind == tok::eof)
        continue;
      std::string SymName = Lang.G.symbolName(Node->symbol()).str();
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "grammar-error";
      D.message =
          "syntax error: code does not fit grammar (expected " + SymName + ")";
      AddDiag(std::move(D));
    }
  }

  // 3. Multi-branch verification: evaluate alternative preprocessor branches
  pseudo::DirectiveTree TreeCopy = Parsed.Directives;
  std::vector<pseudo::DirectiveTree::Conditional *> Conds;
  collectConditionals(TreeCopy, Conds);

  clang::LangOptions LangOpts = pseudo::genericLangOpts(
      clang::Language::CXX, clang::LangStandard::lang_cxx20);
  std::optional<pseudo::SymbolID> StartSym =
      Lang.G.findNonterminal("translation-unit");

  size_t MaxAlts = 16;
  size_t AltsEvaluated = 0;

  for (size_t CondIdx = 0; CondIdx < Conds.size(); ++CondIdx) {
    auto *Cond = Conds[CondIdx];
    if (Cond->Branches.size() <= 1)
      continue;

    std::vector<BranchBrackets> BranchStats;
    for (const auto &Branch : Cond->Branches)
      BranchStats.push_back(
          analyzeBranchBrackets(Branch.second, Parsed.RawStream));

    unsigned PrimaryTaken = Cond->Taken.value_or(0);
    int PrimaryUnclosed = (PrimaryTaken < BranchStats.size())
                              ? BranchStats[PrimaryTaken].UnclosedBraces
                              : 0;

    for (unsigned B = 0; B < Cond->Branches.size(); ++B) {
      if (B == PrimaryTaken)
        continue;
      if (++AltsEvaluated > MaxAlts)
        break;

      // Check if branch B is missing an opening brace when an outer closing brace exists
      if (Cond->End.Kind == tok::pp_endif && Cond->End.Tokens.size() > 0 &&
          PrimaryUnclosed > BranchStats[B].UnclosedBraces) {
        bool HasOuterClosingBrace = false;
        size_t EndIdx = Cond->End.Tokens.End;
        if (EndIdx <= Parsed.RawStream.tokens().size()) {
          for (size_t Idx = EndIdx; Idx < Parsed.RawStream.tokens().size();
               ++Idx) {
            const auto &Tok = Parsed.RawStream.tokens()[Idx];
            if (Tok.Kind == tok::comment)
              continue;
            if (Tok.Kind == tok::r_brace) {
              HasOuterClosingBrace = true;
              break;
            }
            if (Tok.Kind == tok::l_brace)
              break;
          }
        }

        if (HasOuterClosingBrace && BranchStats[B].LastCodeToken) {
          Diagnostic D;
          D.range = tokenRange(*BranchStats[B].LastCodeToken, Parsed, Code);
          D.severity = DiagnosticSeverityError;
          D.source = "pseudo-parser";
          D.code = "missing-brace";
          std::string TokName = BranchStats[B].LastCodeToken->text().str();
          D.message = "missing '{' after '" + TokName +
                      "' in conditional branch to match closing '}'";
          AddDiag(std::move(D));
        }
      }

      // Synthesize and check the alternative stream
      pseudo::DirectiveTree AltTree = Parsed.Directives;
      std::vector<pseudo::DirectiveTree::Conditional *> AltConds;
      collectConditionals(AltTree, AltConds);
      if (CondIdx < AltConds.size() &&
          B < AltConds[CondIdx]->Branches.size()) {
        AltConds[CondIdx]->Taken = B;

        auto AltStripped = AltTree.stripDirectives(Parsed.RawStream);
        auto AltParseable = pseudo::stripAttributes(
            pseudo::stripComments(pseudo::cook(AltStripped, LangOpts)));

        bool AltHasCodeTokens = false;
        for (const auto &Tok : AltParseable.tokens()) {
          if (Tok.Kind != tok::eof && Tok.Kind != tok::comment) {
            AltHasCodeTokens = true;
            break;
          }
        }
        if (!AltHasCodeTokens)
          continue;

        pseudo::pairBrackets(AltParseable);
        checkBracketDiagnostics(AltParseable, Parsed, Code, AddDiag);

        if (StartSym) {
          pseudo::ForestArena AltArena;
          pseudo::GSS AltGSS;
          const auto *AltRoot = &pseudo::glrParse(
              pseudo::ParseParams{AltParseable, AltArena, AltGSS},
              *StartSym, Lang);
          if (AltRoot) {
            std::vector<const pseudo::ForestNode *> AltOpaques;
            llvm::DenseSet<const pseudo::ForestNode *> AltVisited;
            pseudo::Disambiguation EmptyDisambig;
            collectOpaqueNodes(AltRoot, EmptyDisambig, AltOpaques, AltVisited);
            for (const auto *Node : AltOpaques) {
              auto StartIdx = Node->startTokenIndex();
              if (StartIdx < AltParseable.tokens().size()) {
                const auto &Tok = AltParseable.tokens()[StartIdx];
                if (Tok.Kind == tok::eof)
                  continue;
                std::string SymName = Lang.G.symbolName(Node->symbol()).str();
                Diagnostic D;
                D.range = tokenRange(Tok, Parsed, Code);
                D.severity = DiagnosticSeverityError;
                D.source = "pseudo-parser";
                D.code = "grammar-error";
                D.message =
                    "syntax error in conditional branch: code does not fit "
                    "grammar (expected " +
                    SymName + ")";
                AddDiag(std::move(D));
              }
            }
          }
        }
      }
    }
  }

  llvm::sort(Diags, [](const Diagnostic &A, const Diagnostic &B) {
    if (A.range.start.line != B.range.start.line)
      return A.range.start.line < B.range.start.line;
    return A.range.start.character < B.range.start.character;
  });

  return Diags;
}

} // namespace clangd
} // namespace clang
