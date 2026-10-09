//===--- Token.cpp - Tokens and token streams in the pseudoparser ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang-pseudo/Token.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"

namespace clang {
namespace pseudo {

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Token &T) {
  OS << llvm::formatv("{0} {1}:{2} ", clang::tok::getTokenName(T.Kind), T.Line,
                      T.Indent);
  OS << '"';
  llvm::printEscapedString(T.text(), OS);
  OS << '"';
  if (T.Flags)
    OS << llvm::format(" flags=%x", T.Flags);
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const TokenStream &TS) {
  OS << "Index               Kind    Line  Text\n";
  for (const auto &T : TS.tokens()) {
    OS << llvm::format("%5d:  %16s %4d:%-2d  ", TS.index(T),
                       clang::tok::getTokenName(T.Kind), T.Line, T.Indent);
    OS << '"';
    llvm::printEscapedString(T.text(), OS);
    OS << '"';
    if (T.Flags)
      OS << llvm::format("  flags=%x", T.Flags);
    OS << '\n';
  }
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Token::Range &R) {
  OS << llvm::formatv("[{0},{1})", R.Begin, R.End);
  return OS;
}

TokenStream::TokenStream(std::shared_ptr<void> Payload)
    : Payload(std::move(Payload)) {
  Storage.emplace_back();
  Storage.back().Kind = clang::tok::eof;
}

void TokenStream::finalize() {
  assert(!isFinalized());
  unsigned LastLine = Storage.back().Line;
  Storage.emplace_back();
  Storage.back().Kind = tok::eof;
  Storage.back().Line = LastLine + 1;

  Tokens = Storage;
  Tokens = Tokens.drop_front().drop_back();
}

bool TokenStream::isFinalized() const {
  assert(!Storage.empty() && Storage.front().Kind == tok::eof);
  if (Storage.size() == 1)
    return false;
  return Storage.back().Kind == tok::eof;
}

void TokenStream::print(llvm::raw_ostream &OS) const {
  bool FirstToken = true;
  unsigned LastLine = -1;
  StringRef LastText;
  for (const auto &T : tokens()) {
    StringRef Text = T.text();
    if (FirstToken) {
      FirstToken = false;
    } else if (T.Line == LastLine) {
      if (LastText.data() + LastText.size() != Text.data())
        OS << ' ';
    } else {
      OS << '\n';
      OS.indent(T.Indent);
    }
    OS << Text;
    LastLine = T.Line;
    LastText = Text;
  }
  if (!FirstToken)
    OS << '\n';
}

clang::LangOptions genericLangOpts(clang::Language Lang,
                                   clang::LangStandard::Kind Standard) {
  clang::LangOptions Opts;
  std::vector<std::string> UnusedIncludes;
  LangOptions::setLangDefaults(Opts, Lang, llvm::Triple(), UnusedIncludes,
                               Standard);

  // Some options are "on by default", but e.g. at the driver level.
  if (Opts.CPlusPlus)
    Opts.CXXOperatorNames = true;
  if (Opts.CPlusPlus20)
    Opts.Coroutines = true;

  // Some options are off by default, but define keywords we want to tolerate.
  if (Opts.CPlusPlus)
    Opts.MicrosoftExt = true;  // kw__try, kw__finally
  Opts.DeclSpecKeyword = true; // __declspec
  Opts.WChar = true;

  return Opts;
}

TokenStream stripComments(const TokenStream &Input) {
  TokenStream Out(Input.getPayload());
  for (const Token &T : Input.tokens()) {
    if (T.Kind == tok::comment)
      continue;
    Out.push(T);
  }
  Out.finalize();
  return Out;
}

TokenStream stripAttributes(const TokenStream &Input) {
  TokenStream Out(Input.getPayload());
  const auto &Tokens = Input.tokens();
  size_t N = Tokens.size();
  for (size_t I = 0; I < N;) {
    // 1. Standard C++ [[ ... ]]
    if (I + 1 < N && Tokens[I].Kind == tok::l_square &&
        Tokens[I + 1].Kind == tok::l_square) {
      size_t J = I + 2;
      int BracketDepth = 0;
      while (J < N) {
        if (Tokens[J].Kind == tok::l_square || Tokens[J].Kind == tok::l_paren ||
            Tokens[J].Kind == tok::l_brace) {
          ++BracketDepth;
          ++J;
        } else if (BracketDepth > 0 &&
                   (Tokens[J].Kind == tok::r_square ||
                    Tokens[J].Kind == tok::r_paren ||
                    Tokens[J].Kind == tok::r_brace)) {
          --BracketDepth;
          ++J;
        } else if (BracketDepth == 0 && J + 1 < N &&
                   Tokens[J].Kind == tok::r_square &&
                   Tokens[J + 1].Kind == tok::r_square) {
          J += 2;
          break;
        } else {
          ++J;
        }
      }
      I = J;
      continue;
    }

    // 2. GNU __attribute__((...))
    if (Tokens[I].Kind == tok::kw___attribute ||
        (Tokens[I].Kind == tok::raw_identifier &&
         Tokens[I].text() == "__attribute__")) {
      size_t J = I + 1;
      if (J < N && Tokens[J].Kind == tok::l_paren) {
        int ParenDepth = 0;
        while (J < N) {
          if (Tokens[J].Kind == tok::l_paren) {
            ++ParenDepth;
            ++J;
          } else if (Tokens[J].Kind == tok::r_paren) {
            --ParenDepth;
            ++J;
            if (ParenDepth == 0)
              break;
          } else {
            ++J;
          }
        }
        I = J;
        continue;
      }
    }

    // 3. MSVC __declspec(...)
    if (Tokens[I].Kind == tok::kw___declspec ||
        (Tokens[I].Kind == tok::raw_identifier &&
         Tokens[I].text() == "__declspec")) {
      size_t J = I + 1;
      if (J < N && Tokens[J].Kind == tok::l_paren) {
        int ParenDepth = 0;
        while (J < N) {
          if (Tokens[J].Kind == tok::l_paren) {
            ++ParenDepth;
            ++J;
          } else if (Tokens[J].Kind == tok::r_paren) {
            --ParenDepth;
            ++J;
            if (ParenDepth == 0)
              break;
          } else {
            ++J;
          }
        }
        I = J;
        continue;
      }
    }

    // 3b. LLVM attribute macros: LLVM_PREFERRED_TYPE(...), LLVM_ABI, etc.
    if (Tokens[I].Kind == tok::raw_identifier || Tokens[I].Kind == tok::identifier) {
      llvm::StringRef Txt = Tokens[I].text();
      if (Txt == "LLVM_PREFERRED_TYPE" ||
          (Txt.starts_with("LLVM_") && Txt != "LLVM_DEBUG" &&
           (Txt.ends_with("_TYPE") || Txt.contains("ATTRIBUTE") ||
            Txt.contains("NODISCARD") || Txt.contains("READONLY") ||
            Txt.contains("ALIGNAS")))) {
        size_t J = I + 1;
        if (J < N && Tokens[J].Kind == tok::l_paren) {
          int ParenDepth = 0;
          while (J < N) {
            if (Tokens[J].Kind == tok::l_paren) {
              ++ParenDepth;
              ++J;
            } else if (Tokens[J].Kind == tok::r_paren) {
              --ParenDepth;
              ++J;
              if (ParenDepth == 0)
                break;
            } else {
              ++J;
            }
          }
          I = J;
          continue;
        }
      }
    }

    // 4. Standalone macro invocations without semicolon (e.g. DECLARE_SERVICE(Foo), Q_OBJECT)
    if (Tokens[I].Kind == tok::raw_identifier || Tokens[I].Kind == tok::identifier) {
      bool PrecededByDeclBoundary = false;
      if (I == 0) {
        PrecededByDeclBoundary = true;
      } else {
        size_t PrevIdx = I - 1;
        while (PrevIdx > 0 && Tokens[PrevIdx].Kind == tok::comment)
          --PrevIdx;
        auto PK = Tokens[PrevIdx].Kind;
        if (PK == tok::l_brace || PK == tok::semi || PK == tok::colon ||
            PK == tok::kw_public || PK == tok::kw_protected ||
            PK == tok::kw_private)
          PrecededByDeclBoundary = true;
      }

      if (PrecededByDeclBoundary) {
        if (I + 1 < N && Tokens[I + 1].Kind == tok::l_paren) {
          size_t J = I + 1;
          int ParenDepth = 0;
          while (J < N) {
            if (Tokens[J].Kind == tok::l_paren)
              ++ParenDepth;
            else if (Tokens[J].Kind == tok::r_paren) {
              --ParenDepth;
              if (ParenDepth == 0)
                break;
            }
            ++J;
          }
          if (ParenDepth == 0 && J < N) {
            size_t NextIdx = J + 1;
            while (NextIdx < N && Tokens[NextIdx].Kind == tok::comment)
              ++NextIdx;
            if (NextIdx < N) {
              auto NK = Tokens[NextIdx].Kind;
              bool IsFollowedByDeclBoundary =
                  (NK == tok::kw_public || NK == tok::kw_protected ||
                   NK == tok::kw_private || NK == tok::r_brace ||
                   NK == tok::kw_void || NK == tok::kw_int || NK == tok::kw_bool ||
                   NK == tok::kw_char || NK == tok::kw_unsigned ||
                   NK == tok::kw_signed || NK == tok::kw_long ||
                   NK == tok::kw_short || NK == tok::kw_float ||
                   NK == tok::kw_double || NK == tok::kw_auto ||
                   NK == tok::kw_const || NK == tok::kw_volatile ||
                   NK == tok::kw_constexpr || NK == tok::kw_class ||
                   NK == tok::kw_struct || NK == tok::kw_virtual ||
                   NK == tok::kw_static);
              if (IsFollowedByDeclBoundary) {
                I = NextIdx;
                continue;
              }
            }
          }
        } else {
          size_t NextIdx = I + 1;
          while (NextIdx < N && Tokens[NextIdx].Kind == tok::comment)
            ++NextIdx;
          if (NextIdx < N) {
            auto NK = Tokens[NextIdx].Kind;
            if (NK == tok::kw_public || NK == tok::kw_protected ||
                NK == tok::kw_private || NK == tok::r_brace) {
              I = NextIdx;
              continue;
            }
          }
        }
      }
    }

    Out.push(Tokens[I]);
    ++I;
  }
  Out.finalize();
  return Out;
}

} // namespace pseudo
} // namespace clang
