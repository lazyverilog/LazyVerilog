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

# LspCpp vendors ixwebsocket for optional WebSocket transport support.  This
# server talks JSON-RPC over stdin/stdout, so it does not need WebSocket
# per-message compression.  ixwebsocket enables ZLIB by default and Windows
# GitHub runners do not provide a CMake-discoverable ZLIB package out of the
# box, which breaks release builds before our own targets are configured.
# Disable it globally before FetchContent_MakeAvailable(LspCpp) so the vendored
# ixwebsocket subproject sees the cache option during its first configure pass.
set(USE_ZLIB OFF CACHE BOOL "Disable ixwebsocket zlib compression" FORCE)

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

# LspCpp vendors network-uri and enables -Werror inside that third-party
# project.  GCC's optimized Release builds can emit a false-positive
# -Wmaybe-uninitialized warning in network::detail::uri_part::empty() after
# inlining optional<uri_part> code.  Because the warning originates in vendored
# code, keep LazyVerilog warnings strict but prevent this dependency from
# turning that diagnostic into a release-build failure.  Place this after
# FetchContent_MakeAvailable(LspCpp) so the vendored target already exists.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND TARGET network-uri)
    target_compile_options(network-uri PRIVATE -Wno-error=maybe-uninitialized)
endif()

# Patch: LspCpp's lsServerCapabilities MAKE_REFLECT_STRUCT omits inlayHintProvider,
# so the field is never serialized in the initialize response and clients never see
# the capability. Apply an idempotent string replacement after every cmake run.
set(_caps_h "${lspcpp_SOURCE_DIR}/include/LibLsp/lsp/general/lsServerCapabilities.h")
file(READ "${_caps_h}" _caps_content)
string(REPLACE
    "monikerProvider, semanticTokensProvider\n)"
    "monikerProvider, semanticTokensProvider, inlayHintProvider\n)"
    _caps_content "${_caps_content}")
file(WRITE "${_caps_h}" "${_caps_content}")

# EditWatermark parses the raw didChange messages LspCpp hands it through the
# hook below, and parses them with the same JSON library LspCpp itself uses so
# the two agree on what a message says.  LspCpp keeps RapidJSON a private
# include, so name it here; this is the vendored copy its own configure reports
# as "Using local RapidJSON".
set(LAZYVERILOG_RAPIDJSON_INCLUDE "${lspcpp_SOURCE_DIR}/third_party/rapidjson/include")
if(NOT EXISTS "${LAZYVERILOG_RAPIDJSON_INCLUDE}/rapidjson/document.h")
    message(FATAL_ERROR
        "RapidJSON headers not found at ${LAZYVERILOG_RAPIDJSON_INCLUDE}. LspCpp vendors them "
        "as a submodule; src/edit_watermark.cpp needs them to read incoming messages.")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Patch: give LspCpp's RemoteEndPoint a reader-thread hook for incoming messages.
#
# LspCpp reads framed messages on one thread and asio::post()s each of them to a
# thread pool; with max_workers = 1 that pool runs them strictly in order.  So a
# handler only ever sees messages that have already been dispatched, and cannot
# tell that a newer edit to the same document is already in flight — the pending
# work is in asio's own task queue, which has no introspection API, and each
# entry is an unparsed string besides.  The producer callback, which fires once
# per complete framed message before that post, is the one place where it is
# visible.  The hook itself takes no position on what a message means; see
# EditWatermark in src/edit_watermark.hpp for what this server does with it.
#
# Applied as an idempotent edit after populate, for the same reason as the patch
# above: FetchContent's PATCH_COMMAND runs only on the first populate, and a
# dependency that silently loses its patch on a re-configure is worse than one
# that never had it.  The inserted text is in cmake/lspcpp/ so it reads as C++
# rather than as an escaped CMake string.
# ──────────────────────────────────────────────────────────────────────────────
set(_rep_h "${lspcpp_SOURCE_DIR}/include/LibLsp/JsonRpc/RemoteEndPoint.h")
set(_rep_cpp "${lspcpp_SOURCE_DIR}/src/jsonrpc/RemoteEndPoint.cpp")
set(_rep_h_anchor "    void handle(MessageIssue&&) override;")
set(_rep_cpp_anchor "                    asio::post(")

file(READ "${_rep_h}" _rep_h_content)
string(FIND "${_rep_h_content}" "setIncomingMessagePreview" _rep_h_done)
if(_rep_h_done EQUAL -1)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lspcpp/remote_endpoint_preview.h.in" _rep_h_insert)
    string(REPLACE "${_rep_h_anchor}" "${_rep_h_anchor}\n${_rep_h_insert}"
           _rep_h_content "${_rep_h_content}")
    file(WRITE "${_rep_h}" "${_rep_h_content}")
endif()

file(READ "${_rep_cpp}" _rep_cpp_content)
string(FIND "${_rep_cpp_content}" "incoming_message_preview_" _rep_cpp_done)
if(_rep_cpp_done EQUAL -1)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lspcpp/remote_endpoint_preview.cpp.in" _rep_cpp_insert)
    string(REPLACE "${_rep_cpp_anchor}" "${_rep_cpp_insert}${_rep_cpp_anchor}"
           _rep_cpp_content "${_rep_cpp_content}")
    file(WRITE "${_rep_cpp}" "${_rep_cpp_content}")
endif()

# An upstream bump that moves either anchor must fail the configure, not produce
# a server that has quietly stopped dropping superseded work.
string(FIND "${_rep_h_content}" "setIncomingMessagePreview" _rep_h_ok)
string(FIND "${_rep_cpp_content}" "incoming_message_preview_" _rep_cpp_ok)
if(_rep_h_ok EQUAL -1 OR _rep_cpp_ok EQUAL -1)
    message(FATAL_ERROR
        "Could not patch LspCpp's RemoteEndPoint with the incoming-message preview hook. "
        "The pinned LspCpp revision has probably moved and the anchors above no longer "
        "match its source. Re-derive them before bumping GIT_TAG.")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Version string for --version, from the nearest git tag
# ──────────────────────────────────────────────────────────────────────────────
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE LAZYVERILOG_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_describe_result
    ERROR_QUIET
)
if(NOT _git_describe_result EQUAL 0 OR LAZYVERILOG_GIT_VERSION STREQUAL "")
    set(LAZYVERILOG_GIT_VERSION "unknown")
endif()
add_compile_definitions(LAZYVERILOG_VERSION="${LAZYVERILOG_GIT_VERSION}")

# ──────────────────────────────────────────────────────────────────────────────
# Main LSP server binary
# ──────────────────────────────────────────────────────────────────────────────
set(LAZYVERILOG_SOURCES
    src/main.cpp
    src/server.cpp
    src/config.cpp
    src/cpu_budget.cpp
    src/filelist.cpp
    src/analyzer.cpp
    src/dynamic_file_index.cpp
    src/background_compiler.cpp
    src/syntax_index.cpp
    src/syntax_index_shared.cpp
    src/index_cache.cpp
    src/edit_watermark.cpp
    src/features/hover.cpp
    src/features/definition.cpp
    src/features/references.cpp
    src/features/rename.cpp
    src/features/completion.cpp
    src/features/signature_help.cpp
    src/features/inlay_hints.cpp
    src/features/workspace_symbols.cpp
    src/features/document_symbols.cpp
    src/features/formatter.cpp
    src/features/lint.cpp
    src/features/autoinst.cpp
    src/features/autoarg.cpp
    src/features/autowire.cpp
    src/features/autoff.cpp
    src/features/autofunc.cpp
    src/features/connect.cpp
    src/features/code_action.cpp
    src/features/folding_range.cpp
)

add_executable(lazyverilog-lsp ${LAZYVERILOG_SOURCES})
target_include_directories(lazyverilog-lsp PRIVATE
    src
    ${lspcpp_SOURCE_DIR}/include
    ${LAZYVERILOG_RAPIDJSON_INCLUDE}
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
    tests/test_cpu_budget.cpp
    tests/test_path_uri.cpp
    tests/test_filelist.cpp
    tests/test_document_sync.cpp
    tests/test_open_parse_header_cache.cpp
    tests/test_open_header_projection.cpp
    tests/test_open_shard_scope.cpp
    tests/test_syntax_index.cpp
    tests/test_shared_header_scaling.cpp
    tests/test_index_cache.cpp
    tests/test_edit_watermark.cpp
    tests/test_formatter.cpp
    tests/test_lint.cpp
    tests/test_autoff.cpp
    tests/test_connect.cpp
    tests/test_references.cpp
    tests/test_rename.cpp
    tests/test_definition.cpp
    tests/test_inlay_hints.cpp
    tests/test_lsp_features.cpp
    tests/test_completion.cpp
    tests/test_folding_ranges.cpp
    tests/test_document_symbols.cpp
    tests/test_opentitan_benchmark.cpp
    # Feature sources needed by tests (no LSP protocol dependency)
    src/features/completion.cpp
    src/features/code_action.cpp
    src/features/autoinst.cpp
    src/features/autoarg.cpp
    src/config.cpp
    src/cpu_budget.cpp
    src/filelist.cpp
    src/analyzer.cpp
    src/dynamic_file_index.cpp
    src/syntax_index.cpp
    src/syntax_index_shared.cpp
    src/index_cache.cpp
    src/edit_watermark.cpp
    src/features/formatter.cpp
    src/features/lint.cpp
    src/features/autoff.cpp
    src/features/autofunc.cpp
    src/features/autowire.cpp
    src/features/connect.cpp
    src/features/inlay_hints.cpp
    src/features/references.cpp
    src/features/rename.cpp
    src/features/hover.cpp
    src/features/signature_help.cpp
    src/features/workspace_symbols.cpp
    src/features/document_symbols.cpp
    src/features/folding_range.cpp
)

add_executable(lazyverilog-tests ${LAZYVERILOG_TEST_SOURCES})
target_include_directories(lazyverilog-tests PRIVATE
    src
    tests
    ${lspcpp_SOURCE_DIR}/include
    ${LAZYVERILOG_RAPIDJSON_INCLUDE}
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

add_executable(parse-bench
    tools/parse_bench.cpp
    src/dynamic_file_index.cpp
    src/syntax_index.cpp
    src/syntax_index_shared.cpp
    src/index_cache.cpp)
target_include_directories(parse-bench PRIVATE src)
target_link_libraries(parse-bench PRIVATE slang::slang)

add_executable(index-bench
    tools/index_bench.cpp
    src/analyzer.cpp
    src/config.cpp
    src/cpu_budget.cpp
    src/filelist.cpp
    src/dynamic_file_index.cpp
    src/syntax_index.cpp
    src/syntax_index_shared.cpp
    src/index_cache.cpp)
target_include_directories(index-bench PRIVATE src)
target_link_libraries(index-bench PRIVATE slang::slang tomlplusplus::tomlplusplus)

add_executable(lazyverilog-fmt tools/fmt_file.cpp src/features/formatter.cpp src/config.cpp)
target_include_directories(lazyverilog-fmt PRIVATE src)
target_link_libraries(lazyverilog-fmt PRIVATE slang::slang tomlplusplus::tomlplusplus)

# ──────────────────────────────────────────────────────────────────────────────
# lazyverilog-lint / lazyverilog-rtltree CLI tools
# Share the same project-bootstrap sources as the LSP server (minus main.cpp,
# server.cpp, and LspCpp) so they see the same parse/lint/compile/rtl-tree
# behavior as `lazyverilog-lsp` for the same lazyverilog.toml.
# ──────────────────────────────────────────────────────────────────────────────
set(LAZYVERILOG_CLI_PROJECT_SOURCES
    tools/cli_project.cpp
    src/config.cpp
    src/filelist.cpp
    src/analyzer.cpp
    src/cpu_budget.cpp
    src/dynamic_file_index.cpp
    src/background_compiler.cpp
    src/syntax_index.cpp
    src/syntax_index_shared.cpp
    src/index_cache.cpp
    src/features/lint.cpp
)

add_executable(lazyverilog-lint tools/lint_file.cpp ${LAZYVERILOG_CLI_PROJECT_SOURCES})
target_include_directories(lazyverilog-lint PRIVATE src tools)
target_link_libraries(lazyverilog-lint PRIVATE slang::slang tomlplusplus::tomlplusplus)

add_executable(lazyverilog-rtltree tools/rtltree_file.cpp ${LAZYVERILOG_CLI_PROJECT_SOURCES})
target_include_directories(lazyverilog-rtltree PRIVATE src tools)
target_link_libraries(lazyverilog-rtltree PRIVATE slang::slang tomlplusplus::tomlplusplus)

add_executable(lazyverilog-rtl-format-test tests/rtl_format_sweep.cpp)
add_test(
    NAME formatter-rtl-corpus
    COMMAND lazyverilog-rtl-format-test
            $<TARGET_FILE:lazyverilog-fmt>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/rtl/lazyverilog_formatter_corpus
)

add_executable(lazyverilog-lint-cli-smoke tests/lint_cli_smoke.cpp)
target_include_directories(lazyverilog-lint-cli-smoke PRIVATE tests)
add_test(
    NAME lint-cli-smoke
    COMMAND lazyverilog-lint-cli-smoke
            $<TARGET_FILE:lazyverilog-lint>
            ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(lazyverilog-config-root-cli-smoke tests/config_root_cli_smoke.cpp)
target_include_directories(lazyverilog-config-root-cli-smoke PRIVATE tests)
add_test(
    NAME config-root-cli-smoke
    COMMAND lazyverilog-config-root-cli-smoke
            $<TARGET_FILE:lazyverilog-lsp>
            ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(lazyverilog-request-reply-cli-smoke tests/request_reply_cli_smoke.cpp)
target_include_directories(lazyverilog-request-reply-cli-smoke PRIVATE tests)
add_test(
    NAME request-reply-cli-smoke
    COMMAND lazyverilog-request-reply-cli-smoke
            $<TARGET_FILE:lazyverilog-lsp>
)

add_executable(lazyverilog-rtltree-cli-smoke tests/rtltree_cli_smoke.cpp)
target_include_directories(lazyverilog-rtltree-cli-smoke PRIVATE tests)
add_test(
    NAME rtltree-cli-smoke
    COMMAND lazyverilog-rtltree-cli-smoke
            $<TARGET_FILE:lazyverilog-rtltree>
            ${CMAKE_CURRENT_SOURCE_DIR}
)
