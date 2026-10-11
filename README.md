# Clangd Pseudo-Parser

[![CI](https://github.com/Last-Humans-Coding-Under-White-Nights/pseudo-parser/actions/workflows/ci.yml/badge.svg)](https://github.com/Last-Humans-Coding-Under-White-Nights/pseudo-parser/actions/workflows/ci.yml)

**Clangd Pseudo-Parser** is a C++ GLR pseudo-parser and dynamic feature module for [clangd](https://clangd.llvm.org/). It runs without requiring complete compilation flags, complex build graphs, or resolved include hierarchies.

Its primary purpose is to provide **instant, high-performance syntax and preprocessor diagnostics** when files are opened or edited in your editor.

---

## Key Features

- **Fast Opening Diagnostics**: Emits diagnostics immediately upon `textDocument/didOpen` and `textDocument/didChange` using a GLR parser rather than waiting for full Clang AST builds or header resolution.
- **Dynamic Plugin Architecture**: Runs against **unmodified original upstream clangd** using the standard `-load` command-line argument. No forks or custom patches to clangd required.
- **Error Recovery & Multi-Branch Support**: Resilient to broken code, missing braces/brackets, incomplete statements, and preprocessor conditionals (`#ifdef`, `#elif`, `#else`, `#endif`).
- **Cross-Platform**: Built and tested for Linux (x86_64) and macOS (Apple Silicon arm64 & Intel x86_64).

---

## Quick Start: Using with Clangd

### 1. Download or Build the Plugin

Download the prebuilt shared library for your platform from [Releases](https://github.com/Last-Humans-Coding-Under-White-Nights/pseudo-parser/releases):
- **Linux (x86_64)**: `libclangd_pseudo.so`
- **macOS**: `libclangd_pseudo.dylib`

### 2. Configure Your Editor / LSP Client

Add `--load=<path-to-plugin>` to your clangd server arguments:

```bash
clangd --load=/path/to/libclangd_pseudo.so
```

#### Example: VS Code (`settings.json`)
```json
{
  "clangd.arguments": [
    "--load=/path/to/libclangd_pseudo.so"
  ]
}
```

#### Example: Neovim (`lspconfig`)
```lua
require('lspconfig').clangd.setup({
  cmd = { "clangd", "--load=/path/to/libclangd_pseudo.so" },
})
```

#### Example: CLI Check Mode
```bash
clangd -enable-config=0 -check=file.cpp -load=/path/to/libclangd_pseudo.so
```

---

## How It Works

1. Upstream `clangd` (on `main`) supports loading dynamic plugins via `-load`.
2. When loaded, `libclangd_pseudo` registers itself into `clangd::FeatureModuleRegistry`.
3. Upon LSP client initialization, the module intercepts document opening (`textDocument/didOpen`) and edits (`textDocument/didChange`).
4. It parses the code with the C++ GLR pseudo-parser and publishes immediate syntax, bracket, and preprocessor diagnostics via `textDocument/publishDiagnostics`.

---

## Building from Source

### Prerequisites

- CMake >= 3.20
- Ninja
- C++17 compatible compiler (Clang or GCC)
- LLVM and Clang (from `main` branch with plugin support enabled)

### Build Instructions

```bash
# 1. Clone this repository
git clone https://github.com/Last-Humans-Coding-Under-White-Nights/pseudo-parser.git
cd pseudo-parser

# 2. Configure with CMake pointing to your LLVM build
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/path/to/llvm-build/lib/cmake/llvm \
  -DClang_DIR=/path/to/llvm-build/lib/cmake/clang \
  -DCLANGD_INCLUDE_DIRS=/path/to/llvm-project/clang-tools-extra/clangd \
  -DPSEUDO_ENABLE_TESTS=ON

# 3. Build the dynamic feature module and the CLI tool
ninja -C build clangd_pseudo pseudo-tool

# 4. Build and run every test: lit regression tests, ClangPseudoTests and
#    PseudoModuleTests (the gtest binaries run through the lit shim in test/Unit)
ninja -C build check-clang-pseudo
```

### Test dependencies

The tests need gtest, LLVM's testing libraries, `lit`, `FileCheck`, `not`,
`count`, and clangd's test scaffolding. Installed LLVM packages do not ship
most of these, so the build compiles them from an `llvm-project` source tree
that matches the LLVM you build against. That tree is found automatically
when `CLANGD_INCLUDE_DIRS` points inside one (as in the example above);
otherwise pass `-DPSEUDO_LLVM_SOURCE_DIR=/path/to/llvm-project`. `FileCheck`,
`not` and `count` are taken from the LLVM `bin/` directory, and the fuzzer
links `LLVMFuzzerCLI`, so build them in your LLVM tree if they are missing
(`ninja -C llvm-build FileCheck not count LLVMFuzzerCLI`).

`PseudoModuleTests` additionally needs the `clangDaemon` library, which an
LLVM build tree that built `clangd` exports, and clangd's test scaffolding
from `clang-tools-extra/clangd/unittests`. When either is missing, that one
target is skipped with a status message and the other tests still build.

`PSEUDO_ENABLE_TESTS` defaults to ON when these dependencies are found and
to OFF otherwise, so a plain configure always builds the plugin and the tool.
With tests ON, a missing dependency is a configure error rather than a
silent skip.

Optional targets: `-DPSEUDO_ENABLE_FUZZER=ON` builds `clang-pseudo-fuzzer`
without the tests (the lit suite always builds it);
`-DPSEUDO_ENABLE_BENCHMARKS=ON` builds `ClangPseudoBenchmark` and requires
LLVM's `benchmark` target (`LLVM_INCLUDE_BENCHMARKS=ON`).

---

## Repository Structure

```
pseudo-parser/
├── include/              # Public headers for clang-pseudo (GLR, Grammar, Token, CXX)
├── lib/                  # Core pseudo-parser implementation and grammar tables
├── gen/                  # clang-pseudo-gen code generator
├── tool/                 # Standalone clang-pseudo CLI tool and HTML forest browser
├── clangd/               # Clangd FeatureModule implementation (PseudoModule)
├── unittests/            # gtest suites (core parser and PseudoModule)
├── test/                 # lit regression tests
├── fuzzer/               # clang-pseudo-fuzzer
├── benchmarks/           # Parser benchmark (opt-in)
├── cmake/                # Build helpers (test dependency fallback)
├── docs/                 # Design documents and grammar disambiguation notes
└── .github/workflows/    # CI pipeline for Linux and macOS
```

---

## License

This project is part of the LLVM Project and is licensed under the Apache License v2.0 with LLVM Exceptions. See [LICENSE](LICENSE) for details.
