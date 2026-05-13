cmake_minimum_required(VERSION 3.20)
project(lazyverilog-lsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(CTest)

# ──────────────────────────────────────────────────────────────────────────────
# LspCpp flags — MUST be set BEFORE FetchContent_MakeAvailable(LspCpp)
# LSPCPP_STANDALONE_ASIO: use vendored asio (third_party/asio) instead of Boost
# LSPCPP_USE_CPP17: use tl::optional instead of boost::optional
# ──────────────────────────────────────────────────────────────────────────────
set(LSPCPP_STANDALONE_ASIO ON CACHE BOOL "Use standalone asio (no Boost)" FORCE)
set(LSPCPP_USE_CPP17 ON CACHE BOOL "Use tl::optional instead of boost::optional" FORCE)
set(LSPCPP_BUILD_EXAMPLES OFF CACHE BOOL "Skip LspCpp examples" FORCE)
set(OPTIONAL_BUILD_TESTS OFF CACHE BOOL "Skip tl-optional tests" FORCE)

# ──────────────────────────────────────────────────────────────────────────────
# External: fmt v11
# OVERRIDE_FIND_PACKAGE lets downstream dependencies reuse this pinned fmt.
# fmt::fmt is brought transitively by slang::slang — do NOT link fmt::fmt
# explicitly.
# ──────────────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.1.4
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(fmt)

# ──────────────────────────────────────────────────────────────────────────────
# External: toml++ (header-only, for lazyverilog.toml parsing)
# ──────────────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# ──────────────────────────────────────────────────────────────────────────────
# External: Catch2 (unit tests)
# ──────────────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(Catch2)

# ──────────────────────────────────────────────────────────────────────────────
# External: slang (SystemVerilog compiler/analyzer)
# ──────────────────────────────────────────────────────────────────────────────
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "Skip slang tools" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "Skip slang tests" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "Skip slang docs" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "Skip slang Python bindings" FORCE)
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "Skip slang install targets" FORCE)
FetchContent_Declare(
    slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG        v10.0
)
FetchContent_MakeAvailable(slang)

# ──────────────────────────────────────────────────────────────────────────────
# External: LspCpp (LSP JSON-RPC transport + type system)
# ──────────────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    LspCpp
    GIT_REPOSITORY https://github.com/kuafuwang/LspCpp.git
    GIT_TAG        82b3d75
    GIT_SUBMODULES "third_party/asio;third_party/ixwebsocket"
)
FetchContent_MakeAvailable(LspCpp)

# ──────────────────────────────────────────────────────────────────────────────
# Main LSP server binary
# ──────────────────────────────────────────────────────────────────────────────
set(LAZYVERILOG_SOURCES
    src/main.cpp
    src/server.cpp
    src/config.cpp
    src/analyzer.cpp
    src/syntax_index.cpp
    src/features/hover.cpp
    src/features/definition.cpp
    src/features/references.cpp
    src/features/rename.cpp
    src/features/completion.cpp
    src/features/signature_help.cpp
    src/features/inlay_hints.cpp
    src/features/workspace_symbols.cpp
    src/features/formatter.cpp
    src/features/lint.cpp
    src/features/autoinst.cpp
    src/features/autoarg.cpp
    src/features/autowire.cpp
    src/features/autoff.cpp
    src/features/autofunc.cpp
    src/features/connect.cpp
    src/features/code_action.cpp
)

add_executable(lazyverilog-lsp ${LAZYVERILOG_SOURCES})
target_include_directories(lazyverilog-lsp PRIVATE
    src
    ${lspcpp_SOURCE_DIR}/include
)
target_link_libraries(lazyverilog-lsp
    PRIVATE
        slang::slang
        lspcpp
        tomlplusplus::tomlplusplus
)

# ──────────────────────────────────────────────────────────────────────────────
# Test binary (Catch2)
# ──────────────────────────────────────────────────────────────────────────────
enable_testing()

set(LAZYVERILOG_TEST_SOURCES
    tests/test_main.cpp
    tests/test_config.cpp
    tests/test_document_sync.cpp
    tests/test_syntax_index.cpp
    tests/test_formatter.cpp
    tests/test_lint.cpp
    tests/test_autoff.cpp
    tests/test_references.cpp
    tests/test_rename.cpp
    tests/test_definition.cpp
    tests/test_inlay_hints.cpp
    tests/test_lsp_features.cpp
    # Feature sources needed by tests (no LSP protocol dependency)
    src/config.cpp
    src/analyzer.cpp
    src/syntax_index.cpp
    src/features/formatter.cpp
    src/features/lint.cpp
    src/features/autoff.cpp
    src/features/inlay_hints.cpp
    src/features/references.cpp
    src/features/rename.cpp
    src/features/hover.cpp
    src/features/signature_help.cpp
    src/features/workspace_symbols.cpp
)

add_executable(lazyverilog-tests ${LAZYVERILOG_TEST_SOURCES})
target_include_directories(lazyverilog-tests PRIVATE
    src
    ${lspcpp_SOURCE_DIR}/include
)
target_link_libraries(lazyverilog-tests
    PRIVATE
        slang::slang
        lspcpp
        tomlplusplus::tomlplusplus
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(lazyverilog-tests)

# ──────────────────────────────────────────────────────────────────────────────
# Slang playground (optional, for experimentation)
# ──────────────────────────────────────────────────────────────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/slang-playground/common.cpp")
    add_executable(slang-playground slang-playground/common.cpp)
    target_link_libraries(slang-playground PRIVATE slang::slang)
endif()
