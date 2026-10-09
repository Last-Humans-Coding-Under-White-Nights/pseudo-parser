//===-- PseudoModuleTests.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Annotations.h"
#include "ClangdLSPServer.h"
#include "ClangdServer.h"
#include "DumpAST.h"
#include "FeatureModule.h"
#include "GlobalCompilationDatabase.h"
#include "LSPClient.h"
#include "Protocol.h"
#include "PseudoModule.h"
#include "SyncAPI.h"
#include "TestFS.h"
#include "TestTU.h"
#include "support/Logger.h"
#include "support/Threading.h"
#include "llvm/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <memory>
#include <vector>

namespace clang {
namespace clangd {
namespace {

static std::string dumpAST(ClangdServer &Server, PathRef File) {
  std::string Result;
  Notification Done;
  Server.customAction(File, "DumpAST", [&](llvm::Expected<InputsAndAST> AST) {
    if (AST) {
      llvm::raw_string_ostream ResultOS(Result);
      AST->AST.getASTContext().getTranslationUnitDecl()->dump(ResultOS, true);
    } else {
      llvm::consumeError(AST.takeError());
      Result = "<no-ast>";
    }
    Done.notify();
  });
  Done.wait();
  return Result;
}

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

MATCHER_P(withName, N, "") { return arg.name == N; }
MATCHER_P(withKind, Kind, "") { return arg.kind == Kind; }

template <class... ChildMatchers>
::testing::Matcher<DocumentSymbol> children(ChildMatchers... ChildrenM) {
  return Field(&DocumentSymbol::children, UnorderedElementsAre(ChildrenM...));
}

TEST(PseudoModuleTest, RegistryRegistration) {
  FeatureModuleSet Set = FeatureModuleSet::fromRegistry();
  auto *Mod = Set.get<PseudoModule>();
  EXPECT_NE(Mod, nullptr);
}

TEST(PseudoModuleTest, BasicDocumentSymbols) {
  PseudoModule Mod;
  llvm::StringRef Code = R"cpp(
    namespace my_ns {
      class MyClass {
        void myMethod() {}
        int myField;
      };
      struct MyStruct {
        int x;
      };
      void globalFunc() {}
      enum class Color {
        Red,
        Green,
        Blue
      };
      using IntAlias = int;
    }
  )cpp";

  auto Symbols = Mod.getDocumentSymbols(Code);
  ASSERT_TRUE(bool(Symbols)) << llvm::toString(Symbols.takeError());

  EXPECT_THAT(
      *Symbols,
      ElementsAre(AllOf(
          withName("my_ns"), withKind(SymbolKind::Namespace),
          children(
              AllOf(withName("MyClass"), withKind(SymbolKind::Class),
                    children(AllOf(withName("myMethod"),
                                   withKind(SymbolKind::Method)),
                             AllOf(withName("myField"),
                                   withKind(SymbolKind::Field)))),
              AllOf(withName("MyStruct"), withKind(SymbolKind::Struct),
                    children(
                        AllOf(withName("x"), withKind(SymbolKind::Field)))),
              AllOf(withName("globalFunc"), withKind(SymbolKind::Function)),
              AllOf(withName("Color"), withKind(SymbolKind::Enum),
                    children(
                        AllOf(withName("Red"), withKind(SymbolKind::EnumMember)),
                        AllOf(withName("Green"),
                              withKind(SymbolKind::EnumMember)),
                        AllOf(withName("Blue"),
                              withKind(SymbolKind::EnumMember)))),
              AllOf(withName("IntAlias"), withKind(SymbolKind::Class))))));
}

TEST(PseudoModuleTest, BrokenCodeRecovery) {
  PseudoModule Mod;
  // Code with broken function body or missing tokens that GLR recovers from
  llvm::StringRef Code = R"cpp(
    namespace broken_ns {
      void goodFunc() {}
      void brokenFunc() {
        invalid syntax ;;; %%%
      }
      class Survives {
        void method() {}
      };
    }
  )cpp";

  auto Symbols = Mod.getDocumentSymbols(Code);
  ASSERT_TRUE(bool(Symbols)) << llvm::toString(Symbols.takeError());

  EXPECT_THAT(
      *Symbols,
      ElementsAre(AllOf(
          withName("broken_ns"), withKind(SymbolKind::Namespace),
          children(
              AllOf(withName("goodFunc"), withKind(SymbolKind::Function)),
              AllOf(withName("brokenFunc"), withKind(SymbolKind::Function)),
              AllOf(withName("Survives"), withKind(SymbolKind::Class),
                    children(AllOf(withName("method"),
                                   withKind(SymbolKind::Method))))))));
}

TEST(PseudoModuleTest, SemanticRanges) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    namespace test {
      void func() {
        int $point^x = 42;
      }
    }
  )cpp");

  auto Ranges = Mod.getSemanticRanges(Code.code(), {Code.point("point")});
  ASSERT_TRUE(bool(Ranges)) << llvm::toString(Ranges.takeError());
  ASSERT_EQ(Ranges->size(), 1u);

  // Innermost selection range should cover the variable or expression
  const auto &Innermost = (*Ranges)[0];
  EXPECT_TRUE(Innermost.range.start.line <= Code.point("point").line);
  EXPECT_TRUE(Innermost.range.end.line >= Code.point("point").line);
  EXPECT_NE(Innermost.parent, nullptr);
}

TEST(PseudoModuleTest, FoldingRanges) {
  PseudoModule Mod;
  llvm::StringRef Code = R"cpp(
    namespace fold_ns {
      void myFunc() {
        int a = 1;
        int b = 2;
      }
    }
  )cpp";

  auto Foldings = Mod.getFoldingRanges(Code, /*LineFoldingOnly=*/false);
  ASSERT_TRUE(bool(Foldings)) << llvm::toString(Foldings.takeError());
  EXPECT_FALSE(Foldings->empty());
  for (const auto &FR : *Foldings) {
    EXPECT_LT(FR.startLine, FR.endLine);
  }
}

TEST(PseudoModuleTest, LocateSymbolAtLocal) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void testFunc() {
      int $decl[[myVar]] = 10;
      $usage^myVar = 20;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "myVar");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.uri.file(), File);
}

TEST(PseudoModuleTest, LocateSymbolAtParameter) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void testFunc(int $decl[[param]]) {
      int x = $usage^param + 1;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "param");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, LocateSymbolAtMember) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    class MyClass {
      int $decl[[memberVal]];
      void foo() {
        $usage^memberVal = 42;
      }
    };
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "memberVal");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, LocateSymbolAtOutOfLineMethod) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    class MyClass {
      int $decl[[memberVal]];
      void method();
    };
    void MyClass::method() {
      $usage^memberVal = 100;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "memberVal");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, FindReferencesLocal) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void funcOne() {
      int $ref1[[var]] = 1;
      $ref2[[var]] = 2;
    }
    void funcTwo() {
      int var = 3;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Refs = Mod.findReferences(File, Code.code(), Code.range("ref2").start);
  ASSERT_TRUE(bool(Refs)) << llvm::toString(Refs.takeError());
  EXPECT_EQ(Refs->References.size(), 2u);
  std::vector<Range> Ranges;
  for (const auto &R : Refs->References)
    Ranges.push_back(R.Loc.range);
  EXPECT_THAT(Ranges, UnorderedElementsAre(Code.range("ref1"), Code.range("ref2")));
  EXPECT_TRUE(Refs->References[0].Attributes & ReferencesResult::Declaration);
}

TEST(PseudoModuleTest, FindReferencesFunctionAndLimit) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void $call1[[foo]]() {}
    void caller() {
      $call2[[foo]]();
      $call3[[foo]]();
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Refs = Mod.findReferences(File, Code.code(), Code.range("call1").start);
  ASSERT_TRUE(bool(Refs)) << llvm::toString(Refs.takeError());
  EXPECT_EQ(Refs->References.size(), 3u);

  // Test limit
  auto Limited = Mod.findReferences(File, Code.code(), Code.range("call1").start, /*Limit=*/2);
  ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
  EXPECT_EQ(Limited->References.size(), 2u);
  EXPECT_TRUE(Limited->HasMore);
}

TEST(PseudoModuleTest, ClangdServerFallbackOnInvalidCommand) {
  // End-to-end test verifying ClangdServer falls back to PseudoModule
  // when compile command is invalid (AST cannot be built).
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();
  // Use -### so clang driver prints command without running cc1, causing AST build to fail.
  CDB->ExtraClangFlags = {"-###"};

  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  Modules.add(std::move(Pseudo));

  ClangdServer::Options Opts = ClangdServer::optsForTest();
  Opts.FeatureModules = &Modules;

  ClangdServer Server(*CDB, FS, Opts);

  Annotations Source(R"cpp(
    namespace fallback_ns {
      class FallbackClass {
        void $decl[[work]]() {}
      };
      void topLevel() {
        $call^work();
      }
    }
  )cpp");

  std::string FilePath = testPath("broken_command.cpp");
  FS.Files[FilePath] = Source.code().str();
  Server.addDocument(FilePath, Source.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // Verify AST indeed failed to build
  EXPECT_EQ(dumpAST(Server, FilePath), "<no-ast>");

  // Test documentSymbols fallback
  bool Done = false;
  std::optional<llvm::Expected<llvm::json::Value>> SymbolsResult;
  DocumentSymbolParams SymParams;
  SymParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PseudoPtr->onDocumentSymbol(
      SymParams,
      [&](llvm::Expected<llvm::json::Value> Symbols) {
        SymbolsResult = std::move(Symbols);
        Done = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(Done);
  ASSERT_TRUE(SymbolsResult.has_value());
  ASSERT_TRUE(bool(*SymbolsResult))
      << "Fallback should succeed: "
      << llvm::toString(SymbolsResult->takeError());

  // Test semantic ranges fallback
  bool RangesDone = false;
  std::optional<llvm::Expected<std::vector<SelectionRange>>> RangesResult;
  SelectionRangeParams SelParams;
  SelParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  SelParams.positions = {Position{2, 12}};
  PseudoPtr->onSelectionRange(
      SelParams,
      [&](llvm::Expected<std::vector<SelectionRange>> Ranges) {
        RangesResult = std::move(Ranges);
        RangesDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(RangesDone);
  ASSERT_TRUE(RangesResult.has_value());
  ASSERT_TRUE(bool(*RangesResult))
      << "Semantic ranges fallback should succeed: "
      << llvm::toString(RangesResult->takeError());
  EXPECT_FALSE((*RangesResult)->empty());

  // Test locateSymbolAt fallback
  TextDocumentPositionParams PosParams;
  PosParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PosParams.position = Source.point("call");
  bool LocDone = false;
  std::optional<llvm::Expected<std::vector<Location>>> LocResult;
  PseudoPtr->onGoToDefinition(
      PosParams,
      [&](llvm::Expected<std::vector<Location>> Locs) {
        LocResult = std::move(Locs);
        LocDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(LocDone);
  ASSERT_TRUE(LocResult.has_value());
  ASSERT_TRUE(bool(*LocResult)) << llvm::toString(LocResult->takeError());
  ASSERT_EQ((*LocResult)->size(), 1u);
  EXPECT_EQ((*LocResult)->front().range, Source.range("decl"));

  // Test findDocumentHighlights fallback
  bool HighDone = false;
  std::optional<llvm::Expected<std::vector<DocumentHighlight>>> HighResult;
  PseudoPtr->onDocumentHighlight(
      PosParams,
      [&](llvm::Expected<std::vector<DocumentHighlight>> Highs) {
        HighResult = std::move(Highs);
        HighDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(HighDone);
  ASSERT_TRUE(HighResult.has_value());
  ASSERT_TRUE(bool(*HighResult)) << llvm::toString(HighResult->takeError());
  EXPECT_EQ((*HighResult)->size(), 2u);

  // Test findReferences fallback
  bool RefsDone = false;
  std::optional<llvm::Expected<std::vector<ReferenceLocation>>> RefsResult;
  ReferenceParams RefParams;
  RefParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  RefParams.position = Source.point("call");
  RefParams.context.includeDeclaration = true;
  PseudoPtr->onReference(
      RefParams,
      [&](llvm::Expected<std::vector<ReferenceLocation>> Refs) {
        RefsResult = std::move(Refs);
        RefsDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(RefsDone);
  ASSERT_TRUE(RefsResult.has_value());
  ASSERT_TRUE(bool(*RefsResult)) << llvm::toString(RefsResult->takeError());
  EXPECT_EQ((*RefsResult)->size(), 2u);

  // When pseudo-parser is disabled, it should use the original implementation and NOT fall back
  PseudoPtr->setEnabled(false);
  std::optional<llvm::Expected<std::vector<Location>>> DisabledLoc;
  PseudoPtr->onGoToDefinition(
      PosParams,
      [&](llvm::Expected<std::vector<Location>> Locs) {
        DisabledLoc = std::move(Locs);
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(DisabledLoc.has_value());
  EXPECT_FALSE(bool(*DisabledLoc));
  llvm::consumeError(DisabledLoc->takeError());
}

TEST(PseudoModuleTest, LocateSymbolAtTemplateTypeParameter) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    template <typename [[T]]>
    void foo() {
      ^T var;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "T");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range());
}

TEST(PseudoModuleTest, LocateSymbolAtTypedMemberAccess) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    struct ClassA {
      int $declA[[val]];
    };
    struct ClassB {
      int val;
    };
    void test() {
      ClassA a;
      a.^val = 1;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "val");
  // Must specifically point to ClassA's val, not ClassB's
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("declA"));
}

TEST(PseudoModuleTest, LocateSymbolAtUsingDeclaration) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    namespace my_ns {
      void $decl[[targetFunc]]();
    }
    using my_ns::^targetFunc;
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "targetFunc");
}

TEST(PseudoModuleTest, LocateSymbolAtNamespaceAlias) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    namespace LongNamespaceName {
      int x;
    }
    namespace $decl[[ShortName]] = LongNamespaceName;
    void test() {
      ^ShortName::x = 42;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "ShortName");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, UsePseudoParserOnlyOption) {
  // End-to-end test verifying setPseudoOnly disables Clang parser completely
  // and routes open-file navigation requests directly to PseudoModule.
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  PseudoPtr->setPseudoOnly(true);
  Modules.add(std::move(Pseudo));

  ClangdServer::Options Opts = ClangdServer::optsForTest();
  Opts.FeatureModules = &Modules;

  ClangdServer Server(*CDB, FS, Opts);

  Annotations Source(R"cpp(
    namespace test_only {
      class Worker {
        void $decl[[doJob]]() {}
      };
      void run() {
        Worker w;
        w.^doJob();
      }
    }
  )cpp");

  std::string FilePath = testPath("pseudo_only.cpp");
  FS.Files[FilePath] = Source.code().str();
  Server.addDocument(FilePath, Source.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // In UsePseudoParserOnly mode, no Clang AST is built at all!
  EXPECT_EQ(dumpAST(Server, FilePath), "<no-ast>");

  // locateSymbolAt should work via PseudoModule
  TextDocumentPositionParams PosParams;
  PosParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PosParams.position = Source.point();
  bool LocDone = false;
  std::optional<llvm::Expected<std::vector<Location>>> LocResult;
  PseudoPtr->onGoToDefinition(
      PosParams,
      [&](llvm::Expected<std::vector<Location>> Locs) {
        LocResult = std::move(Locs);
        LocDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(LocDone);
  ASSERT_TRUE(LocResult.has_value());
  ASSERT_TRUE(bool(*LocResult)) << llvm::toString(LocResult->takeError());
  ASSERT_EQ((*LocResult)->size(), 1u);
  EXPECT_EQ((*LocResult)->front().range, Source.range("decl"));

  // findDocumentHighlights should work via PseudoModule
  bool HighDone = false;
  std::optional<llvm::Expected<std::vector<DocumentHighlight>>> HighResult;
  PseudoPtr->onDocumentHighlight(
      PosParams,
      [&](llvm::Expected<std::vector<DocumentHighlight>> Highs) {
        HighResult = std::move(Highs);
        HighDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(HighDone);
  ASSERT_TRUE(HighResult.has_value());
  ASSERT_TRUE(bool(*HighResult)) << llvm::toString(HighResult->takeError());
  EXPECT_EQ((*HighResult)->size(), 2u);

  // findReferences should work via PseudoModule
  bool RefsDone = false;
  std::optional<llvm::Expected<std::vector<ReferenceLocation>>> RefsResult;
  ReferenceParams RefParams;
  RefParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  RefParams.position = Source.point();
  RefParams.context.includeDeclaration = true;
  PseudoPtr->onReference(
      RefParams,
      [&](llvm::Expected<std::vector<ReferenceLocation>> Refs) {
        RefsResult = std::move(Refs);
        RefsDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(RefsDone);
  ASSERT_TRUE(RefsResult.has_value());
  ASSERT_TRUE(bool(*RefsResult)) << llvm::toString(RefsResult->takeError());
  EXPECT_EQ((*RefsResult)->size(), 2u);

  // documentSymbols should work via PseudoModule
  bool Done = false;
  std::optional<llvm::Expected<llvm::json::Value>> SymbolsResult;
  DocumentSymbolParams SymParams;
  SymParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PseudoPtr->onDocumentSymbol(
      SymParams,
      [&](llvm::Expected<llvm::json::Value> Symbols) {
        SymbolsResult = std::move(Symbols);
        Done = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(Done);
  ASSERT_TRUE(SymbolsResult.has_value());
  ASSERT_TRUE(bool(*SymbolsResult));

  // hover should work via PseudoModule
  bool HoverDone = false;
  std::optional<llvm::Expected<std::optional<Hover>>> HoverResult;
  PseudoPtr->onHover(
      PosParams,
      [&](llvm::Expected<std::optional<Hover>> H) {
        HoverResult = std::move(H);
        HoverDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(HoverDone);
  ASSERT_TRUE(HoverResult.has_value());
  ASSERT_TRUE(bool(*HoverResult)) << llvm::toString(HoverResult->takeError());
  ASSERT_TRUE((*HoverResult)->has_value());
  EXPECT_NE((**HoverResult)->contents.value.find("doJob"), std::string::npos);
}

TEST(PseudoModuleTest, EndToEndLSPOverriding) {
  MockFS FS;
  ClangdLSPServer::Options Opts;
  ClangdServer::Options &Base = Opts;
  Base = ClangdServer::optsForTest();
  FeatureModuleSet FeatureModules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  PseudoPtr->setPseudoOnly(true);
  FeatureModules.add(std::move(Pseudo));
  Base.FeatureModules = &FeatureModules;

  LSPClient Client;
  std::optional<ClangdLSPServer> Server;
  std::optional<std::thread> ServerThread;

  Server.emplace(Client.transport(), FS, std::move(Opts));
  ServerThread.emplace([&] { EXPECT_TRUE(Server->run()); });
  Client.call("initialize", llvm::json::Object{});

  Annotations Code(R"cpp(
    int $decl[[my_var]] = 10;
    int foo() {
      return ^my_var + 1;
    }
  )cpp");

  Client.didOpen("test.cpp", Code.code());

  auto &Def = Client.call("textDocument/definition",
                          llvm::json::Object{
                              {"textDocument", Client.documentID("test.cpp")},
                              {"position", Code.point()},
                          });
  auto DefValue = Def.takeValue();
  ASSERT_TRUE(DefValue.getAsArray() != nullptr);
  EXPECT_EQ(DefValue.getAsArray()->size(), 1u);
  auto Loc = (*DefValue.getAsArray())[0].getAsObject();
  auto RangeVal = Loc->get("range");
  ASSERT_NE(RangeVal, nullptr);
  Range R;
  llvm::json::Path::Root Root;
  ASSERT_TRUE(fromJSON(*RangeVal, R, Root));
  EXPECT_EQ(R, Code.range("decl"));

  Client.call("shutdown", nullptr);
  Client.notify("exit", nullptr);
  Client.stop();
  ServerThread->join();
  Server.reset();
  ServerThread.reset();
}

TEST(PseudoModuleTest, ClangdServerAdjustParseInputsNavigation) {
  PseudoModule Mod;
  std::string Path = testPath("ClangdServer.cpp");
  Annotations Code(R"cpp(
    void adjustParseInputs(bool SkipPreambleBuild, const char *File) {
      auto $def[[HasRequiredModules]] = [File]() {
        return hasRequiredModules($file_use^File);
      };
      if (SkipPreambleBuild || $call^HasRequiredModules())
        return;
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(Path, Code.code(), Code.point("call"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  EXPECT_EQ(Loc->size(), 1u) << "Loc size is " << Loc->size();
  if (!Loc->empty()) {
    EXPECT_EQ((*Loc)[0].Name, "HasRequiredModules");
    EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("def"));
  }

  auto LocFile = Mod.locateSymbolAt(Path, Code.code(), Code.point("file_use"));
  ASSERT_TRUE(bool(LocFile)) << llvm::toString(LocFile.takeError());
  EXPECT_EQ(LocFile->size(), 1u) << "LocFile size is " << LocFile->size();
  if (!LocFile->empty()) {
    EXPECT_EQ((*LocFile)[0].Name, "File");
  }
}

TEST(PseudoModuleTest, ExtractIncludes) {
  llvm::StringRef Code = R"cpp(
#include "my_header.h"
#  include <vector>
#include_next "next_header.h"
// #include "commented.h"
int main() {}
#include "footer.h"
)cpp";

  auto Incs = PseudoModule::extractIncludes(Code);
  ASSERT_EQ(Incs.size(), 4u);
  EXPECT_EQ(Incs[0].Written, "my_header.h");
  EXPECT_FALSE(Incs[0].IsAngled);
  EXPECT_EQ(Incs[0].HashLine, 1);

  EXPECT_EQ(Incs[1].Written, "vector");
  EXPECT_TRUE(Incs[1].IsAngled);
  EXPECT_EQ(Incs[1].HashLine, 2);

  EXPECT_EQ(Incs[2].Written, "next_header.h");
  EXPECT_FALSE(Incs[2].IsAngled);
  EXPECT_EQ(Incs[2].HashLine, 3);

  EXPECT_EQ(Incs[3].Written, "footer.h");
  EXPECT_FALSE(Incs[3].IsAngled);
  EXPECT_EQ(Incs[3].HashLine, 6);
}

TEST(PseudoModuleTest, GoToDefinitionOnIncludeDirective) {
  MockFS FS;
  FS.Files[testPath("foo.h")] = "struct Foo {};\n";
  FS.Files[testPath("sys/bar.h")] = "struct Bar {};\n";

  MockCompilationDatabase CDB(testRoot());
  CDB.ExtraClangFlags = {"-I" + testPath("sys")};

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(&CDB);

  Annotations Main(R"cpp(
#include ^"foo.h"
#include <^bar.h>
int main() {}
)cpp");

  auto LocFoo =
      Mod.locateSymbolAt(testPath("main.cpp"), Main.code(), Main.points()[0]);
  ASSERT_TRUE(bool(LocFoo)) << llvm::toString(LocFoo.takeError());
  ASSERT_EQ(LocFoo->size(), 1u);
  EXPECT_EQ((*LocFoo)[0].Name, "foo.h");
  EXPECT_EQ((*LocFoo)[0].PreferredDeclaration.uri.file(), testPath("foo.h"));

  auto LocBar =
      Mod.locateSymbolAt(testPath("main.cpp"), Main.code(), Main.points()[1]);
  ASSERT_TRUE(bool(LocBar)) << llvm::toString(LocBar.takeError());
  ASSERT_EQ(LocBar->size(), 1u);
  EXPECT_EQ((*LocBar)[0].Name, "bar.h");
  EXPECT_EQ((*LocBar)[0].PreferredDeclaration.uri.file(), testPath("sys/bar.h"));
}

TEST(PseudoModuleTest, GoToDefinitionAcrossHeadersDirectAndTransitive) {
  MockFS FS;
  FS.Files[testPath("inc2/header_b.h")] = R"cpp(
    struct TransitiveWidget {
      int value;
    };
  )cpp";

  FS.Files[testPath("inc1/header_a.h")] = R"cpp(
    #include "header_b.h"

    struct DirectStruct {
      int x;
    };

    class DirectClass {
    public:
      void doSomething();
    };

    enum class DirectEnum {
      First,
      Second
    };

    using DirectAlias = DirectStruct;
  )cpp";

  MockCompilationDatabase CDB(testRoot());
  CDB.ExtraClangFlags = {"-I" + testPath("inc1"), "-I" + testPath("inc2")};

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(&CDB);

  Annotations Main(R"cpp(
    #include "header_a.h"

    void test() {
      ^DirectStruct S;
      ^DirectClass C;
      ^DirectEnum E;
      ^DirectAlias A;
      ^TransitiveWidget W;
    }
  )cpp");

  auto CheckSym = [&](Position Pos, llvm::StringRef ExpectedName,
                      PathRef ExpectedFile) {
    auto Loc = Mod.locateSymbolAt(testPath("main.cpp"), Main.code(), Pos);
    ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
    ASSERT_EQ(Loc->size(), 1u) << "Expected symbol " << ExpectedName.str();
    EXPECT_EQ((*Loc)[0].Name, ExpectedName);
    EXPECT_EQ((*Loc)[0].PreferredDeclaration.uri.file(), ExpectedFile);
  };

  auto P = Main.points();
  CheckSym(P[0], "DirectStruct", testPath("inc1/header_a.h"));
  CheckSym(P[1], "DirectClass", testPath("inc1/header_a.h"));
  CheckSym(P[2], "DirectEnum", testPath("inc1/header_a.h"));
  CheckSym(P[3], "DirectAlias", testPath("inc1/header_a.h"));
  CheckSym(P[4], "TransitiveWidget", testPath("inc2/header_b.h"));
}

TEST(PseudoModuleTest, ClangdServerParseInputsAndPathRefFromHeaders) {
  MockFS FS;
  FS.Files[testPath("include/Compiler.h")] = R"cpp(
    struct ParseInputs {
      int Version;
    };
  )cpp";
  FS.Files[testPath("include/support/Path.h")] = R"cpp(
    using PathRef = const char *;
  )cpp";
  FS.Files[testPath("include/TUScheduler.h")] = R"cpp(
    #include "Compiler.h"
  )cpp";
  FS.Files[testPath("include/ClangdServer.h")] = R"cpp(
    #include "support/Path.h"
    #include "TUScheduler.h"

    class ClangdServer {
    public:
      ClangdServer();
      void adjustParseInputs(ParseInputs &Inputs, PathRef File) const;
    };
  )cpp";
  FS.Files[testPath("sys_include/vector")] = R"cpp(
    namespace std { template <typename T> class vector {}; }
  )cpp";

  MockCompilationDatabase CDB(testRoot());
  CDB.ExtraClangFlags = {"-I" + testPath("include"), "-isystem" + testPath("sys_include")};

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(&CDB);

  std::string Path = testPath("ClangdServer.cpp");
  Annotations Code(R"cpp(
    #include "ClangdServer.h"
    #include <vector>

    ClangdServer::ClangdServer() {}

    void ClangdServer::adjustParseInputs(ParseInputs &Inputs, PathRef File) const {}
  )cpp");

  // 1. Jump on #include "ClangdServer.h"
  size_t IncPos = Code.code().find("#include \"ClangdServer.h\"");
  ASSERT_NE(IncPos, llvm::StringRef::npos);
  Position IncPosPoint = offsetToPosition(Code.code(), IncPos + 12);
  auto LocInc = Mod.locateSymbolAt(Path, Code.code(), IncPosPoint);
  ASSERT_TRUE(bool(LocInc)) << llvm::toString(LocInc.takeError());
  ASSERT_EQ(LocInc->size(), 1u);
  EXPECT_EQ((*LocInc)[0].Name, "ClangdServer.h");
  EXPECT_TRUE(llvm::StringRef((*LocInc)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.h"));

  // 2. Jump to ParseInputs (declared in Compiler.h, included via TUScheduler.h)
  size_t ParseInputsPos =
      Code.code().find("void ClangdServer::adjustParseInputs(ParseInputs &Inputs, PathRef File) const");
  ASSERT_NE(ParseInputsPos, llvm::StringRef::npos);
  size_t ParseInputsOffset =
      ParseInputsPos + std::string("void ClangdServer::adjustParseInputs(").size() + 2;
  Position ParseInputsPosition = offsetToPosition(Code.code(), ParseInputsOffset);

  auto LocParseInputs = Mod.locateSymbolAt(Path, Code.code(), ParseInputsPosition);
  ASSERT_TRUE(bool(LocParseInputs)) << llvm::toString(LocParseInputs.takeError());
  ASSERT_EQ(LocParseInputs->size(), 1u);
  EXPECT_EQ((*LocParseInputs)[0].Name, "ParseInputs");
  EXPECT_TRUE(llvm::StringRef((*LocParseInputs)[0].PreferredDeclaration.uri.file())
                  .ends_with("Compiler.h"))
      << "Actual file: "
      << (*LocParseInputs)[0].PreferredDeclaration.uri.file().str();

  // 3. Jump to PathRef (defined in support/Path.h, included via ClangdServer.h)
  size_t PathRefOffset =
      ParseInputsPos +
      std::string("void ClangdServer::adjustParseInputs(ParseInputs &Inputs, ").size() + 2;
  Position PathRefPosition = offsetToPosition(Code.code(), PathRefOffset);

  auto LocPathRef = Mod.locateSymbolAt(Path, Code.code(), PathRefPosition);
  ASSERT_TRUE(bool(LocPathRef)) << llvm::toString(LocPathRef.takeError());
  ASSERT_EQ(LocPathRef->size(), 1u);
  EXPECT_EQ((*LocPathRef)[0].Name, "PathRef");
  EXPECT_TRUE(llvm::StringRef((*LocPathRef)[0].PreferredDeclaration.uri.file())
                  .ends_with("Path.h"))
      << "Actual file: "
      << (*LocPathRef)[0].PreferredDeclaration.uri.file().str();

  // 4. Jump to ClangdServer class qualifier from adjustParseInputs
  // Should jump to class ClangdServer in ClangdServer.h, NOT to constructor in ClangdServer.cpp!
  size_t ClassQualifierPos = ParseInputsPos + std::string("void ").size() + 2;
  Position ClassQualifierPoint = offsetToPosition(Code.code(), ClassQualifierPos);
  auto LocClass = Mod.locateSymbolAt(Path, Code.code(), ClassQualifierPoint);
  ASSERT_TRUE(bool(LocClass)) << llvm::toString(LocClass.takeError());
  ASSERT_EQ(LocClass->size(), 1u);
  EXPECT_EQ((*LocClass)[0].Name, "ClangdServer");
  EXPECT_TRUE(llvm::StringRef((*LocClass)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.h"))
      << "Actual file: "
      << (*LocClass)[0].PreferredDeclaration.uri.file().str();

  // 5. In ClangdServer::ClangdServer:
  // First ClangdServer is class (jumps to ClangdServer.h)
  // Second ClangdServer is constructor definition (jumps to ClangdServer.cpp)
  size_t CtorPos = Code.code().find("ClangdServer::ClangdServer(");
  ASSERT_NE(CtorPos, llvm::StringRef::npos);
  Position CtorClassPoint = offsetToPosition(Code.code(), CtorPos + 2);
  auto LocCtorClass = Mod.locateSymbolAt(Path, Code.code(), CtorClassPoint);
  ASSERT_TRUE(bool(LocCtorClass)) << llvm::toString(LocCtorClass.takeError());
  ASSERT_EQ(LocCtorClass->size(), 1u);
  EXPECT_EQ((*LocCtorClass)[0].Name, "ClangdServer");
  EXPECT_TRUE(llvm::StringRef((*LocCtorClass)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.h"))
      << "Actual file: "
      << (*LocCtorClass)[0].PreferredDeclaration.uri.file().str();

  Position CtorFuncPoint =
      offsetToPosition(Code.code(), CtorPos + std::string("ClangdServer::").size() + 2);
  auto LocCtorFunc = Mod.locateSymbolAt(Path, Code.code(), CtorFuncPoint);
  ASSERT_TRUE(bool(LocCtorFunc)) << llvm::toString(LocCtorFunc.takeError());
  ASSERT_EQ(LocCtorFunc->size(), 1u);
  EXPECT_EQ((*LocCtorFunc)[0].Name, "ClangdServer");
  EXPECT_TRUE(llvm::StringRef((*LocCtorFunc)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.cpp"))
      << "Actual file: "
      << (*LocCtorFunc)[0].PreferredDeclaration.uri.file().str();

  // 6. Jump on #include <vector>
  size_t VectorPos = Code.code().find("#include <vector>");
  ASSERT_NE(VectorPos, llvm::StringRef::npos);
  Position VectorPoint = offsetToPosition(Code.code(), VectorPos + 12);
  auto LocVector = Mod.locateSymbolAt(Path, Code.code(), VectorPoint);
  ASSERT_TRUE(bool(LocVector)) << llvm::toString(LocVector.takeError());
  ASSERT_EQ(LocVector->size(), 1u);
  EXPECT_EQ((*LocVector)[0].Name, "vector");
  EXPECT_TRUE(llvm::StringRef((*LocVector)[0].PreferredDeclaration.uri.file())
                  .ends_with("vector"));
}

TEST(PseudoModuleTest, DisambiguateTypeAndValueWithSameName) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    struct $structDecl[[Foo]] {
      int x;
    };

    void test() {
      ^Foo $varDecl[[Foo]];
      ^Foo.x = 1;
    }
  )cpp");

  std::string File = testPath("test.cpp");
  auto Points = Code.points();
  ASSERT_EQ(Points.size(), 2u);

  // 1. First Foo is in type position -> should locate struct Foo
  auto LocType = Mod.locateSymbolAt(File, Code.code(), Points[0]);
  ASSERT_TRUE(bool(LocType)) << llvm::toString(LocType.takeError());
  ASSERT_EQ(LocType->size(), 1u);
  EXPECT_EQ((*LocType)[0].Name, "Foo");
  EXPECT_EQ((*LocType)[0].PreferredDeclaration.range, Code.range("structDecl"));

  // 2. Second Foo is in value/member-access position -> should locate variable Foo
  auto LocVal = Mod.locateSymbolAt(File, Code.code(), Points[1]);
  ASSERT_TRUE(bool(LocVal)) << llvm::toString(LocVal.takeError());
  ASSERT_EQ(LocVal->size(), 1u);
  EXPECT_EQ((*LocVal)[0].Name, "Foo");
  EXPECT_EQ((*LocVal)[0].PreferredDeclaration.range, Code.range("varDecl"));
}

TEST(PseudoModuleTest, SlideExample2) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    #define USE_V2 1
    #if USE_V2
    class $serviceDecl[[DataService]] {
    #else
    class LegacyService {
    #endif
    public:
      void $syncDecl[[sync]]();
    };
    void run() {
      ^DataService svc;
      svc.^sync();
    }
  )cpp");
  auto Points = Code.points();
  auto LocSvc = Mod.locateSymbolAt(File, Code.code(), Points[0]);
  ASSERT_TRUE(bool(LocSvc)) << "Ex2 svc error: " << llvm::toString(LocSvc.takeError());
  ASSERT_EQ(LocSvc->size(), 1u) << "Ex2 svc size";
  EXPECT_EQ((*LocSvc)[0].Name, "DataService");
  EXPECT_EQ((*LocSvc)[0].PreferredDeclaration.range, Code.range("serviceDecl"));

  auto LocSync = Mod.locateSymbolAt(File, Code.code(), Points[1]);
  ASSERT_TRUE(bool(LocSync)) << "Ex2 sync error: " << llvm::toString(LocSync.takeError());
  ASSERT_EQ(LocSync->size(), 1u) << "Ex2 sync size";
  EXPECT_EQ((*LocSync)[0].Name, "sync");
  EXPECT_EQ((*LocSync)[0].PreferredDeclaration.range, Code.range("syncDecl"));
}

TEST(PseudoModuleTest, SlideExample3) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    class $serverDecl[[Server]] {
    public:
      Server(int port);
      void start();
    };
    void ^Server::start() {}
    Server::Server(int port) {}
  )cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << "Ex3 error: " << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u) << "Ex3 size";
  EXPECT_EQ((*Loc)[0].Name, "Server");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("serverDecl"));
}

TEST(PseudoModuleTest, SlideExample4) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    #define DECLARE_SERVICE(Name) \
    public: static const char* id() { return #Name; } private:

    class AuthManager {
      DECLARE_SERVICE(AuthManager)
    public:
      void $authDecl[[authenticate]]();
    };

    void handleLogin(AuthManager* mgr) {
      mgr->^authenticate();
    }
  )cpp");
  auto Syms = Mod.getDocumentSymbols(Code.code());
  ASSERT_TRUE(bool(Syms)) << llvm::toString(Syms.takeError());
  ASSERT_EQ(Syms->size(), 2u);
  EXPECT_EQ((*Syms)[0].name, "AuthManager");
  ASSERT_EQ((*Syms)[0].children.size(), 1u);
  EXPECT_EQ((*Syms)[0].children[0].name, "authenticate");

  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << "Ex4 error: " << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u) << "Ex4 size";
  EXPECT_EQ((*Loc)[0].Name, "authenticate");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("authDecl"));
}

TEST(PseudoModuleTest, DataServiceOutOfClassDefinition) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    #define USE_V2 1
    #if USE_V2
    class $serviceDecl[[DataService]] {
    #else
    class LegacyService {
    #endif
    public:
      void $syncDecl[[sync]]();
    };

    void DataService::$syncDef[[sync]]() {
      int work = 42;
    }

    void run() {
      $svcDecl[[DataService]] svc;
      svc.$callSync[[sync]]();
    }
  )cpp");

  // 1. locateSymbolAt on svc.sync() call
  Position CallSyncPos = Code.range("callSync").start;
  auto LocCallSync = Mod.locateSymbolAt(File, Code.code(), CallSyncPos);
  ASSERT_TRUE(bool(LocCallSync)) << llvm::toString(LocCallSync.takeError());
  ASSERT_EQ(LocCallSync->size(), 1u);
  EXPECT_EQ((*LocCallSync)[0].Name, "sync");
  EXPECT_EQ((*LocCallSync)[0].PreferredDeclaration.range, Code.range("syncDecl"));
  ASSERT_TRUE((*LocCallSync)[0].Definition.has_value());
  EXPECT_EQ((*LocCallSync)[0].Definition->range, Code.range("syncDef"));

  // 2. locateSymbolAt on in-class declaration void sync();
  Position DeclPos = Code.range("syncDecl").start;
  auto LocDecl = Mod.locateSymbolAt(File, Code.code(), DeclPos);
  ASSERT_TRUE(bool(LocDecl)) << llvm::toString(LocDecl.takeError());
  ASSERT_EQ(LocDecl->size(), 1u);
  EXPECT_EQ((*LocDecl)[0].Name, "sync");
  EXPECT_EQ((*LocDecl)[0].PreferredDeclaration.range, Code.range("syncDecl"));
  ASSERT_TRUE((*LocDecl)[0].Definition.has_value());
  EXPECT_EQ((*LocDecl)[0].Definition->range, Code.range("syncDef"));

  // 3. locateSymbolAt on out-of-class definition void DataService::sync()
  Position DefPos = Code.range("syncDef").start;
  auto LocDef = Mod.locateSymbolAt(File, Code.code(), DefPos);
  ASSERT_TRUE(bool(LocDef)) << llvm::toString(LocDef.takeError());
  ASSERT_EQ(LocDef->size(), 1u);
  EXPECT_EQ((*LocDef)[0].Name, "sync");
  EXPECT_EQ((*LocDef)[0].PreferredDeclaration.range, Code.range("syncDecl"));
  ASSERT_TRUE((*LocDef)[0].Definition.has_value());
  EXPECT_EQ((*LocDef)[0].Definition->range, Code.range("syncDef"));

  // 4. Test Go-To-Definition and Go-To-Declaration via ClangdServer / LSP
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();
  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  PseudoPtr->setPseudoOnly(true);
  Modules.add(std::move(Pseudo));

  ClangdServer::Options SvrOpts = ClangdServer::optsForTest();
  SvrOpts.FeatureModules = &Modules;
  ClangdServer Server(*CDB, FS, SvrOpts);

  FS.Files[File] = Code.code().str();
  Server.addDocument(File, Code.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // (a) Go-to-Definition on svc.sync() -> should jump to out-of-class definition (syncDef)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = CallSyncPos;
    std::optional<llvm::Expected<std::vector<Location>>> DefResult;
    PseudoPtr->onGoToDefinition(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          DefResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(DefResult.has_value());
    ASSERT_TRUE(bool(*DefResult)) << llvm::toString(DefResult->takeError());
    ASSERT_EQ((*DefResult)->size(), 1u);
    EXPECT_EQ((*DefResult)->front().range, Code.range("syncDef"));
  }

  // (b) Go-to-Declaration on svc.sync() -> should jump to in-class declaration (syncDecl)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = CallSyncPos;
    std::optional<llvm::Expected<std::vector<Location>>> DeclResult;
    PseudoPtr->onGoToDeclaration(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          DeclResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(DeclResult.has_value());
    ASSERT_TRUE(bool(*DeclResult)) << llvm::toString(DeclResult->takeError());
    ASSERT_EQ((*DeclResult)->size(), 1u);
    EXPECT_EQ((*DeclResult)->front().range, Code.range("syncDecl"));
  }

  // (c) Go-to-Definition while on out-of-class definition (syncDef) -> should toggle to declaration (syncDecl)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = DefPos;
    std::optional<llvm::Expected<std::vector<Location>>> ToggleResult;
    PseudoPtr->onGoToDefinition(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          ToggleResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(ToggleResult.has_value());
    ASSERT_TRUE(bool(*ToggleResult)) << llvm::toString(ToggleResult->takeError());
    ASSERT_EQ((*ToggleResult)->size(), 1u);
    EXPECT_EQ((*ToggleResult)->front().range, Code.range("syncDecl"));
  }

  // (d) Go-to-Definition while on in-class declaration (syncDecl) -> should toggle to definition (syncDef)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = DeclPos;
    std::optional<llvm::Expected<std::vector<Location>>> ToggleResult;
    PseudoPtr->onGoToDefinition(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          ToggleResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(ToggleResult.has_value());
    ASSERT_TRUE(bool(*ToggleResult)) << llvm::toString(ToggleResult->takeError());
    ASSERT_EQ((*ToggleResult)->size(), 1u);
    EXPECT_EQ((*ToggleResult)->front().range, Code.range("syncDef"));
  }
}

TEST(PseudoModuleTest, TemplateWithRequiresClause) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    template <typename T>
    requires (sizeof(T) > 4)
    class $decl[[DataBuffer]] {
    public:
      void $writeDecl[[write]](T data);
    };

    void run() {
      $typeUsage[[DataBuffer]]<double> buf;
      buf.$callWrite[[write]](3.14);
    }
  )cpp");

  auto Syms = Mod.getDocumentSymbols(Code.code());
  ASSERT_TRUE(bool(Syms)) << llvm::toString(Syms.takeError());
  ASSERT_GE(Syms->size(), 2u);
  EXPECT_EQ((*Syms)[0].name, "DataBuffer");
  ASSERT_GE((*Syms)[0].children.size(), 1u);
  EXPECT_EQ((*Syms)[0].children[0].name, "write");

  // (a) Go-to-Definition on DataBuffer
  auto Loc1 = Mod.locateSymbolAt(File, Code.code(), Code.range("typeUsage").start);
  ASSERT_TRUE(bool(Loc1)) << llvm::toString(Loc1.takeError());
  ASSERT_EQ(Loc1->size(), 1u);
  EXPECT_EQ((*Loc1)[0].Name, "DataBuffer");
  EXPECT_EQ((*Loc1)[0].PreferredDeclaration.range, Code.range("decl"));

  // (b) Go-to-Definition on write()
  auto Loc2 = Mod.locateSymbolAt(File, Code.code(), Code.range("callWrite").start);
  ASSERT_TRUE(bool(Loc2)) << llvm::toString(Loc2.takeError());
  ASSERT_EQ(Loc2->size(), 1u);
  EXPECT_EQ((*Loc2)[0].Name, "write");
  EXPECT_EQ((*Loc2)[0].PreferredDeclaration.range, Code.range("writeDecl"));
}

TEST(PseudoModuleTest, ConceptAndConstrainedTemplate) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    template <typename T>
    concept $conceptDecl[[Serializable]] = requires(T x) {
      x.serialize();
    };

    template <$conceptUsage[[Serializable]] T>
    class $pipelineDecl[[DataPipeline]] {
    public:
      void $processDecl[[process]](T data);
    };

    void run() {
      $pipelineUsage[[DataPipeline]]<int> pipeline;
      pipeline.$callProcess[[process]](42);
    }
  )cpp");

  auto Syms = Mod.getDocumentSymbols(Code.code());
  ASSERT_TRUE(bool(Syms)) << llvm::toString(Syms.takeError());
  bool FoundPipeline = false;
  for (const auto &S : *Syms) {
    if (S.name == "DataPipeline")
      FoundPipeline = true;
  }
  EXPECT_TRUE(FoundPipeline);

  // (a) Go-to-Definition on DataPipeline
  auto Loc1 = Mod.locateSymbolAt(File, Code.code(), Code.range("pipelineUsage").start);
  ASSERT_TRUE(bool(Loc1)) << llvm::toString(Loc1.takeError());
  ASSERT_EQ(Loc1->size(), 1u);
  EXPECT_EQ((*Loc1)[0].Name, "DataPipeline");
  EXPECT_EQ((*Loc1)[0].PreferredDeclaration.range, Code.range("pipelineDecl"));

  // (b) Go-to-Definition on concept Serializable
  auto Loc2 = Mod.locateSymbolAt(File, Code.code(), Code.range("conceptUsage").start);
  ASSERT_TRUE(bool(Loc2)) << llvm::toString(Loc2.takeError());
  ASSERT_EQ(Loc2->size(), 1u);
  EXPECT_EQ((*Loc2)[0].Name, "Serializable");
  EXPECT_EQ((*Loc2)[0].PreferredDeclaration.range, Code.range("conceptDecl"));
}

TEST(PseudoModuleTest, HoverBasic) {
  PseudoModule Mod;
  std::string File = testPath("hover_test.cpp");
  Annotations Code(R"cpp(
    #include <vector>

    class DataService {
    public:
      int cacheSize;
      void sync(int timeout);
    };

    void run() {
      $svcUsage[[DataService]] svc;
      svc.$cacheUsage[[cacheSize]] = 100;
      svc.$syncUsage[[sync]](5);
    }
  )cpp");

  // (a) Hover on DataService
  auto H1 = Mod.getHover(File, Code.code(), Code.range("svcUsage").start);
  ASSERT_TRUE(bool(H1)) << llvm::toString(H1.takeError());
  ASSERT_TRUE(H1->has_value());
  EXPECT_EQ((*H1)->range, Code.range("svcUsage"));
  EXPECT_THAT((*H1)->contents.value, testing::HasSubstr("class DataService"));

  // (b) Hover on cacheSize
  auto H2 = Mod.getHover(File, Code.code(), Code.range("cacheUsage").start);
  ASSERT_TRUE(bool(H2)) << llvm::toString(H2.takeError());
  ASSERT_TRUE(H2->has_value());
  EXPECT_EQ((*H2)->range, Code.range("cacheUsage"));
  EXPECT_THAT((*H2)->contents.value, testing::HasSubstr("cacheSize"));

  // (c) Hover on sync()
  auto H3 = Mod.getHover(File, Code.code(), Code.range("syncUsage").start);
  ASSERT_TRUE(bool(H3)) << llvm::toString(H3.takeError());
  ASSERT_TRUE(H3->has_value());
  EXPECT_EQ((*H3)->range, Code.range("syncUsage"));
  EXPECT_THAT((*H3)->contents.value, testing::HasSubstr("sync(int timeout)"));

  // (d) Hover on #include line
  auto H4 = Mod.getHover(File, Code.code(), Position{1, 5});
  ASSERT_TRUE(bool(H4)) << llvm::toString(H4.takeError());
  ASSERT_TRUE(H4->has_value());
  EXPECT_THAT((*H4)->contents.value, testing::HasSubstr("#include <vector>"));
}

TEST(PseudoModuleTest, SemanticHighlightingBasic) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    class DataService {
    public:
      int count;
      void sync(int timeout);
    };
    void DataService::sync(int timeout) {
      int localVal = timeout + count;
    }
  )cpp");

  auto Tokens = Mod.getSemanticHighlightings(Code.code());
  ASSERT_TRUE(bool(Tokens)) << llvm::toString(Tokens.takeError());
  ASSERT_FALSE(Tokens->empty());

  bool FoundClass = false;
  bool FoundMethod = false;
  bool FoundField = false;
  bool FoundLocal = false;
  bool FoundParam = false;
  bool FoundPrimitive = false;

  for (const auto &Tok : *Tokens) {
    if (Tok.Kind == HighlightingKind::Class)
      FoundClass = true;
    if (Tok.Kind == HighlightingKind::Method)
      FoundMethod = true;
    if (Tok.Kind == HighlightingKind::Field)
      FoundField = true;
    if (Tok.Kind == HighlightingKind::LocalVariable)
      FoundLocal = true;
    if (Tok.Kind == HighlightingKind::Parameter)
      FoundParam = true;
    if (Tok.Kind == HighlightingKind::Primitive)
      FoundPrimitive = true;
  }

  EXPECT_TRUE(FoundClass);
  EXPECT_TRUE(FoundMethod);
  EXPECT_TRUE(FoundField);
  EXPECT_TRUE(FoundLocal);
  EXPECT_TRUE(FoundParam);
  EXPECT_TRUE(FoundPrimitive);

  // Conversion to SemanticTokens protocol format
  auto SemTokens = Mod.getSemanticTokens(Code.code());
  ASSERT_TRUE(bool(SemTokens)) << llvm::toString(SemTokens.takeError());
  EXPECT_FALSE(SemTokens->tokens.empty());

  // Test delta diffing
  auto Edits = diffTokens(SemTokens->tokens, SemTokens->tokens);
  EXPECT_TRUE(Edits.empty());
}

TEST(PseudoModuleTest, CodeCompletionDirectivesAndMembers) {
  PseudoModule Mod;
  std::string File = testPath("completion_test.cpp");
  Annotations Code(R"cpp(
    class DataService {
    public:
      int cacheSize;
      void sync();
      void start();
    };

    void run() {
      DataService svc;
      svc.^
    }
  )cpp");

  // Member completion on svc.
  auto Comps = Mod.getCompletions(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Comps)) << llvm::toString(Comps.takeError());
  ASSERT_FALSE(Comps->items.empty());

  bool FoundSync = false;
  bool FoundStart = false;
  bool FoundCacheSize = false;
  for (const auto &Item : Comps->items) {
    if (Item.label == "sync") {
      FoundSync = true;
      EXPECT_EQ(Item.kind, CompletionItemKind::Method);
    }
    if (Item.label == "start") {
      FoundStart = true;
      EXPECT_EQ(Item.kind, CompletionItemKind::Method);
    }
    if (Item.label == "cacheSize") {
      FoundCacheSize = true;
      EXPECT_EQ(Item.kind, CompletionItemKind::Field);
    }
  }
  EXPECT_TRUE(FoundSync);
  EXPECT_TRUE(FoundStart);
  EXPECT_TRUE(FoundCacheSize);

  // Preprocessor directive completion
  Annotations DirectiveCode(R"cpp(
    #inc^
  )cpp");
  auto DirComps = Mod.getCompletions(File, DirectiveCode.code(), DirectiveCode.point());
  ASSERT_TRUE(bool(DirComps)) << llvm::toString(DirComps.takeError());
  bool FoundInclude = false;
  for (const auto &Item : DirComps->items) {
    if (Item.label == "include")
      FoundInclude = true;
  }
  EXPECT_TRUE(FoundInclude);
}

TEST(PseudoModuleTest, CodeCompletionScopeAndKeywords) {
  PseudoModule Mod;
  std::string File = testPath("completion_scope_test.cpp");

  // Scope resolution DataService::
  Annotations ScopeCode(R"cpp(
    class DataService {
    public:
      static void reset();
      void sync();
    };
    void test() {
      DataService::^
    }
  )cpp");
  auto ScopeComps = Mod.getCompletions(File, ScopeCode.code(), ScopeCode.point());
  ASSERT_TRUE(bool(ScopeComps)) << llvm::toString(ScopeComps.takeError());
  bool FoundReset = false;
  for (const auto &Item : ScopeComps->items) {
    if (Item.label == "reset")
      FoundReset = true;
  }
  EXPECT_TRUE(FoundReset);

  // Keyword / local completion
  Annotations KeywordCode(R"cpp(
    void test() {
      int localCount = 10;
      ret^
    }
  )cpp");
  auto KwComps = Mod.getCompletions(File, KeywordCode.code(), KeywordCode.point());
  ASSERT_TRUE(bool(KwComps)) << llvm::toString(KwComps.takeError());
  bool FoundReturn = false;
  for (const auto &Item : KwComps->items) {
    if (Item.label == "return")
      FoundReturn = true;
  }
  EXPECT_TRUE(FoundReturn);
}

TEST(PseudoModuleTest, FeatureModuleCompletionsAcrossFileAndHeaders) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string HeaderFile = testPath("FeatureModule.h");
  std::string SourceFile = testPath("FeatureModule.cpp");

  FS.Files[HeaderFile] = R"cpp(
    namespace clang {
    namespace clangd {

    class FeatureModule {
    public:
      virtual ~FeatureModule();
      virtual void *typeId() const;
      virtual void initializeLSP();

      struct Facilities {
        int Scheduler;
        int Index;
        int FS;
        int Server;
        int CDB;
      };
      void initialize(const Facilities &F);
      virtual void stop();
      virtual bool blockUntilIdle();
      Facilities &facilities();

    private:
      Facilities Fac;
    };

    class FeatureModuleSet {
      int Modules;
      int Map;
    public:
      static FeatureModuleSet fromRegistry();
      void add(FeatureModule *M);
      bool addImpl(void *Key, FeatureModule *M, const char *Source);
    };

    using FeatureModuleRegistry = int;

    } // namespace clangd
    } // namespace clang
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  auto hasItem = [](const CompletionList &List, llvm::StringRef Label) {
    for (const auto &Item : List.items) {
      if (Item.label == Label)
        return true;
    }
    return false;
  };

  // Test 1: Inside initialize - general completion on empty line
  Annotations InitEmptyCode(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    void FeatureModule::initialize(const Facilities &F) {
      assert(!Fac && "Initialized twice");
      ^
      Fac.emplace(F);
    }

    }
    }
  )cpp");
  auto InitComps = Mod.getCompletions(SourceFile, InitEmptyCode.code(), InitEmptyCode.point());
  ASSERT_TRUE(bool(InitComps)) << llvm::toString(InitComps.takeError());
  EXPECT_FALSE(InitComps->items.empty());

  // Should have parameter F
  EXPECT_TRUE(hasItem(*InitComps, "F"));
  // Should have class member Fac
  EXPECT_TRUE(hasItem(*InitComps, "Fac"));
  // Should have class member facilities
  EXPECT_TRUE(hasItem(*InitComps, "facilities"));
  // Should have header type Facilities
  EXPECT_TRUE(hasItem(*InitComps, "Facilities"));
  // Should have header class FeatureModule
  EXPECT_TRUE(hasItem(*InitComps, "FeatureModule"));
  // Should have header class FeatureModuleSet
  EXPECT_TRUE(hasItem(*InitComps, "FeatureModuleSet"));
  // Should have document token assert
  EXPECT_TRUE(hasItem(*InitComps, "assert"));

  // Test 2: Member access F. inside initialize
  Annotations FMemberCode(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    void FeatureModule::initialize(const Facilities &F) {
      F.^
    }

    }
    }
  )cpp");
  auto FComps = Mod.getCompletions(SourceFile, FMemberCode.code(), FMemberCode.point());
  ASSERT_TRUE(bool(FComps)) << llvm::toString(FComps.takeError());
  EXPECT_TRUE(hasItem(*FComps, "Scheduler"));
  EXPECT_TRUE(hasItem(*FComps, "Index"));
  EXPECT_TRUE(hasItem(*FComps, "Server"));

  // Test 3: Member access M-> inside add
  Annotations MMemberCode(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    void FeatureModuleSet::add(FeatureModule *M) {
      M->^
    }

    }
    }
  )cpp");
  auto MComps = Mod.getCompletions(SourceFile, MMemberCode.code(), MMemberCode.point());
  ASSERT_TRUE(bool(MComps)) << llvm::toString(MComps.takeError());
  EXPECT_TRUE(hasItem(*MComps, "typeId"));
  EXPECT_TRUE(hasItem(*MComps, "initialize"));
  EXPECT_TRUE(hasItem(*MComps, "stop"));

  // Test 4: Scope resolution FeatureModule::
  Annotations ScopeCode(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    void test() {
      FeatureModule::^
    }

    }
    }
  )cpp");
  auto ScopeComps = Mod.getCompletions(SourceFile, ScopeCode.code(), ScopeCode.point());
  ASSERT_TRUE(bool(ScopeComps)) << llvm::toString(ScopeComps.takeError());
  EXPECT_TRUE(hasItem(*ScopeComps, "Facilities"));
  EXPECT_TRUE(hasItem(*ScopeComps, "initialize"));
  EXPECT_TRUE(hasItem(*ScopeComps, "stop"));

  // Test 5: Scope resolution FeatureModuleSet::
  Annotations SetScopeCode(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    void test() {
      FeatureModuleSet::^
    }

    }
    }
  )cpp");
  auto SetScopeComps = Mod.getCompletions(SourceFile, SetScopeCode.code(), SetScopeCode.point());
  ASSERT_TRUE(bool(SetScopeComps)) << llvm::toString(SetScopeComps.takeError());
  EXPECT_TRUE(hasItem(*SetScopeComps, "fromRegistry"));
  EXPECT_TRUE(hasItem(*SetScopeComps, "add"));

  // Test 6: Chained member access E.getName(). inside fromRegistry
  Annotations ChainedCode(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    FeatureModuleSet FeatureModuleSet::fromRegistry() {
      FeatureModuleSet ModuleSet;
      for (FeatureModuleRegistry::entry E : FeatureModuleRegistry::entries()) {
        auto M = E.instantiate();
        if (void *Key = M->typeId())
          ModuleSet.addImpl(Key, std::move(M), E.getName().^);
        else
          ModuleSet.add(std::move(M));
      }
      return ModuleSet;
    }

    }
    }
  )cpp");
  auto ChainedComps = Mod.getCompletions(SourceFile, ChainedCode.code(), ChainedCode.point());
  ASSERT_TRUE(bool(ChainedComps)) << llvm::toString(ChainedComps.takeError());
  EXPECT_TRUE(hasItem(*ChainedComps, "data"));
  EXPECT_TRUE(hasItem(*ChainedComps, "size"));
  EXPECT_TRUE(hasItem(*ChainedComps, "empty"));

  // Test 7: Chained member access with prefix E.getName().d
  Annotations ChainedPrefixCode(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    FeatureModuleSet FeatureModuleSet::fromRegistry() {
      FeatureModuleSet ModuleSet;
      for (FeatureModuleRegistry::entry E : FeatureModuleRegistry::entries()) {
        auto M = E.instantiate();
        if (void *Key = M->typeId())
          ModuleSet.addImpl(Key, std::move(M), E.getName().d^);
        else
          ModuleSet.add(std::move(M));
      }
      return ModuleSet;
    }

    }
    }
  )cpp");
  auto ChainedPrefixComps = Mod.getCompletions(SourceFile, ChainedPrefixCode.code(), ChainedPrefixCode.point());
  ASSERT_TRUE(bool(ChainedPrefixComps)) << llvm::toString(ChainedPrefixComps.takeError());
  EXPECT_TRUE(hasItem(*ChainedPrefixComps, "data"));
}

TEST(PseudoModuleTest, ClangdServerPseudoOnlyHoverSemanticTokensCompletion) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  PseudoPtr->setPseudoOnly(true);
  Modules.add(std::move(Pseudo));

  ClangdServer::Options Opts = ClangdServer::optsForTest();
  Opts.FeatureModules = &Modules;
  ClangdServer Server(*CDB, FS, Opts);

  Annotations Source(R"cpp(
    class Worker {
    public:
      void $workDecl[[doWork]]();
    };
    void run() {
      Worker w;
      w.$workCall[[doWork]]();
      w.^
    }
  )cpp");

  std::string FilePath = testPath("pseudo_lsp.cpp");
  FS.Files[FilePath] = Source.code().str();
  Server.addDocument(FilePath, Source.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // (a) Hover via onHover
  TextDocumentPositionParams HoverParams;
  HoverParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  HoverParams.position = Source.range("workCall").start;
  std::optional<llvm::Expected<std::optional<Hover>>> HoverResult;
  PseudoPtr->onHover(HoverParams, [&](llvm::Expected<std::optional<Hover>> H) {
    HoverResult = std::move(H);
  });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(HoverResult.has_value());
  ASSERT_TRUE(bool(*HoverResult)) << llvm::toString(HoverResult->takeError());
  ASSERT_TRUE((*HoverResult)->has_value());
  EXPECT_THAT((**HoverResult)->contents.value, testing::HasSubstr("doWork"));

  // (b) Semantic tokens full via onSemanticTokens
  SemanticTokensParams STParams;
  STParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  std::optional<llvm::Expected<SemanticTokens>> STResult;
  PseudoPtr->onSemanticTokens(STParams, [&](llvm::Expected<SemanticTokens> ST) {
    STResult = std::move(ST);
  });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(STResult.has_value());
  ASSERT_TRUE(bool(*STResult)) << llvm::toString(STResult->takeError());
  EXPECT_FALSE((*STResult)->tokens.empty());
  std::string FirstResultId = (*STResult)->resultId;
  EXPECT_FALSE(FirstResultId.empty());

  // (c) Semantic tokens delta via onSemanticTokensDelta
  SemanticTokensDeltaParams DeltaParams;
  DeltaParams.textDocument.uri = STParams.textDocument.uri;
  DeltaParams.previousResultId = FirstResultId;
  std::optional<llvm::Expected<SemanticTokensOrDelta>> DeltaResult;
  PseudoPtr->onSemanticTokensDelta(DeltaParams, [&](llvm::Expected<SemanticTokensOrDelta> D) {
    DeltaResult = std::move(D);
  });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(DeltaResult.has_value());
  ASSERT_TRUE(bool(*DeltaResult)) << llvm::toString(DeltaResult->takeError());
  EXPECT_NE((*DeltaResult)->resultId, FirstResultId);
  ASSERT_TRUE((*DeltaResult)->edits.has_value());
  EXPECT_TRUE((*DeltaResult)->edits->empty());

  // (d) Completion via onCompletion
  CompletionParams CompParams;
  CompParams.textDocument.uri = HoverParams.textDocument.uri;
  CompParams.position = Source.point();
  std::optional<llvm::Expected<CompletionList>> CompResult;
  PseudoPtr->onCompletion(CompParams, [&](llvm::Expected<CompletionList> CL) {
    CompResult = std::move(CL);
  });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(CompResult.has_value());
  ASSERT_TRUE(bool(*CompResult)) << llvm::toString(CompResult->takeError());
  bool FoundDoWork = false;
  for (const auto &Item : (*CompResult)->items) {
    if (Item.label == "doWork")
      FoundDoWork = true;
  }
  EXPECT_TRUE(FoundDoWork);
}

TEST(PseudoModuleTest, ClangdServerFallbackHoverSemanticTokensCompletion) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();
  // Invalid compile command that causes Clang AST build to fail
  CDB->ExtraClangFlags = {"-###"};

  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  Modules.add(std::move(Pseudo));

  ClangdServer::Options Opts = ClangdServer::optsForTest();
  Opts.FeatureModules = &Modules;
  ClangdServer Server(*CDB, FS, Opts);

  Annotations Source(R"cpp(
    class BackupService {
    public:
      void $decl[[performBackup]]();
    };
    void test() {
      BackupService svc;
      svc.$call[[performBackup]]();
      svc.^
    }
  )cpp");

  std::string FilePath = testPath("broken_ast.cpp");
  FS.Files[FilePath] = Source.code().str();
  Server.addDocument(FilePath, Source.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // Clang AST fails to build
  EXPECT_EQ(dumpAST(Server, FilePath), "<no-ast>");

  // Fallback for hover
  TextDocumentPositionParams HoverParams;
  HoverParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  HoverParams.position = Source.range("call").start;
  std::optional<llvm::Expected<std::optional<Hover>>> HoverResult;
  PseudoPtr->onHover(HoverParams, [&](llvm::Expected<std::optional<Hover>> H) {
    HoverResult = std::move(H);
  });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(HoverResult.has_value());
  ASSERT_TRUE(bool(*HoverResult)) << llvm::toString(HoverResult->takeError());
  ASSERT_TRUE((*HoverResult)->has_value());
  EXPECT_THAT((**HoverResult)->contents.value, testing::HasSubstr("performBackup"));

  // Fallback for semantic tokens
  SemanticTokensParams STParams;
  STParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  std::optional<llvm::Expected<SemanticTokens>> STResult;
  PseudoPtr->onSemanticTokens(STParams, [&](llvm::Expected<SemanticTokens> ST) {
    STResult = std::move(ST);
  });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(STResult.has_value());
  ASSERT_TRUE(bool(*STResult)) << llvm::toString(STResult->takeError());
  EXPECT_FALSE((*STResult)->tokens.empty());

  // Fallback for completions
  CompletionParams CompParams;
  CompParams.textDocument.uri = HoverParams.textDocument.uri;
  CompParams.position = Source.point();
  std::optional<llvm::Expected<CompletionList>> CompResult;
  PseudoPtr->onCompletion(CompParams, [&](llvm::Expected<CompletionList> CL) {
    CompResult = std::move(CL);
  });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(CompResult.has_value());
  ASSERT_TRUE(bool(*CompResult)) << llvm::toString(CompResult->takeError());
  bool FoundPerformBackup = false;
  for (const auto &Item : (*CompResult)->items) {
    if (Item.label == "performBackup")
      FoundPerformBackup = true;
  }
  EXPECT_TRUE(FoundPerformBackup);
}

TEST(PseudoModuleTest, StdMoveHoverCompletionAndGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string UtilityHeader = testPath("utility");
  std::string FeatureHeader = testPath("FeatureModule.h");
  std::string SourceFile = testPath("FeatureModule.cpp");

  FS.Files[UtilityHeader] = R"cpp(
    namespace std {
      template <typename T>
      constexpr T&& move(T& t) noexcept;
    }
  )cpp";

  FS.Files[FeatureHeader] = R"cpp(
    #include "utility"
    namespace clang {
    namespace clangd {
      class FeatureModule {};
      class FeatureModuleSet {
        int Modules;
      public:
        void add(FeatureModule *M);
      };
    }
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  // 1. Hover on std::move: cursor on 'move'
  Annotations HoverMoveCode(R"cpp(
    #include "FeatureModule.h"
    namespace clang {
    namespace clangd {
    void FeatureModuleSet::add(FeatureModule *M) {
      Modules.push_back(std::$move^move(M));
    }
    }
    }
  )cpp");
  auto HMove = Mod.getHover(SourceFile, HoverMoveCode.code(), HoverMoveCode.point("move"));
  ASSERT_TRUE(bool(HMove)) << llvm::toString(HMove.takeError());
  ASSERT_TRUE(HMove->has_value());
  EXPECT_TRUE((*HMove)->contents.value.find("move") != std::string::npos);
  EXPECT_TRUE((*HMove)->contents.value.find("remove_reference_t") != std::string::npos);

  // 2. Hover on std::move: cursor on 'std'
  Annotations HoverStdCode(R"cpp(
    #include "FeatureModule.h"
    namespace clang {
    namespace clangd {
    void FeatureModuleSet::add(FeatureModule *M) {
      Modules.push_back($std^std::move(M));
    }
    }
    }
  )cpp");
  auto HStd = Mod.getHover(SourceFile, HoverStdCode.code(), HoverStdCode.point("std"));
  ASSERT_TRUE(bool(HStd)) << llvm::toString(HStd.takeError());
  ASSERT_TRUE(HStd->has_value());
  EXPECT_TRUE((*HStd)->contents.value.find("namespace std") != std::string::npos);

  // 3. Completion on std:: inside call
  Annotations CompStdCode(R"cpp(
    #include "FeatureModule.h"
    namespace clang {
    namespace clangd {
    void FeatureModuleSet::add(FeatureModule *M) {
      Modules.push_back(std::^);
    }
    }
    }
  )cpp");
  auto CStd = Mod.getCompletions(SourceFile, CompStdCode.code(), CompStdCode.point());
  ASSERT_TRUE(bool(CStd)) << llvm::toString(CStd.takeError());
  bool FoundMove = false;
  bool FoundForward = false;
  bool FoundUniquePtr = false;
  for (const auto &Item : CStd->items) {
    if (Item.label == "move") {
      FoundMove = true;
      EXPECT_EQ(Item.kind, CompletionItemKind::Function);
      EXPECT_TRUE(Item.detail.find("move") != std::string::npos);
    }
    if (Item.label == "forward")
      FoundForward = true;
    if (Item.label == "unique_ptr")
      FoundUniquePtr = true;
  }
  EXPECT_TRUE(FoundMove);
  EXPECT_TRUE(FoundForward);
  EXPECT_TRUE(FoundUniquePtr);

  // 4. Completion on std::mo
  Annotations CompStdMoCode(R"cpp(
    #include "FeatureModule.h"
    namespace clang {
    namespace clangd {
    void FeatureModuleSet::add(FeatureModule *M) {
      Modules.push_back(std::mo^);
    }
    }
    }
  )cpp");
  auto CStdMo = Mod.getCompletions(SourceFile, CompStdMoCode.code(), CompStdMoCode.point());
  ASSERT_TRUE(bool(CStdMo)) << llvm::toString(CStdMo.takeError());
  FoundMove = false;
  for (const auto &Item : CStdMo->items) {
    if (Item.label == "move")
      FoundMove = true;
  }
  EXPECT_TRUE(FoundMove);

  // 5. GTD on move jumps to utility header
  Annotations GTDCode(R"cpp(
    #include "FeatureModule.h"
    namespace clang {
    namespace clangd {
    void FeatureModuleSet::add(FeatureModule *M) {
      Modules.push_back(std::$move^move(M));
    }
    }
    }
  )cpp");
  auto Locs = Mod.locateSymbolAt(SourceFile, GTDCode.code(), GTDCode.point("move"));
  ASSERT_TRUE(bool(Locs)) << llvm::toString(Locs.takeError());
  ASSERT_FALSE(Locs->empty());
  EXPECT_EQ(Locs->front().Name, "move");

  // 6. Semantic Highlighting
  auto Highs = Mod.getSemanticHighlightings(GTDCode.code());
  ASSERT_TRUE(bool(Highs)) << llvm::toString(Highs.takeError());
  bool HighlightedStd = false;
  bool HighlightedMove = false;
  for (const auto &Tok : *Highs) {
    if (Tok.Kind == HighlightingKind::Namespace)
      HighlightedStd = true;
    if (Tok.Kind == HighlightingKind::Function)
      HighlightedMove = true;
  }
  EXPECT_TRUE(HighlightedStd);
  EXPECT_TRUE(HighlightedMove);
}

TEST(PseudoModuleTest, FeatureModuleRegistryEntriesAndRegistryEntryMethodsGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string RegistryHeader = testPath("Registry.h");
  std::string UtilityHeader = testPath("utility");
  std::string FeatureHeader = testPath("FeatureModule.h");
  std::string SourceFile = testPath("FeatureModule.cpp");

  FS.Files[RegistryHeader] = R"cpp(
    namespace llvm {
    template <typename T>
    class SimpleRegistryEntry {
    public:
      void getName();
      void getDesc();
      void instantiate();
    };

    template <typename T>
    class Registry {
    public:
      using entry = SimpleRegistryEntry<T>;
      static void entries();
    };
    }
  )cpp";

  FS.Files[UtilityHeader] = R"cpp(
    namespace std {
      template <typename T>
      constexpr T&& move(T& t) noexcept;
    }
  )cpp";

  FS.Files[FeatureHeader] = R"cpp(
    #include "Registry.h"
    namespace clang {
    namespace clangd {
      class FeatureModule {};
      class FeatureModuleSet {
      public:
        static FeatureModuleSet fromRegistry();
      };
      using FeatureModuleRegistry = llvm::Registry<FeatureModule>;
    }
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "FeatureModule.h"
    namespace clang {
    namespace clangd {
    FeatureModuleSet FeatureModuleSet::fromRegistry() {
      FeatureModuleSet ModuleSet;
      for (FeatureModuleRegistry::entry E : FeatureModuleRegistry::$entries^entries()) {
        E.$getName^getName();
        E.$getDesc^getDesc();
        auto M = E.$instantiate^instantiate();
        std::$move^move(M);
      }
      return ModuleSet;
    }
    }
    }
  )cpp");

  // 1. GTD on FeatureModuleRegistry::entries()
  auto LocEntries = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("entries"));
  ASSERT_TRUE(bool(LocEntries)) << llvm::toString(LocEntries.takeError());
  ASSERT_FALSE(LocEntries->empty());
  EXPECT_EQ(LocEntries->front().Name, "entries");
  EXPECT_TRUE(llvm::StringRef(LocEntries->front().PreferredDeclaration.uri.file())
                  .ends_with("Registry.h"));

  // 2. GTD on E.getName()
  auto LocGetName = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("getName"));
  ASSERT_TRUE(bool(LocGetName)) << llvm::toString(LocGetName.takeError());
  ASSERT_FALSE(LocGetName->empty());
  EXPECT_EQ(LocGetName->front().Name, "getName");
  EXPECT_TRUE(llvm::StringRef(LocGetName->front().PreferredDeclaration.uri.file())
                  .ends_with("Registry.h"));

  // 3. GTD on E.getDesc()
  auto LocGetDesc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("getDesc"));
  ASSERT_TRUE(bool(LocGetDesc)) << llvm::toString(LocGetDesc.takeError());
  ASSERT_FALSE(LocGetDesc->empty());
  EXPECT_EQ(LocGetDesc->front().Name, "getDesc");
  EXPECT_TRUE(llvm::StringRef(LocGetDesc->front().PreferredDeclaration.uri.file())
                  .ends_with("Registry.h"));

  // 4. GTD on E.instantiate()
  auto LocInst = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("instantiate"));
  ASSERT_TRUE(bool(LocInst)) << llvm::toString(LocInst.takeError());
  ASSERT_FALSE(LocInst->empty());
  EXPECT_EQ(LocInst->front().Name, "instantiate");
  EXPECT_TRUE(llvm::StringRef(LocInst->front().PreferredDeclaration.uri.file())
                  .ends_with("Registry.h"));

  // 5. GTD on std::move()
  auto LocMove = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("move"));
  ASSERT_TRUE(bool(LocMove)) << llvm::toString(LocMove.takeError());
  ASSERT_FALSE(LocMove->empty());
  EXPECT_EQ(LocMove->front().Name, "move");
  EXPECT_TRUE(llvm::StringRef(LocMove->front().PreferredDeclaration.uri.file())
                  .ends_with("utility"));

  // 6. Hover on E.getName()
  auto HGetName = Mod.getHover(SourceFile, Code.code(), Code.point("getName"));
  ASSERT_TRUE(bool(HGetName)) << llvm::toString(HGetName.takeError());
  ASSERT_TRUE(HGetName->has_value());
  EXPECT_TRUE((*HGetName)->contents.value.find("getName") != std::string::npos);

  // 7. Hover on FeatureModuleRegistry::entries()
  auto HEntries = Mod.getHover(SourceFile, Code.code(), Code.point("entries"));
  ASSERT_TRUE(bool(HEntries)) << llvm::toString(HEntries.takeError());
  ASSERT_TRUE(HEntries->has_value());
  EXPECT_TRUE((*HEntries)->contents.value.find("entries") != std::string::npos);

  // 8. Hover on std::move()
  auto HMove = Mod.getHover(SourceFile, Code.code(), Code.point("move"));
  ASSERT_TRUE(bool(HMove)) << llvm::toString(HMove.takeError());
  ASSERT_TRUE(HMove->has_value());
  EXPECT_TRUE((*HMove)->contents.value.find("move") != std::string::npos);
}

TEST(PseudoModuleTest, SemaPrintingPolicyGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string ASTContextH = testPath("clang/AST/ASTContext.h");
  std::string PrettyPrinterH = testPath("clang/AST/PrettyPrinter.h");
  std::string LangOptionsH = testPath("clang/Basic/LangOptions.h");
  std::string LangOptionsDef = testPath("clang/Basic/LangOptions.def");
  std::string MacroInfoH = testPath("clang/Lex/MacroInfo.h");
  std::string PreprocessorH = testPath("clang/Lex/Preprocessor.h");
  std::string SemaH = testPath("clang/Sema/Sema.h");
  std::string SemaCodeCompletionH = testPath("clang/Sema/SemaCodeCompletion.h");
  std::string SourceFile = testPath("clang/lib/Sema/Sema.cpp");

  FS.Files[PrettyPrinterH] = R"cpp(
    namespace clang {
    struct PrintingPolicy {
      bool Bool;
      LLVM_PREFERRED_TYPE(bool)
      unsigned EntireContentsOfLargeArray : 1;
    };
    }
  )cpp";

  FS.Files[LangOptionsDef] = R"cpp(
    LANGOPT(Bool, 1, 0, NotCompatible, "bool, true, and false keywords")
  )cpp";

  FS.Files[LangOptionsH] = R"cpp(
    #include "clang/Basic/LangOptions.def"
    namespace clang {
    class LangOptionsBase {
    };
    class LangOptions : public LangOptionsBase {
    };
    }
  )cpp";

  FS.Files[ASTContextH] = R"cpp(
    #include "clang/AST/PrettyPrinter.h"
    #include "clang/Basic/LangOptions.h"
    namespace clang {
    class ASTContext {
    public:
      const PrintingPolicy &getPrintingPolicy() const;
      const LangOptions &getLangOpts() const;
      const char *getBoolName() const;
    };
    }
  )cpp";

  FS.Files[MacroInfoH] = R"cpp(
    namespace clang {
    class MacroInfo {
    public:
      bool isObjectLike() const;
      unsigned getNumTokens() const;
    };
    }
  )cpp";

  FS.Files[PreprocessorH] = R"cpp(
    #include "clang/Lex/MacroInfo.h"
    namespace clang {
    class Preprocessor {
    public:
      const MacroInfo *getMacroInfo(const char *Name) const;
    };
    }
  )cpp";

  FS.Files[SemaCodeCompletionH] = R"cpp(
    namespace clang {
    class MacroInfo;
    }
  )cpp";

  FS.Files[SemaH] = R"cpp(
    #include "clang/AST/ASTContext.h"
    #include "clang/Lex/Preprocessor.h"
    #include "clang/Sema/SemaCodeCompletion.h"
    namespace clang {
    class Sema {
    public:
      PrintingPolicy getPrintingPolicy(const ASTContext &Context, const Preprocessor &PP);
    };
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "clang/AST/ASTContext.h"
    #include "clang/Lex/Preprocessor.h"
    #include "clang/Sema/SemaCodeCompletion.h"
    #include "clang/Sema/Sema.h"
    namespace clang {
    PrintingPolicy Sema::getPrintingPolicy(const ASTContext &Context,
                                           const Preprocessor &PP) {
      PrintingPolicy Policy = Context.$ctxPolicy^getPrintingPolicy();
      Policy.Bool = Context.getLangOpts().$langOptBool^Bool;
      if (!Policy.Bool) {
        if (const $macroInfo^MacroInfo *BoolMacro = PP.getMacroInfo(Context.getBoolName())) {
          Policy.Bool = BoolMacro->$isObj^isObjectLike();
        }
      }
      Policy.$largeArray^EntireContentsOfLargeArray = false;
      return Policy;
    }
    }
  )cpp");

  // 1. Context.getPrintingPolicy() must jump to ASTContext::getPrintingPolicy, NOT Sema::getPrintingPolicy
  auto LocCtxPolicy = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("ctxPolicy"));
  EXPECT_TRUE(bool(LocCtxPolicy)) << (LocCtxPolicy ? "" : llvm::toString(LocCtxPolicy.takeError()));
  if (LocCtxPolicy && !LocCtxPolicy->empty()) {
    EXPECT_EQ(LocCtxPolicy->front().Name, "getPrintingPolicy");
    EXPECT_TRUE(llvm::StringRef(LocCtxPolicy->front().PreferredDeclaration.uri.file())
                    .ends_with("ASTContext.h"))
        << "Expected ASTContext.h, got " << LocCtxPolicy->front().PreferredDeclaration.uri.file();
  } else {
    ADD_FAILURE() << "LocCtxPolicy is empty";
  }

  // 2. Context.getLangOpts().Bool must jump to LangOptions.def
  auto LocBool = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("langOptBool"));
  EXPECT_TRUE(bool(LocBool)) << (LocBool ? "" : llvm::toString(LocBool.takeError()));
  if (LocBool && !LocBool->empty()) {
    EXPECT_EQ(LocBool->front().Name, "Bool");
    EXPECT_TRUE(llvm::StringRef(LocBool->front().PreferredDeclaration.uri.file())
                    .ends_with("LangOptions.def"))
        << "Expected LangOptions.def, got " << LocBool->front().PreferredDeclaration.uri.file();
  } else {
    ADD_FAILURE() << "LocBool is empty";
  }

  // 3. MacroInfo must jump to MacroInfo class definition (MacroInfo.h), not forward decl
  auto LocMacroInfo = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("macroInfo"));
  EXPECT_TRUE(bool(LocMacroInfo)) << (LocMacroInfo ? "" : llvm::toString(LocMacroInfo.takeError()));
  if (LocMacroInfo && !LocMacroInfo->empty()) {
    EXPECT_EQ(LocMacroInfo->front().Name, "MacroInfo");
    EXPECT_TRUE(llvm::StringRef(LocMacroInfo->front().PreferredDeclaration.uri.file())
                    .ends_with("MacroInfo.h"))
        << "Expected MacroInfo.h, got " << LocMacroInfo->front().PreferredDeclaration.uri.file();
  } else {
    ADD_FAILURE() << "LocMacroInfo is empty";
  }

  // 4. BoolMacro->isObjectLike() must jump to MacroInfo::isObjectLike in MacroInfo.h
  auto LocIsObj = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("isObj"));
  EXPECT_TRUE(bool(LocIsObj)) << (LocIsObj ? "" : llvm::toString(LocIsObj.takeError()));
  if (LocIsObj && !LocIsObj->empty()) {
    EXPECT_EQ(LocIsObj->front().Name, "isObjectLike");
    EXPECT_TRUE(llvm::StringRef(LocIsObj->front().PreferredDeclaration.uri.file())
                    .ends_with("MacroInfo.h"))
        << "Expected MacroInfo.h, got " << LocIsObj->front().PreferredDeclaration.uri.file();
  } else {
    ADD_FAILURE() << "LocIsObj is empty";
  }

  // 5. Policy.EntireContentsOfLargeArray must jump to PrettyPrinter.h
  auto LocLargeArray = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("largeArray"));
  EXPECT_TRUE(bool(LocLargeArray)) << (LocLargeArray ? "" : llvm::toString(LocLargeArray.takeError()));
  if (LocLargeArray && !LocLargeArray->empty()) {
    EXPECT_EQ(LocLargeArray->front().Name, "EntireContentsOfLargeArray");
    EXPECT_TRUE(llvm::StringRef(LocLargeArray->front().PreferredDeclaration.uri.file())
                    .ends_with("PrettyPrinter.h"))
        << "Expected PrettyPrinter.h, got " << LocLargeArray->front().PreferredDeclaration.uri.file();
  } else {
    ADD_FAILURE() << "LocLargeArray is empty";
  }
}

TEST(PseudoModuleTest, ClangdServerDraftMgrGetDraftGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string DraftStoreH = testPath("DraftStore.h");
  std::string ClangdServerH = testPath("ClangdServer.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[DraftStoreH] = R"cpp(
    namespace clang {
    namespace clangd {
    class DraftStore {
    public:
      int getDraft(const char *File) const;
    };
    }
    }
  )cpp";

  FS.Files[ClangdServerH] = R"cpp(
    #include "DraftStore.h"
    namespace clang {
    namespace clangd {
    class ClangdServer {
    public:
      int getDraft(const char *File) const;
      void reparseOpenFilesIfNeeded();
    private:
      DraftStore DraftMgr;
    };
    }
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "ClangdServer.h"
    namespace clang {
    namespace clangd {
    int ClangdServer::getDraft(const char *File) const {
      return DraftMgr.$callGetDraft^getDraft(File);
    }
    }
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("callGetDraft"));
  EXPECT_TRUE(bool(Loc)) << (Loc ? "" : llvm::toString(Loc.takeError()));
  if (Loc && !Loc->empty()) {
    EXPECT_EQ(Loc->front().Name, "getDraft");
    EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file())
                    .ends_with("DraftStore.h"))
        << "Expected DraftStore.h, got " << Loc->front().PreferredDeclaration.uri.file();
  } else {
    ADD_FAILURE() << "Loc is empty";
  }
}

TEST(PseudoModuleTest, StdMoveJumpsToFunctionNotInclude) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string UtilityH = testPath("utility");
  std::string BitsMoveH = testPath("bits/move.h");
  std::string SourceFile = testPath("test.cpp");

  FS.Files[BitsMoveH] = R"cpp(
    namespace std {
    template <typename T>
    constexpr T&& move(T& t) noexcept {
      return static_cast<T&&>(t);
    }
    }
  )cpp";

  FS.Files[UtilityH] = R"cpp(
    #include <bits/move.h>
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include <utility>
    void test() {
      int x = 42;
      int y = std::$moveCall^move(x);
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("moveCall"));
  EXPECT_TRUE(bool(Loc)) << (Loc ? "" : llvm::toString(Loc.takeError()));
  ASSERT_TRUE(Loc && !Loc->empty());
  EXPECT_EQ(Loc->front().Name, "move");
  EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("bits/move.h"))
      << "Expected bits/move.h, got " << Loc->front().PreferredDeclaration.uri.file();
  EXPECT_NE(Loc->front().PreferredDeclaration.range.start.line, 0)
      << "Should not jump to line 0 (#include directive)";
}

TEST(PseudoModuleTest, ClangdServerCallbacksTypeGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string ClangdServerH = testPath("ClangdServer.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[ClangdServerH] = R"cpp(
    namespace clang {
    namespace clangd {
    class ClangdServer {
    public:
      class Callbacks {
      public:
        virtual ~Callbacks() = default;
      };
      ClangdServer(int Opts, Callbacks *Callbacks = nullptr);
    };
    }
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "ClangdServer.h"
    namespace clang {
    namespace clangd {
    ClangdServer::ClangdServer(int Opts, Callbacks *Callbacks) {}

    struct Impl {
      ClangdServer::$qualCallbacks^Callbacks *Publish;
    };
    }
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("qualCallbacks"));
  EXPECT_TRUE(bool(Loc)) << (Loc ? "" : llvm::toString(Loc.takeError()));
  ASSERT_TRUE(Loc && !Loc->empty());
  EXPECT_EQ(Loc->front().Name, "Callbacks");
  EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("ClangdServer.h"))
      << "Expected ClangdServer.h, got " << Loc->front().PreferredDeclaration.uri.file();
  EXPECT_NE(Loc->front().PreferredDeclaration.uri.file(), SourceFile);
}

TEST(PseudoModuleTest, StringRefInLambdaParamGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string StringRefH = testPath("llvm/ADT/StringRef.h");
  std::string ConfigH = testPath("Config.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[StringRefH] = R"cpp(
    namespace llvm {
    class StringRef {
    public:
      StringRef();
    };
    }
  )cpp";

  FS.Files[ConfigH] = R"cpp(
    #include "llvm/ADT/StringRef.h"
    #include <vector>
    #include <functional>
    struct Config {
      std::vector<std::function<bool(llvm::StringRef)>> IgnoreHeader;
    };
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "Config.h"
    #include "llvm/ADT/StringRef.h"
    void test() {
      auto fn = [](llvm::$strRef^StringRef) {};
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("strRef"));
  EXPECT_TRUE(bool(Loc)) << (Loc ? "" : llvm::toString(Loc.takeError()));
  ASSERT_TRUE(Loc && !Loc->empty());
  EXPECT_EQ(Loc->front().Name, "StringRef");
  EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("StringRef.h"))
      << "Expected StringRef.h, got " << Loc->front().PreferredDeclaration.uri.file();
  EXPECT_FALSE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("Config.h"))
      << "Must not jump to Config.h!";
}

TEST(PseudoModuleTest, CtorMemberInitializerGTD) {
  Annotations Code(R"cpp(
    struct Callbacks {};
    struct Provider {};
    struct Impl {
      const Provider *$fieldProv^Provider;
      Callbacks *$fieldPub^Publish;

      Impl(const Provider *$paramProv^Provider, Callbacks *$paramPub^Publish)
          : $initProvField^Provider($initProvArg^Provider),
            $initPubField^Publish($initPubArg^Publish) {}
    };
  )cpp");

  PseudoModule Mod;
  std::string SourceFile = testPath("Test.cpp");

  // Field before '(' must jump to class field declaration
  auto LocFieldPub = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("initPubField"));
  ASSERT_TRUE(LocFieldPub && !LocFieldPub->empty());
  EXPECT_EQ(LocFieldPub->front().PreferredDeclaration.range.start, Code.point("fieldPub"));

  auto LocFieldProv = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("initProvField"));
  ASSERT_TRUE(LocFieldProv && !LocFieldProv->empty());
  EXPECT_EQ(LocFieldProv->front().PreferredDeclaration.range.start, Code.point("fieldProv"));

  // Argument inside '(' must jump to constructor parameter
  auto LocArgPub = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("initPubArg"));
  ASSERT_TRUE(LocArgPub && !LocArgPub->empty());
  EXPECT_EQ(LocArgPub->front().PreferredDeclaration.range.start, Code.point("paramPub"));

  auto LocArgProv = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("initProvArg"));
  ASSERT_TRUE(LocArgProv && !LocArgProv->empty());
  EXPECT_EQ(LocArgProv->front().PreferredDeclaration.range.start, Code.point("paramProv"));
}

TEST(PseudoModuleTest, ContextCurrentCloneGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string ContextH = testPath("support/Context.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[ContextH] = R"cpp(
    namespace clang {
    namespace clangd {
    class Context {
    public:
      static const Context &current();
      Context clone() const;
    };
    }
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "support/Context.h"
    void test() {
      auto fn = []() {
        return Context::$cur^current().$cln^clone();
      };
    }
  )cpp");

  auto LocCur = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("cur"));
  ASSERT_TRUE(LocCur && !LocCur->empty());
  EXPECT_EQ(LocCur->front().Name, "current");
  EXPECT_TRUE(llvm::StringRef(LocCur->front().PreferredDeclaration.uri.file()).ends_with("Context.h"));

  auto LocCln = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("cln"));
  ASSERT_TRUE(LocCln && !LocCln->empty());
  EXPECT_EQ(LocCln->front().Name, "clone");
  EXPECT_TRUE(llvm::StringRef(LocCln->front().PreferredDeclaration.uri.file()).ends_with("Context.h"));
}

TEST(PseudoModuleTest, RealStringRefMacroClassGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string StringRefH = testPath("llvm/ADT/StringRef.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[StringRefH] = R"cpp(
    namespace llvm {
    class StringRef;
    class LLVM_GSL_POINTER StringRef {
    public:
      StringRef();
    };
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "llvm/ADT/StringRef.h"
    void test() {
      auto fn = [](llvm::$strRef^StringRef) {};
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("strRef"));
  ASSERT_TRUE(Loc && !Loc->empty());
  EXPECT_EQ(Loc->front().Name, "StringRef");
  EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("StringRef.h"));
}

TEST(PseudoModuleTest, ClangdServerLine359IntegrationGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string StringRefH = testPath("llvm/ADT/StringRef.h");
  std::string ContextH = testPath("support/Context.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[StringRefH] = R"cpp(
    namespace llvm {
    class StringRef;
    class LLVM_GSL_POINTER StringRef {
    public:
      StringRef();
    };
    }
  )cpp";

  FS.Files[ContextH] = R"cpp(
    namespace clang {
    namespace clangd {
    class Context {
    public:
      static const Context &current();
      Context clone() const;
    };
    }
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "support/Context.h"
    #include "llvm/ADT/StringRef.h"
    namespace clang {
    namespace clangd {
    namespace config { struct Provider {}; }
    class ClangdServer {
      struct Callbacks {};
      std::function<Context(PathRef)>
      createConfiguredContextProvider(const config::Provider *Provider,
                                      Callbacks *Publish) {
        if (!Provider)
          return [](llvm::$strRef^StringRef) {
            return Context::$cur^current().$cln^clone();
          };

        struct Impl {
          const config::Provider *$provField^Provider;
          ClangdServer::Callbacks *$pubField^Publish;

          Impl(const config::Provider *$provParam^Provider, ClangdServer::Callbacks *$pubParam^Publish)
              : $provInit^Provider($provArg^Provider), $pubInit^Publish($pubArg^Publish) {}
        };
        return Impl(Provider, Publish);
      }
    };
    }
    }
  )cpp");

  auto LocStrRef = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("strRef"));
  ASSERT_TRUE(LocStrRef && !LocStrRef->empty());
  EXPECT_EQ(LocStrRef->front().Name, "StringRef");
  EXPECT_TRUE(llvm::StringRef(LocStrRef->front().PreferredDeclaration.uri.file()).ends_with("StringRef.h"));

  auto LocCur = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("cur"));
  ASSERT_TRUE(LocCur && !LocCur->empty());
  EXPECT_EQ(LocCur->front().Name, "current");
  EXPECT_TRUE(llvm::StringRef(LocCur->front().PreferredDeclaration.uri.file()).ends_with("Context.h"));

  auto LocClone = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("cln"));
  ASSERT_TRUE(LocClone && !LocClone->empty());
  EXPECT_EQ(LocClone->front().Name, "clone");
  EXPECT_TRUE(llvm::StringRef(LocClone->front().PreferredDeclaration.uri.file()).ends_with("Context.h"));

  auto LocFirstPub = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("pubInit"));
  ASSERT_TRUE(LocFirstPub && !LocFirstPub->empty());
  EXPECT_EQ(LocFirstPub->front().PreferredDeclaration.range.start, Code.point("pubField"));

  auto LocSecondPub = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("pubArg"));
  ASSERT_TRUE(LocSecondPub && !LocSecondPub->empty());
  EXPECT_EQ(LocSecondPub->front().PreferredDeclaration.range.start, Code.point("pubParam"));

  auto LocFirstProv = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("provInit"));
  ASSERT_TRUE(LocFirstProv && !LocFirstProv->empty());
  EXPECT_EQ(LocFirstProv->front().PreferredDeclaration.range.start, Code.point("provField"));

  auto LocSecondProv = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("provArg"));
  ASSERT_TRUE(LocSecondProv && !LocSecondProv->empty());
  EXPECT_EQ(LocSecondProv->front().PreferredDeclaration.range.start, Code.point("provParam"));
}

TEST(PseudoModuleTest, PosixPathStrGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string H1 = testPath("H1.h");
  std::string H2 = testPath("H2.h");
  std::string H3 = testPath("H3.h");
  std::string H4 = testPath("H4.h");
  std::string SmallStringH = testPath("llvm/ADT/SmallString.h");
  std::string SStreamH = testPath("sstream");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[H1] = "#include \"H2.h\"\n";
  FS.Files[H2] = "#include \"H3.h\"\n";
  FS.Files[H3] = "#include \"H4.h\"\n";
  FS.Files[H4] = "#include \"llvm/ADT/SmallString.h\"\n";

  FS.Files[SmallStringH] = R"cpp(
    namespace llvm {
    template <unsigned N>
    class SmallString {
    public:
      StringRef str() const;
    };
    }
  )cpp";

  FS.Files[SStreamH] = R"cpp(
    namespace std {
    class stringstream {
    public:
      string str() const;
    };
    }
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "H1.h"
    #include "sstream"
    namespace clang {
    namespace clangd {
    class ClangdServer {
      struct Impl {
        void operator()(llvm::StringRef File) {
          llvm::SmallString<256> PosixPath;
          PosixPath.$target^str();
        }
      };
    };
    }
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("target"));
  ASSERT_TRUE(Loc && !Loc->empty()) << (Loc ? "empty" : llvm::toString(Loc.takeError()));
  EXPECT_EQ(Loc->front().Name, "str");
  EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("SmallString.h"))
      << "Got: " << Loc->front().PreferredDeclaration.uri.file();

  auto Hover = Mod.getHover(SourceFile, Code.code(), Code.point("target"));
  ASSERT_TRUE(Hover && Hover->has_value());
  EXPECT_TRUE(llvm::StringRef((*Hover)->contents.value).contains("SmallString"));

  // Verify that an unknown receiver type or unmatched method NEVER jumps to a random std implementation
  Annotations UnknownCode(R"cpp(
    #include "sstream"
    void bar() {
      UnknownType Var;
      Var.$target^str();
    }
  )cpp");
  auto UnknownLoc = Mod.locateSymbolAt(SourceFile, UnknownCode.code(), UnknownCode.point("target"));
  EXPECT_TRUE(!UnknownLoc || UnknownLoc->empty());
}

TEST(PseudoModuleTest, LambdaScopeAndGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string SourceFile = testPath("test.cpp");
  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    void codeComplete(int $cbOuter^CB) {
      int $optsOuter^CodeCompleteOpts = 42;
      auto Task = [$optsCap^CodeCompleteOpts, $cbCap^CB = CB](int $paramIP^IP) mutable {
        int $specVar^SpecFuzzyFind = 1;
        int $parseInputVar^ParseInput{2};
        $cbUse^CB;
        $specUse^SpecFuzzyFind;
        $parseInputUse^ParseInput;
        $optsUse^CodeCompleteOpts;
      };
    }
  )cpp");

  auto LocCB = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("cbUse"));
  ASSERT_TRUE(LocCB && !LocCB->empty());
  EXPECT_EQ(LocCB->front().PreferredDeclaration.range.start, Code.point("cbCap"));

  auto LocSpec = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("specUse"));
  ASSERT_TRUE(LocSpec && !LocSpec->empty());
  EXPECT_EQ(LocSpec->front().PreferredDeclaration.range.start, Code.point("specVar"));

  auto LocParseInput = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("parseInputUse"));
  ASSERT_TRUE(LocParseInput && !LocParseInput->empty());
  EXPECT_EQ(LocParseInput->front().PreferredDeclaration.range.start, Code.point("parseInputVar"));

  auto LocOpts = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("optsUse"));
  ASSERT_TRUE(LocOpts && !LocOpts->empty());
  EXPECT_EQ(LocOpts->front().PreferredDeclaration.range.start, Code.point("optsCap"));
}

TEST(PseudoModuleTest, WantDiagnosticsAutoGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string TUSchedulerH = testPath("TUScheduler.h");
  std::string SourceFile = testPath("test.cpp");

  Annotations Header(R"cpp(
    namespace clang {
    namespace clangd {
    enum class WantDiagnostics {
      Yes,
      No,
      $autoDef^Auto,
    };
    }
    }
  )cpp");
  FS.Files[TUSchedulerH] = Header.code().str();

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "TUScheduler.h"
    namespace clang {
    namespace clangd {
    void test() {
      auto x = WantDiagnostics::$autoUse^Auto;
    }
    }
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("autoUse"));
  ASSERT_TRUE(bool(Loc)) << (Loc ? "" : llvm::toString(Loc.takeError()));
  ASSERT_TRUE(Loc && !Loc->empty());
  EXPECT_EQ(Loc->front().Name, "Auto");
  EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("TUScheduler.h"));
  EXPECT_EQ(Loc->front().PreferredDeclaration.range.start, Header.point("autoDef"));
}

TEST(PseudoModuleTest, ClangdNamespaceSignatureHelpGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string ClangdServerH = testPath("ClangdServer.h");
  std::string CodeCompleteH = testPath("CodeComplete.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[ClangdServerH] = R"cpp(
    namespace clang {
    namespace clangd {
    class ClangdServer {
    public:
      void signatureHelp();
    };
    }
    }
  )cpp";

  Annotations CodeComplete(R"cpp(
    namespace clang {
    namespace clangd {
    void $freeSigHelp^signatureHelp(int A, int B);
    }
    }
  )cpp");
  FS.Files[CodeCompleteH] = CodeComplete.code().str();

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    #include "ClangdServer.h"
    #include "CodeComplete.h"
    namespace clang {
    namespace clangd {
    void ClangdServer::signatureHelp() {
      clangd::$sigHelpCall^signatureHelp(1, 2);
    }
    }
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("sigHelpCall"));
  ASSERT_TRUE(bool(Loc)) << (Loc ? "" : llvm::toString(Loc.takeError()));
  ASSERT_TRUE(Loc && !Loc->empty());
  EXPECT_EQ(Loc->front().Name, "signatureHelp");
  EXPECT_TRUE(llvm::StringRef(Loc->front().PreferredDeclaration.uri.file()).ends_with("CodeComplete.h"))
      << "Expected CodeComplete.h, got " << Loc->front().PreferredDeclaration.uri.file();
  EXPECT_EQ(Loc->front().PreferredDeclaration.range.start, CodeComplete.point("freeSigHelp"));
}

TEST(PseudoModuleTest, PrepareRenameParameterNewNameReferences) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string SourceFile = testPath("ClangdServer.cpp");
  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    namespace clang {
    namespace clangd {
    void prepareRename(int $paramNewName^NewName) {
      auto Action = [$capNewName^NewName = $rhsNewName^NewName]() {
        int x = $useNewName^NewName;
      };
    }
    }
    }
  )cpp");

  auto Refs = Mod.findReferences(SourceFile, Code.code(), Code.point("paramNewName"));
  ASSERT_TRUE(bool(Refs)) << (Refs ? "" : llvm::toString(Refs.takeError()));
  EXPECT_FALSE(Refs->References.empty()) << "NewName parameter should have references";

  std::vector<Position> RefPositions;
  for (const auto &R : Refs->References)
    RefPositions.push_back(R.Loc.range.start);

  EXPECT_TRUE(llvm::is_contained(RefPositions, Code.point("paramNewName")));
  EXPECT_TRUE(llvm::is_contained(RefPositions, Code.point("rhsNewName")));
  EXPECT_TRUE(llvm::is_contained(RefPositions, Code.point("capNewName")));
  EXPECT_TRUE(llvm::is_contained(RefPositions, Code.point("useNewName")));
}

TEST(PseudoModuleTest, LambdaInitCaptureSecondFileJumpsToOuter) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string SourceFile = testPath("test.cpp");
  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    struct FileObj {
      void str();
    };
    void codeComplete(FileObj $outerFile^File) {
      auto Task = [$capFile^File = $rhsFile^File.str()]() {
        $useFile^File;
      };
    }
  )cpp");

  // 1. Second File in File = File.str() MUST jump to outerFile
  auto LocRhs = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("rhsFile"));
  ASSERT_TRUE(bool(LocRhs)) << (LocRhs ? "" : llvm::toString(LocRhs.takeError()));
  ASSERT_TRUE(LocRhs && !LocRhs->empty());
  EXPECT_EQ(LocRhs->front().PreferredDeclaration.range.start, Code.point("outerFile"));

  // 2. Use inside body jumps to capFile
  auto LocUse = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("useFile"));
  ASSERT_TRUE(bool(LocUse)) << (LocUse ? "" : llvm::toString(LocUse.takeError()));
  ASSERT_TRUE(LocUse && !LocUse->empty());
  EXPECT_EQ(LocUse->front().PreferredDeclaration.range.start, Code.point("capFile"));
}

TEST(PseudoModuleTest, ShowASTBasicInfo) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();
  std::string SourceFile = testPath("test.cpp");
  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    float root(int *x) {
      return *x + 1;
    }
  )cpp");

  auto ASTRes = Mod.getAST(SourceFile, Code.code());
  ASSERT_TRUE(bool(ASTRes)) << (ASTRes ? "" : llvm::toString(ASTRes.takeError()));
  ASSERT_TRUE(bool(*ASTRes));
  const ASTNode &TU = **ASTRes;
  EXPECT_EQ(TU.role, "declaration");
  EXPECT_EQ(TU.kind, "TranslationUnit");
  ASSERT_FALSE(TU.children.empty());

  const ASTNode &Func = TU.children.front();
  EXPECT_EQ(Func.role, "declaration");
  EXPECT_EQ(Func.kind, "Function");
  EXPECT_EQ(Func.detail, "root");

  ASSERT_GE(Func.children.size(), 2u);
  const ASTNode &Proto = Func.children[0];
  EXPECT_EQ(Proto.role, "type");
  EXPECT_EQ(Proto.kind, "FunctionProto");

  const ASTNode &Body = Func.children[1];
  EXPECT_EQ(Body.role, "statement");
  EXPECT_EQ(Body.kind, "Compound");
  ASSERT_FALSE(Body.children.empty());

  const ASTNode &Ret = Body.children.front();
  EXPECT_EQ(Ret.role, "statement");
  EXPECT_EQ(Ret.kind, "Return");
  ASSERT_FALSE(Ret.children.empty());

  const ASTNode &Op = Ret.children.front();
  EXPECT_EQ(Op.role, "expression");
  EXPECT_EQ(Op.kind, "BinaryOperator");
  EXPECT_EQ(Op.detail, "+");
}

TEST(PseudoModuleTest, ShowASTRangeSelection) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();
  std::string SourceFile = testPath("test.cpp");
  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  Annotations Code(R"cpp(
    float root(int *x) {
      return $[[*x + 1]];
    }
  )cpp");

  auto ASTRes = Mod.getAST(SourceFile, Code.code(), Code.range());
  ASSERT_TRUE(bool(ASTRes)) << (ASTRes ? "" : llvm::toString(ASTRes.takeError()));
  ASSERT_TRUE(bool(*ASTRes));
  const ASTNode &Node = **ASTRes;
  EXPECT_EQ(Node.role, "expression");
  EXPECT_EQ(Node.kind, "BinaryOperator");
  EXPECT_EQ(Node.detail, "+");
}

static std::string findRepoSourceFile(llvm::StringRef RelativePath) {
  if (llvm::sys::fs::exists(RelativePath))
    return RelativePath.str();

  // Search relative to __FILE__
  llvm::SmallString<256> Path(__FILE__);
  llvm::sys::path::remove_filename(Path);
  llvm::sys::path::append(Path, llvm::sys::path::Style::posix,
                          "../../../", RelativePath);
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  if (llvm::sys::fs::exists(Path))
    return std::string(Path.str());

  // Search relative to current working directory moving upwards
  llvm::SmallString<256> Cwd;
  if (!llvm::sys::fs::current_path(Cwd)) {
    for (int Up = 0; Up < 8; ++Up) {
      llvm::SmallString<256> Cand = Cwd;
      llvm::sys::path::append(Cand, RelativePath);
      llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
      if (llvm::sys::fs::exists(Cand))
        return std::string(Cand.str());
      llvm::sys::path::append(Cwd, "..");
    }
  }

  return "";
}

static void validateASTNodesRecursive(const ASTNode &Node, size_t &NodeCount,
                                     std::string &Error) {
  ++NodeCount;

  static const llvm::StringSet<> ValidRoles = {
      "declaration", "statement", "expression", "type",
      "specifier",   "base",      "attribute",  "reference",
      "constructor initializer"};

  if (Node.role.empty()) {
    Error = "Empty role in ASTNode kind=" + Node.kind + " detail=" + Node.detail;
    return;
  }
  if (!ValidRoles.contains(Node.role)) {
    Error = "Invalid role '" + Node.role + "' in ASTNode kind=" + Node.kind;
    return;
  }
  if (Node.kind.empty()) {
    Error = "Empty kind in ASTNode role=" + Node.role + " detail=" + Node.detail;
    return;
  }

  if (Node.role == "type") {
    static const llvm::StringSet<> ValidTypeKinds = {
        "Builtin", "Pointer", "LValueReference", "RValueReference",
        "Qualified", "Record", "FunctionProto", "Auto"};
    if (!ValidTypeKinds.contains(Node.kind)) {
      Error = "Unrecognized type kind '" + Node.kind + "' in type node";
      return;
    }
    if (Node.kind != "FunctionProto" && Node.kind != "Pointer" &&
        Node.kind != "LValueReference" && Node.kind != "RValueReference") {
      if (Node.detail.empty()) {
        Error = "Type node with kind '" + Node.kind + "' has empty detail";
        return;
      }
    }
  }

  if (Node.role == "declaration") {
    if (Node.kind == "Var" || Node.kind == "ParmVar" || Node.kind == "Field" ||
        Node.kind == "Function" || Node.kind == "CXXMethod") {
      bool HasType = llvm::any_of(Node.children, [](const ASTNode &C) {
        return C.role == "type";
      }) || !Node.arcana.empty();
      if (!HasType) {
        Error = "Declaration node kind=" + Node.kind + " detail=" + Node.detail +
                " has no associated type";
        return;
      }
    }
  }

  if (Node.role == "expression") {
    if (Node.kind == "BinaryOperator" || Node.kind == "UnaryOperator") {
      if (Node.detail.empty()) {
        Error = "Operator expr kind=" + Node.kind + " has empty operator detail";
        return;
      }
    }
    if (Node.kind == "IntegerLiteral" || Node.kind == "FloatingLiteral" ||
        Node.kind == "StringLiteral" || Node.kind == "DeclRef" ||
        Node.kind == "Member") {
      if (Node.detail.empty()) {
        Error = "Expr kind=" + Node.kind + " has empty detail";
        return;
      }
    }
  }

  for (const auto &C : Node.children) {
    validateASTNodesRecursive(C, NodeCount, Error);
    if (!Error.empty())
      return;
  }
}

TEST(PseudoModuleTest, ValidateASTSemaAndClangdServer) {
  PseudoModule Mod;
  Mod.setPseudoOnly(true);

  std::vector<std::string> TargetFiles = {
      findRepoSourceFile("clang-tools-extra/clangd/ClangdServer.cpp"),
      findRepoSourceFile("clang/lib/Sema/Sema.cpp"),
  };

  for (const auto &FilePath : TargetFiles) {
    ASSERT_FALSE(FilePath.empty()) << "Could not locate target file";
    auto Buf = llvm::MemoryBuffer::getFile(FilePath);
    ASSERT_TRUE(bool(Buf)) << "Failed to read file " << FilePath;

    auto ASTRes = Mod.getAST(FilePath, (*Buf)->getBuffer());
    ASSERT_TRUE(bool(ASTRes))
        << "Failed to get AST for " << FilePath << ": "
        << (ASTRes ? "" : llvm::toString(ASTRes.takeError()));
    ASSERT_TRUE(bool(*ASTRes)) << "Returned null AST for " << FilePath;

    const ASTNode &TU = **ASTRes;
    EXPECT_EQ(TU.role, "declaration");
    EXPECT_EQ(TU.kind, "TranslationUnit");
    EXPECT_FALSE(TU.children.empty())
        << "Empty children in TranslationUnit for " << FilePath;

    size_t NodeCount = 0;
    std::string ValidationError;
    validateASTNodesRecursive(TU, NodeCount, ValidationError);

    EXPECT_TRUE(ValidationError.empty())
        << "Validation failed in " << FilePath << ": " << ValidationError;
    EXPECT_GT(NodeCount, 100u)
        << "Expected rich AST with >100 nodes, got " << NodeCount;
  }
}

static void checkDeclarationMatch(const ASTNode &Reg, const ASTNode &Pseudo,
                                  llvm::StringRef CodeSnippet) {
  std::vector<const ASTNode *> RegDecls;
  std::vector<const ASTNode *> PseudoDecls;
  std::function<void(const ASTNode &, std::vector<const ASTNode *> &)> CollectDecls =
      [&](const ASTNode &N, std::vector<const ASTNode *> &Out) {
        if (N.role == "declaration" && N.kind != "TranslationUnit" &&
            N.arcana.find("implicit") == std::string::npos)
          Out.push_back(&N);
        for (const auto &C : N.children)
          CollectDecls(C, Out);
      };
  CollectDecls(Reg, RegDecls);
  CollectDecls(Pseudo, PseudoDecls);

  for (const auto *R : RegDecls) {
    auto It = llvm::find_if(PseudoDecls, [&](const ASTNode *P) {
      return P->detail == R->detail && P->kind == R->kind;
    });
    EXPECT_NE(It, PseudoDecls.end())
        << "Expected declaration kind=" << R->kind << " detail='" << R->detail
        << "' from regular AST not found in pseudoparser AST for snippet:\n"
        << CodeSnippet;
  }
}

TEST(PseudoModuleTest, CompareASTWithRegularAST) {
  std::vector<std::string> Snippets = {
      R"cpp(
        struct Base {};
        struct UpdateIndexCallbacks : public Base {
          int x;
        };
      )cpp",
      R"cpp(
        struct Base {};
        class DraftStoreFS : public Base {
          int y;
        };
      )cpp",
      R"cpp(
        struct Metric {
          constexpr Metric(const char*, int, const char*) {}
        };
        static constexpr Metric TweakAvailable("tweak_available", 1, "tweak_id");
      )cpp",
      R"cpp(
        struct Lock { Lock(int); };
        struct Span { Span(const char*); };
        void foo() {
          int m = 0;
          Lock l(m);
          Span s("abc");
        }
      )cpp",
      R"cpp(
        struct Options {};
        struct Server {
          struct Opts {
            operator Options() const;
          };
        };
        Server::Opts::operator Options() const {
          return {};
        }
      )cpp",
      R"cpp(
        class Server {
          Server(int x);
          ~Server();
        };
        Server::Server(int x) {}
        Server::~Server() {}
      )cpp",
      R"cpp(
        struct Context {};
        struct Provider {};
        class Server {
          Context createConfiguredContextProvider(const Provider *P);
        };
        Context Server::createConfiguredContextProvider(const Provider *P) {
          return {};
        }
      )cpp",
      R"cpp(
        void foo() {
          struct Impl {
            int a;
            void operator()() {}
          };
        }
      )cpp",
      R"cpp(
        namespace clang {
        namespace clangd {
        namespace {
        struct Metric {
          constexpr Metric(const char*, int) {}
        };
        static constexpr Metric TweakAvailable("tweak", 1);
        struct UpdateIndexCallbacks {
          int *FIndex;
          UpdateIndexCallbacks(int *FIndex) : FIndex(FIndex) {}
          void onPreambleAST(int Path) {}
        };
        class DraftStoreFS {
        public:
          DraftStoreFS(int Base) : Base(Base) {}
          int viewImpl() const { return 0; }
        private:
          int Base;
        };
        }
        struct Options {
          int AsyncThreadsCount;
          operator int() const { return AsyncThreadsCount; }
        };
        class ClangdServer {
        public:
          ClangdServer(int CDB) {}
          ~ClangdServer() {}
          void addDocument(int File) {}
          int createConfiguredContextProvider(int Provider) {
            struct Impl {
              int Provider;
              Impl(int Provider) : Provider(Provider) {}
              int operator()(int File) {
                int Params = 0;
                return Params;
              }
            };
            return 0;
          }
        };
        int tweakSelection(int Sel) { return Sel; }
        }
        }
      )cpp",
  };

  PseudoModule Mod;
  Mod.setPseudoOnly(true);

  for (const auto &Code : Snippets) {
    TestTU TU;
    TU.Code = Code;
    auto AST = TU.build();
    auto RegNode = clang::clangd::dumpAST(
        DynTypedNode::create(*AST.getASTContext().getTranslationUnitDecl()),
        AST.getTokens(), AST.getASTContext());

    auto PseudoRes = Mod.getAST("test.cpp", Code);
    ASSERT_TRUE(bool(PseudoRes));
    ASSERT_TRUE(bool(*PseudoRes));
    checkDeclarationMatch(RegNode, **PseudoRes, Code);
  }
}

TEST(PseudoModuleTest, ClangdServerValidateAllNodeTypes) {
  PseudoModule Mod;
  Mod.setPseudoOnly(true);
  std::string FilePath =
      findRepoSourceFile("clang-tools-extra/clangd/ClangdServer.cpp");
  ASSERT_FALSE(FilePath.empty()) << "Could not locate ClangdServer.cpp";
  auto Buf = llvm::MemoryBuffer::getFile(FilePath);
  ASSERT_TRUE(bool(Buf));
  auto ASTRes = Mod.getAST(FilePath, (*Buf)->getBuffer());
  ASSERT_TRUE(bool(ASTRes));
  ASSERT_TRUE(bool(*ASTRes));
  const ASTNode &TU = **ASTRes;

  std::vector<const ASTNode *> AllDecls;
  std::function<void(const ASTNode &)> CollectAllDecls = [&](const ASTNode &N) {
    if (N.role == "declaration" && N.kind != "TranslationUnit")
      AllDecls.push_back(&N);
    for (const auto &C : N.children)
      CollectAllDecls(C);
  };
  CollectAllDecls(TU);

  auto FindDecl = [&](llvm::StringRef Kind, llvm::StringRef Detail) -> const ASTNode * {
    for (const auto *D : AllDecls) {
      if (D->kind == Kind && D->detail == Detail)
        return D;
    }
    return nullptr;
  };

  // Verify struct/class records
  EXPECT_NE(FindDecl("CXXRecord", "UpdateIndexCallbacks"), nullptr);
  EXPECT_NE(FindDecl("CXXRecord", "DraftStoreFS"), nullptr);
  EXPECT_NE(FindDecl("CXXRecord", "Impl"), nullptr);

  // Verify constructors and destructors
  EXPECT_NE(FindDecl("CXXConstructor", ""), nullptr);
  EXPECT_NE(FindDecl("CXXDestructor", ""), nullptr);

  // Verify methods and conversion operators
  EXPECT_NE(FindDecl("CXXConversion", "operator Options"), nullptr);
  EXPECT_NE(FindDecl("CXXMethod", "createConfiguredContextProvider"), nullptr);
  EXPECT_NE(FindDecl("CXXMethod", "operator()"), nullptr);
  EXPECT_NE(FindDecl("CXXMethod", "optsForTest"), nullptr);

  // Verify free functions
  EXPECT_NE(FindDecl("Function", "tweakSelection"), nullptr);
  EXPECT_NE(FindDecl("Function", "tryConvertToRename"), nullptr);

  // Verify variables
  EXPECT_NE(FindDecl("Var", "TweakAvailable"), nullptr);
  EXPECT_NE(FindDecl("Var", "TweakAttempt"), nullptr);
  EXPECT_NE(FindDecl("Var", "TweakFailed"), nullptr);
  EXPECT_NE(FindDecl("Var", "Ctx"), nullptr);

  // Verify fields inside classes
  static const char *const ExpectedFields[] = {
      "FIndex", "ServerCallbacks", "TFS", "Stdlib", "Tasks",
      "CollectInactiveRegions", "Base", "DirtyFiles", "Provider",
      "Publish", "PublishMu"};
  for (const char *F : ExpectedFields) {
    EXPECT_NE(FindDecl("Field", F), nullptr)
        << "Expected field '" << F << "' not found in AST";
  }

  // Verify exact counts of record, function, conversion, and field declarations
  size_t RecordCount = 0;
  size_t FunctionCount = 0;
  size_t ConversionCount = 0;
  size_t FieldCount = 0;
  for (const auto *D : AllDecls) {
    if (D->kind == "CXXRecord")
      ++RecordCount;
    else if (D->kind == "Function")
      ++FunctionCount;
    else if (D->kind == "CXXConversion")
      ++ConversionCount;
    else if (D->kind == "Field")
      ++FieldCount;
  }
  EXPECT_EQ(RecordCount, 3u);
  EXPECT_EQ(FunctionCount, 2u);
  EXPECT_EQ(ConversionCount, 1u);
  EXPECT_EQ(FieldCount, 11u);

  // Validate inner nodes and ranges across the entire ClangdServer.cpp AST
  size_t TotalNodes = 0;
  size_t ValidRanges = 0;
  std::map<std::string, size_t> RoleCounts;
  std::map<std::string, size_t> KindCounts;
  std::vector<std::string> RangeErrors;

  std::function<void(const ASTNode &, const ASTNode *)> CheckAllNodes =
      [&](const ASTNode &N, const ASTNode *Parent) {
        ++TotalNodes;
        RoleCounts[N.role]++;
        KindCounts[N.kind]++;

        if (!N.range) {
          if (N.kind != "TranslationUnit")
            RangeErrors.push_back(N.role + "/" + N.kind + " '" + N.detail + "': missing range");
        } else {
          Position S = N.range->start;
          Position E = N.range->end;
          if (S.line > E.line || (S.line == E.line && S.character > E.character)) {
            RangeErrors.push_back(N.role + "/" + N.kind + " '" + N.detail + "': inverted range (" +
                                  std::to_string(S.line) + ":" + std::to_string(S.character) + " - " +
                                  std::to_string(E.line) + ":" + std::to_string(E.character) + ")");
          } else {
            ++ValidRanges;
          }
        }

        if (Parent && Parent->range && N.range) {
          Position PS = Parent->range->start;
          Position PE = Parent->range->end;
          Position CS = N.range->start;
          Position CE = N.range->end;
          bool StartValid = (PS.line < CS.line) || (PS.line == CS.line && PS.character <= CS.character);
          bool EndValid = (CE.line < PE.line) || (CE.line == PE.line && CE.character <= PE.character);
          if (!StartValid || !EndValid) {
            RangeErrors.push_back(
                "Containment violation: parent " + Parent->role + "/" + Parent->kind +
                " (" + std::to_string(PS.line) + ":" + std::to_string(PS.character) + " - " +
                std::to_string(PE.line) + ":" + std::to_string(PE.character) +
                ") does not contain child " + N.role + "/" + N.kind + " '" + N.detail + "' (" +
                std::to_string(CS.line) + ":" + std::to_string(CS.character) + " - " +
                std::to_string(CE.line) + ":" + std::to_string(CE.character) + ")");
          }
        }

        for (const auto &C : N.children)
          CheckAllNodes(C, &N);
      };
  CheckAllNodes(TU, nullptr);

  for (const auto &Err : RangeErrors) {
    ADD_FAILURE() << "Range error: " << Err;
  }

  // Ensure inner statement and expression nodes are present in significant numbers
  EXPECT_GT(RoleCounts["statement"], 50u)
      << "Expected >50 statements in ClangdServer.cpp, got " << RoleCounts["statement"];
  EXPECT_GT(RoleCounts["expression"], 100u)
      << "Expected >100 expressions in ClangdServer.cpp, got " << RoleCounts["expression"];
  EXPECT_GT(KindCounts["Compound"], 20u);
  EXPECT_GT(KindCounts["Call"], 30u);
  EXPECT_GT(KindCounts["Member"], 20u);
  EXPECT_GT(KindCounts["If"], 10u);
  EXPECT_GT(KindCounts["Return"], 10u);

  // Validate specific inner nodes and range selections
  std::vector<const ASTNode *> AllMemberInits;
  std::vector<const ASTNode *> AllParmVars;
  std::vector<const ASTNode *> AllCalls;
  std::vector<const ASTNode *> AllMembers;
  std::function<void(const ASTNode &)> CollectAllInner = [&](const ASTNode &N) {
    if (N.role == "constructor initializer" && N.kind == "MemberInitializer")
      AllMemberInits.push_back(&N);
    else if (N.role == "declaration" && N.kind == "ParmVar")
      AllParmVars.push_back(&N);
    else if (N.role == "expression" && N.kind == "Call")
      AllCalls.push_back(&N);
    else if (N.role == "expression" && N.kind == "Member")
      AllMembers.push_back(&N);
    for (const auto &C : N.children)
      CollectAllInner(C);
  };
  CollectAllInner(TU);

  // Verify constructor initializers
  auto FindInit = [&](llvm::StringRef Name) -> const ASTNode * {
    for (const auto *Init : AllMemberInits) {
      if (Init->detail == Name)
        return Init;
    }
    return nullptr;
  };
  EXPECT_NE(FindInit("FIndex"), nullptr);
  EXPECT_NE(FindInit("ServerCallbacks"), nullptr);
  EXPECT_NE(FindInit("TFS"), nullptr);
  EXPECT_NE(FindInit("Tasks"), nullptr);
  EXPECT_NE(FindInit("CollectInactiveRegions"), nullptr);
  EXPECT_NE(FindInit("Base"), nullptr);
  EXPECT_NE(FindInit("DirtyFiles"), nullptr);

  const auto *BaseInit = FindInit("Base");
  ASSERT_NE(BaseInit, nullptr);
  ASSERT_TRUE(BaseInit->range.has_value());
  auto SubInit = Mod.getAST(FilePath, (*Buf)->getBuffer(), *BaseInit->range);
  ASSERT_TRUE(bool(SubInit));
  ASSERT_TRUE(bool(*SubInit));
  EXPECT_EQ((*SubInit)->kind, "MemberInitializer");
  EXPECT_EQ((*SubInit)->detail, "Base");

  // Verify inner member calls and range selection
  auto FindCall = [&](llvm::StringRef CalleeName) -> const ASTNode * {
    for (const auto *C : AllCalls) {
      for (const auto &Child : C->children) {
        if (Child.kind == "Member" && Child.detail == CalleeName)
          return C;
      }
    }
    return nullptr;
  };
  EXPECT_NE(FindCall("getPreprocessor"), nullptr);
  EXPECT_NE(FindCall("getCompilerInvocation"), nullptr);
  EXPECT_NE(FindCall("updatePreamble"), nullptr);

  const auto *CallPP = FindCall("getPreprocessor");
  ASSERT_NE(CallPP, nullptr);
  ASSERT_TRUE(CallPP->range.has_value());
  auto SubCall = Mod.getAST(FilePath, (*Buf)->getBuffer(), *CallPP->range);
  ASSERT_TRUE(bool(SubCall));
  ASSERT_TRUE(bool(*SubCall));
  EXPECT_EQ((*SubCall)->kind, "Call");

  // Verify condition variable Loc
  const auto *LocDecl = FindDecl("Var", "Loc");
  ASSERT_NE(LocDecl, nullptr);
  ASSERT_TRUE(LocDecl->range.has_value());
  auto SubLoc = Mod.getAST(FilePath, (*Buf)->getBuffer(), *LocDecl->range);
  ASSERT_TRUE(bool(SubLoc));
  ASSERT_TRUE(bool(*SubLoc));
  EXPECT_EQ((*SubLoc)->kind, "Var");
  EXPECT_EQ((*SubLoc)->detail, "Loc");
}

// Verifies that Go-To-Definition on `initialize` in `Mod.initialize(F)`
// correctly resolves through the range-for auto variable `Mod` (deduced from
// `*Opts.FeatureModules` of type `FeatureModuleSet *`) to reach
// `FeatureModule::initialize(const Facilities &F)` in the included header.
TEST(PseudoModuleTest, RangeForAutoTypeResolutionGTD) {
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  std::string HeaderFile = testPath("FeatureModule.h");
  std::string SourceFile = testPath("ClangdServer.cpp");

  FS.Files[HeaderFile] = R"cpp(
    namespace clang {
    namespace clangd {

    class FeatureModule {
    public:
      struct Facilities {
        int Scheduler;
      };
      void initialize(const Facilities &F);
      virtual ~FeatureModule();
    };

    class FeatureModuleSet {
    public:
      FeatureModule *begin();
      FeatureModule *end();
    };

    } // namespace clangd
    } // namespace clang
  )cpp";

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(CDB.get());

  // Simulate the ClangdServer.cpp pattern:
  //   for (auto &Mod : *Opts.FeatureModules)
  //     Mod.initialize(F);
  // GTD on `initialize` should resolve to FeatureModule::initialize in header.
  Annotations Code(R"cpp(
    #include "FeatureModule.h"

    namespace clang {
    namespace clangd {

    struct Options {
      FeatureModuleSet *FeatureModules = nullptr;
    };

    void foo(Options &Opts) {
      FeatureModule::Facilities F{};
      for (auto &$var[[Mod]] : *Opts.FeatureModules)
        Mod.$call^initialize(F);
    }

    } // namespace clangd
    } // namespace clang
  )cpp");

  // GTD on `initialize` call
  auto Loc = Mod.locateSymbolAt(SourceFile, Code.code(), Code.point("call"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  EXPECT_EQ(Loc->size(), 1u) << "Expected 1 location, got " << Loc->size();
  if (!Loc->empty()) {
    EXPECT_EQ((*Loc)[0].Name, "initialize")
        << "Expected 'initialize', got: " << (*Loc)[0].Name;
    // Should resolve to the definition in FeatureModule.h
    EXPECT_TRUE((*Loc)[0].PreferredDeclaration.uri.file().ends_with("FeatureModule.h"))
        << "Expected FeatureModule.h, got: "
        << (*Loc)[0].PreferredDeclaration.uri.file();
  }

  // GTD on the range-for variable `Mod` itself should jump back to its declaration
  auto LocVar = Mod.locateSymbolAt(SourceFile, Code.code(),
                                   Code.range("var").start);
  ASSERT_TRUE(bool(LocVar)) << llvm::toString(LocVar.takeError());
  if (!LocVar->empty()) {
    EXPECT_EQ((*LocVar)[0].Name, "Mod");
  }
}

TEST(PseudoModuleTest, DiagnosticsCleanCode) {
  PseudoModule Mod;
  llvm::StringRef Code = R"cpp(
    int add(int a, int b) {
      return a + b;
    }
    class Calculator {
    public:
      int multiply(int x, int y) {
        return x * y;
      }
    };
  )cpp";

  auto Diags = Mod.getDiagnostics(Code);
  ASSERT_TRUE(bool(Diags)) << llvm::toString(Diags.takeError());
  EXPECT_TRUE(Diags->empty()) << "Clean code should produce no diagnostics, got: "
                              << Diags->size();
}

TEST(PseudoModuleTest, DiagnosticsMissingBrace) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void broken() $[[{]]
      int x = 10;
  )cpp");

  auto Diags = Mod.getDiagnostics(Code.code());
  ASSERT_TRUE(bool(Diags)) << llvm::toString(Diags.takeError());
  ASSERT_FALSE(Diags->empty());
  bool FoundMissingBrace = false;
  for (const auto &D : *Diags) {
    if (D.code == "missing-brace") {
      FoundMissingBrace = true;
      EXPECT_EQ(D.severity, 1); // Error
      EXPECT_EQ(D.source, "pseudo-parser");
      EXPECT_EQ(D.range, Code.range());
    }
  }
  EXPECT_TRUE(FoundMissingBrace);
}

TEST(PseudoModuleTest, DiagnosticsUnmatchedClosingBrace) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void func() {
      int x = 1;
    }
    $[[}]]
  )cpp");

  auto Diags = Mod.getDiagnostics(Code.code());
  ASSERT_TRUE(bool(Diags)) << llvm::toString(Diags.takeError());
  ASSERT_FALSE(Diags->empty());
  bool FoundUnmatchedBrace = false;
  for (const auto &D : *Diags) {
    if (D.code == "unmatched-brace") {
      FoundUnmatchedBrace = true;
      EXPECT_EQ(D.severity, 1); // Error
      EXPECT_EQ(D.source, "pseudo-parser");
      EXPECT_EQ(D.range, Code.range());
    }
  }
  EXPECT_TRUE(FoundUnmatchedBrace);
}

TEST(PseudoModuleTest, DiagnosticsMissingParenAndBracket) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void test() {
      int arr$bracket[[[]]10;
      if $paren[[(]]true {
      }
    }
  )cpp");

  auto Diags = Mod.getDiagnostics(Code.code());
  ASSERT_TRUE(bool(Diags)) << llvm::toString(Diags.takeError());
  ASSERT_FALSE(Diags->empty());
  bool FoundMissingBracket = false;
  bool FoundMissingParen = false;
  for (const auto &D : *Diags) {
    if (D.code == "missing-bracket") {
      FoundMissingBracket = true;
      EXPECT_EQ(D.severity, 1);
      EXPECT_EQ(D.range, Code.range("bracket"));
    }
    if (D.code == "missing-paren") {
      FoundMissingParen = true;
      EXPECT_EQ(D.severity, 1);
      EXPECT_EQ(D.range, Code.range("paren"));
    }
  }
  EXPECT_TRUE(FoundMissingBracket);
  EXPECT_TRUE(FoundMissingParen);
}

TEST(PseudoModuleTest, DiagnosticsGrammarError) {
  PseudoModule Mod;
  llvm::StringRef Code = "class { invalid c++ syntax here @@@@ ;;; };";

  auto Diags = Mod.getDiagnostics(Code);
  ASSERT_TRUE(bool(Diags)) << llvm::toString(Diags.takeError());
  EXPECT_FALSE(Diags->empty());
  bool FoundGrammarOrSyntaxError = false;
  for (const auto &D : *Diags) {
    if (D.code == "grammar-error" || D.code == "syntax-error") {
      FoundGrammarOrSyntaxError = true;
      EXPECT_EQ(D.severity, 1);
      EXPECT_EQ(D.source, "pseudo-parser");
    }
  }
  EXPECT_TRUE(FoundGrammarOrSyntaxError);
}

TEST(PseudoModuleTest, DocumentDraftLifecycleAndDiagnostics) {
  PseudoModule Mod;
  Mod.setPseudoOnly(true);

  std::string File = testPath("draft_test.cpp");

  // 1. Initially no draft -> getDocument returns empty
  EXPECT_EQ(Mod.getDocument(File), "");

  // 2. onDocumentDidOpen
  DidOpenTextDocumentParams OpenParams;
  OpenParams.textDocument.uri = URIForFile::canonicalize(File, File);
  OpenParams.textDocument.version = 1;
  OpenParams.textDocument.text = "int a = 1;";
  Mod.onDocumentDidOpen(OpenParams);

  EXPECT_EQ(Mod.getDocument(File), "int a = 1;");

  // 3. onDocumentDidChange (incremental change)
  DidChangeTextDocumentParams ChangeParams;
  ChangeParams.textDocument.uri = URIForFile::canonicalize(File, File);
  ChangeParams.textDocument.version = 2;
  TextDocumentContentChangeEvent Change;
  Change.range = Range{Position{0, 8}, Position{0, 9}}; // replace '1' with '42'
  Change.text = "42";
  ChangeParams.contentChanges.push_back(Change);
  Mod.onDocumentDidChange(ChangeParams);

  EXPECT_EQ(Mod.getDocument(File), "int a = 42;");

  // 4. onDocumentDidClose
  DidCloseTextDocumentParams CloseParams;
  CloseParams.textDocument.uri = URIForFile::canonicalize(File, File);
  Mod.onDocumentDidClose(CloseParams);

  EXPECT_EQ(Mod.getDocument(File), "");
}

TEST(PseudoModuleTest, DiagnosticsConditionalBranchMissingBrace) {
  PseudoModule Mod;
  llvm::StringRef Code = "#ifdef X\nvoid f(){\n#else\nvoid g()\n#endif\n}\n";

  auto Diags = Mod.getDiagnostics(Code);
  ASSERT_TRUE(bool(Diags)) << llvm::toString(Diags.takeError());
  EXPECT_FALSE(Diags->empty());

  bool FoundMissingBraceInElse = false;
  bool FoundUnmatchedCloseBrace = false;

  for (const auto &D : *Diags) {
    if (D.code == "missing-brace" &&
        D.message.find("conditional branch") != std::string::npos) {
      FoundMissingBraceInElse = true;
      EXPECT_EQ(D.severity, 1);
    }
    if (D.code == "unmatched-brace") {
      FoundUnmatchedCloseBrace = true;
      EXPECT_EQ(D.severity, 1);
    }
  }

  EXPECT_TRUE(FoundMissingBraceInElse);
  EXPECT_TRUE(FoundUnmatchedCloseBrace);
}

TEST(PseudoModuleTest, DiagnosticsConditionalBranchClean) {
  PseudoModule Mod;
  llvm::StringRef Code = "#ifdef X\nvoid f(){\n#else\nvoid g(){\n#endif\n}\n";

  auto Diags = Mod.getDiagnostics(Code);
  ASSERT_TRUE(bool(Diags)) << llvm::toString(Diags.takeError());
  EXPECT_TRUE(Diags->empty())
      << "Expected 0 diagnostics, got: " << Diags->size();
}

TEST(PseudoModuleTest, EmptyAndWhitespaceFileHandling) {
  PseudoModule Mod;
  std::string File = testPath("empty_test.cpp");

  std::vector<std::string> TestInputs = {
      "",
      "   \n\t\r\n   ",
      "// Just a single comment line\n",
      "/* Multi-line comment\n * without any code\n */\n",
  };

  for (const auto &Code : TestInputs) {
    // 1. Diagnostics: must succeed with 0 diagnostics
    auto Diags = Mod.getDiagnostics(Code);
    ASSERT_TRUE(bool(Diags))
        << "Failed for input [" << Code
        << "]: " << llvm::toString(Diags.takeError());
    EXPECT_TRUE(Diags->empty())
        << "Diagnostics not empty for input [" << Code << "]";

    // 2. AST: must return a valid TranslationUnit node
    auto AST = Mod.getAST(File, Code);
    ASSERT_TRUE(bool(AST))
        << "Failed for input [" << Code
        << "]: " << llvm::toString(AST.takeError());
    ASSERT_TRUE(AST->has_value());
    EXPECT_EQ((*AST)->kind, "TranslationUnit");
    EXPECT_TRUE((*AST)->children.empty());

    // 3. DocumentSymbols: must succeed and be empty
    auto Syms = Mod.getDocumentSymbols(Code);
    ASSERT_TRUE(bool(Syms))
        << "Failed for input [" << Code
        << "]: " << llvm::toString(Syms.takeError());
    EXPECT_TRUE(Syms->empty());

    // 4. SemanticTokens: must succeed and be empty
    auto SemTokens = Mod.getSemanticTokens(Code);
    ASSERT_TRUE(bool(SemTokens))
        << "Failed for input [" << Code
        << "]: " << llvm::toString(SemTokens.takeError());
    EXPECT_TRUE(SemTokens->tokens.empty());

    // 5. FoldingRanges: must succeed and be empty
    auto Folding = Mod.getFoldingRanges(Code, /*LineFoldingOnly=*/false);
    ASSERT_TRUE(bool(Folding))
        << "Failed for input [" << Code
        << "]: " << llvm::toString(Folding.takeError());
    EXPECT_TRUE(Folding->empty());

    // 6. Hover: must return std::nullopt without error
    auto H = Mod.getHover(File, Code, Position{0, 0});
    ASSERT_TRUE(bool(H))
        << "Failed for input [" << Code
        << "]: " << llvm::toString(H.takeError());
    EXPECT_FALSE(H->has_value());

    // 7. LocateSymbolAt: must return empty vector without error
    auto Loc = Mod.locateSymbolAt(File, Code, Position{0, 0});
    ASSERT_TRUE(bool(Loc))
        << "Failed for input [" << Code
        << "]: " << llvm::toString(Loc.takeError());
    EXPECT_TRUE(Loc->empty());
  }
}

TEST(PseudoModuleTest, ConditionalBranchesCrashReproducer) {
  PseudoModule Mod;
  llvm::StringRef Code =
      "#ifdef X\nint f() {\n#elif Y\nint g()\n#else\nint ff(*\n#endif\n}";
  std::string File = testPath("test.cpp");
  auto Diags = Mod.getDiagnostics(File, Code);
  ASSERT_TRUE(bool(Diags));
  EXPECT_FALSE(Diags->empty());

  bool FoundMissingBrace = false;
  bool FoundMissingParen = false;
  for (const auto &D : *Diags) {
    if (D.code == "missing-brace")
      FoundMissingBrace = true;
    if (D.code == "missing-paren")
      FoundMissingParen = true;
  }
  EXPECT_TRUE(FoundMissingBrace);
  EXPECT_TRUE(FoundMissingParen);

  auto AST = Mod.getAST(File, Code);
  ASSERT_TRUE(bool(AST));
  auto Syms = Mod.getDocumentSymbols(Code);
  ASSERT_TRUE(bool(Syms));
  auto SemTokens = Mod.getSemanticTokens(Code);
  ASSERT_TRUE(bool(SemTokens));
  auto Folding = Mod.getFoldingRanges(Code, false);
  ASSERT_TRUE(bool(Folding));

  for (int Line = 0; Line < 8; ++Line) {
    for (int Col = 0; Col < 10; ++Col) {
      auto H = Mod.getHover(File, Code, Position{Line, Col});
      ASSERT_TRUE(bool(H));
      auto Loc = Mod.locateSymbolAt(File, Code, Position{Line, Col});
      ASSERT_TRUE(bool(Loc));
      auto Refs = Mod.findReferences(File, Code, Position{Line, Col}, 100);
      ASSERT_TRUE(bool(Refs));
    }
  }
}

TEST(PseudoModuleTest, PreprocessorConditionDiagnostics) {
  PseudoModule Mod;

  // 1. Missing #endif
  {
    llvm::StringRef Code = "#ifdef X\nint a;\n";
    auto Diags = Mod.getDiagnostics("test.cpp", Code);
    ASSERT_TRUE(bool(Diags));
    ASSERT_FALSE(Diags->empty());
    bool FoundMissingEndif = false;
    for (const auto &D : *Diags) {
      if (D.code == "missing-endif") {
        FoundMissingEndif = true;
        EXPECT_EQ(D.range.start.line, 0);
        EXPECT_NE(D.message.find("unterminated '#ifdef'"), std::string::npos);
      }
    }
    EXPECT_TRUE(FoundMissingEndif);
  }

  // 2. Unmatched #endif without #if
  {
    llvm::StringRef Code = "int a;\n#endif\n";
    auto Diags = Mod.getDiagnostics("test.cpp", Code);
    ASSERT_TRUE(bool(Diags));
    ASSERT_FALSE(Diags->empty());
    bool FoundUnmatchedEndif = false;
    for (const auto &D : *Diags) {
      if (D.code == "unmatched-endif") {
        FoundUnmatchedEndif = true;
        EXPECT_EQ(D.range.start.line, 1);
        EXPECT_NE(D.message.find("'#endif' without '#if'"), std::string::npos);
      }
    }
    EXPECT_TRUE(FoundUnmatchedEndif);
  }

  // 3. Unmatched #else and #elif without #if
  {
    llvm::StringRef Code = "int a;\n#else\nint b;\n#elif C\nint c;\n";
    auto Diags = Mod.getDiagnostics("test.cpp", Code);
    ASSERT_TRUE(bool(Diags));
    ASSERT_FALSE(Diags->empty());
    bool FoundElse = false, FoundElif = false;
    for (const auto &D : *Diags) {
      if (D.code == "unmatched-else")
        FoundElse = true;
      if (D.code == "unmatched-elif")
        FoundElif = true;
    }
    EXPECT_TRUE(FoundElse);
    EXPECT_TRUE(FoundElif);
  }

  // 4. #else after #else
  {
    llvm::StringRef Code = "#if 1\nint a;\n#else\nint b;\n#else\nint c;\n#endif\n";
    auto Diags = Mod.getDiagnostics("test.cpp", Code);
    ASSERT_TRUE(bool(Diags));
    ASSERT_FALSE(Diags->empty());
    bool FoundUnexpected = false;
    for (const auto &D : *Diags) {
      if (D.code == "unexpected-directive") {
        FoundUnexpected = true;
        EXPECT_NE(D.message.find("'#else' after '#else'"), std::string::npos);
      }
    }
    EXPECT_TRUE(FoundUnexpected);
  }

  // 5. Nested unclosed conditional
  {
    llvm::StringRef Code = "#ifdef OUTER\n#ifdef INNER\nint a;\n#endif\n";
    auto Diags = Mod.getDiagnostics("test.cpp", Code);
    ASSERT_TRUE(bool(Diags));
    ASSERT_FALSE(Diags->empty());
    bool FoundMissingEndif = false;
    for (const auto &D : *Diags) {
      if (D.code == "missing-endif") {
        FoundMissingEndif = true;
        EXPECT_EQ(D.range.start.line, 0);
      }
    }
    EXPECT_TRUE(FoundMissingEndif);
  }
}

TEST(PseudoModuleTest, TrailingBracesGrammarError) {
  PseudoModule Mod;
  llvm::StringRef Code = "int f() {} {}";
  std::string File = testPath("test.cpp");
  auto Diags = Mod.getDiagnostics(File, Code);
  ASSERT_TRUE(bool(Diags));
  ASSERT_FALSE(Diags->empty());

  for (const auto &D : *Diags) {
    EXPECT_NE(D.message, "syntax error: failed to parse translation unit");
    // Must point specifically to the stray `{}` around column 11, not column 0!
    EXPECT_GE(D.range.start.character, 10);
  }

  // AST must still parse int f() {}
  auto AST = Mod.getAST(File, Code);
  ASSERT_TRUE(bool(AST));
  ASSERT_TRUE(AST->has_value());
  EXPECT_EQ((*AST)->kind, "TranslationUnit");
  EXPECT_FALSE((*AST)->children.empty());
  bool FoundF = false;
  for (const auto &Child : (*AST)->children) {
    if (Child.detail == "f")
      FoundF = true;
  }
  EXPECT_TRUE(FoundF);

  // DocumentSymbols must still return f
  auto Syms = Mod.getDocumentSymbols(Code);
  ASSERT_TRUE(bool(Syms));
  ASSERT_FALSE(Syms->empty());
  EXPECT_EQ((*Syms)[0].name, "f");
}

} // namespace


} // namespace clangd
} // namespace clang
