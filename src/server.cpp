#include "server.hpp"
#include "analyzer.hpp"
#include "config.hpp"

// LspCpp headers
#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/JsonRpc/Endpoint.h"
#include "LibLsp/JsonRpc/RemoteEndPoint.h"
#include "LibLsp/JsonRpc/stream.h"
#include "LibLsp/lsp/ProtocolJsonHandler.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/general/initialize.h"
#include "LibLsp/lsp/general/initialized.h"
#include "LibLsp/lsp/general/lsServerCapabilities.h"
#include "LibLsp/lsp/general/lsTextDocumentClientCapabilities.h"
#include "LibLsp/lsp/general/shutdown.h"
#include "LibLsp/lsp/textDocument/did_change.h"
#include "LibLsp/lsp/textDocument/did_close.h"
#include "LibLsp/lsp/textDocument/did_open.h"
#include "LibLsp/lsp/workspace/did_change_configuration.h"
// Feature handlers
#include "LibLsp/lsp/textDocument/completion.h"
#include "LibLsp/lsp/textDocument/declaration_definition.h"
#include "LibLsp/lsp/textDocument/formatting.h"
#include "LibLsp/lsp/textDocument/hover.h"
#include "LibLsp/lsp/textDocument/inlayHint.h"
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/references.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "LibLsp/lsp/textDocument/signature_help.h"
#include "LibLsp/lsp/windows/MessageNotify.h"
#include "LibLsp/lsp/workspace/symbol.h"
#include "features/autoinst.hpp"
#include "features/autoarg.hpp"
#include "features/autowire.hpp"
#include "features/autoff.hpp"
#include "features/autofunc.hpp"
#include "features/code_action.hpp"
#include "features/definition.hpp"
#include "features/formatter.hpp"
#include "features/hover.hpp"
#include "features/inlay_hints.hpp"
#include "features/lint.hpp"
#include "features/references.hpp"
#include "features/rename.hpp"
#include "features/signature_help.hpp"
#include "features/workspace_symbols.hpp"
#include "LibLsp/JsonRpc/serializer.h"
#include "LibLsp/lsp/workspace/execute_command.h"
#include "syntax_index.hpp"
#include <slang/syntax/SyntaxTree.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

struct StdOutStream : lsp::base_ostream<std::ostream> {
    explicit StdOutStream() : base_ostream<std::ostream>(std::cout) {}
    std::string what() override { return {}; }
};
struct StdInStream : lsp::base_istream<std::istream> {
    explicit StdInStream() : base_istream<std::istream>(std::cin) {}
    std::string what() override { return {}; }
};

// ── Incremental-sync helpers ──────────────────────────────────────────────────

// Convert (line, col) LSP position to byte offset in text.
static size_t lsp_offset(const std::string& text, int line, int col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < text.size() && cur < line) {
        if (text[pos] == '\n')
            ++cur;
        ++pos;
    }
    size_t line_start = pos;
    // Advance col UTF-16 code units (treat ASCII; multi-byte clamps gracefully)
    while (pos < text.size() && text[pos] != '\n' && (int)(pos - line_start) < col)
        ++pos;
    return pos;
}

static std::string apply_incremental_change(std::string text,
                                            const lsTextDocumentContentChangeEvent& chg) {
    if (!chg.range)
        return chg.text; // full-document replacement fallback
    size_t start = lsp_offset(text, chg.range->start.line, chg.range->start.character);
    size_t end = lsp_offset(text, chg.range->end.line, chg.range->end.character);
    if (start > text.size())
        start = text.size();
    if (end > text.size())
        end = text.size();
    if (start > end)
        start = end;
    text.replace(start, end - start, chg.text);
    return text;
}

static std::string trim_copy(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

static std::vector<std::string> load_vcode_files(const std::filesystem::path& root,
                                                 const Config& config) {
    std::vector<std::string> paths;
    if (config.design.vcode.empty())
        return paths;

    auto filelist = std::filesystem::path(config.design.vcode);
    if (filelist.is_relative())
        filelist = root / filelist;
    filelist = std::filesystem::absolute(filelist).lexically_normal();

    std::ifstream input(filelist);
    if (!input)
        return paths;

    std::string line;
    while (std::getline(input, line)) {
        auto comment = line.find("//");
        if (comment != std::string::npos)
            line.erase(comment);

        auto item = trim_copy(line);
        if (item.empty() || item.starts_with("+") || item.starts_with("-"))
            continue;

        auto path = std::filesystem::path(item);
        if (path.is_relative())
            path = filelist.parent_path() / path;
        paths.push_back(std::filesystem::absolute(path).lexically_normal().string());
    }
    return paths;
}

// ─────────────────────────────────────────────────────────────────────────────

struct StderrLog : public lsp::Log {
    void log(Level, std::wstring&& msg) override { std::wcerr << msg << L"\n"; }
    void log(Level, const std::wstring& msg) override { std::wcerr << msg << L"\n"; }
    void log(Level, std::string&& msg) override { std::cerr << msg << "\n"; }
    void log(Level, const std::string& msg) override { std::cerr << msg << "\n"; }
};

struct LazyVerilogServer::Impl {
    StderrLog log;
    std::shared_ptr<lsp::ProtocolJsonHandler> json_handler =
        std::make_shared<lsp::ProtocolJsonHandler>();
    std::shared_ptr<GenericEndpoint> endpoint = std::make_shared<GenericEndpoint>(log);
    // max_workers=1: prevents concurrent mimalloc TLS init race between Asio workers
    // on their first allocation (didOpen vs didChangeConfiguration race).
    RemoteEndPoint remote_endpoint{json_handler, endpoint, log, lsp::JSONStreamStyle::Standard, 1};
    std::shared_ptr<StdOutStream> output = std::make_shared<StdOutStream>();
    std::shared_ptr<StdInStream> input = std::make_shared<StdInStream>();
    Condition<bool> exit_event;
};

LazyVerilogServer::LazyVerilogServer() : impl_(std::make_unique<Impl>()) {
    root_ = std::filesystem::current_path();
    config_ = load_config(root_);
    analyzer_.set_extra_files(load_vcode_files(root_, config_));
    register_handlers();
    impl_->remote_endpoint.startProcessingMessages(impl_->input, impl_->output);
}

LazyVerilogServer::~LazyVerilogServer() = default;

void LazyVerilogServer::run() { impl_->exit_event.wait(); }

void LazyVerilogServer::register_handlers() {
    auto* remote = &impl_->remote_endpoint;
    auto& ep = *remote;
    auto show_warning = [remote](const std::string& message) {
        Notify_ShowMessage::notify note;
        note.params.type = lsMessageType::Warning;
        note.params.message = message;
        remote->sendNotification(note);
    };

    // ── initialize ────────────────────────────────────────────────────────────
    ep.registerHandler([&](const td_initialize::request& req) {
        td_initialize::response rsp;
        rsp.id = req.id;
        try {
            auto& caps = rsp.result.capabilities;

            // Text document sync: incremental + open/close notifications
            lsTextDocumentSyncOptions sync_opts;
            sync_opts.openClose = true;
            sync_opts.change = lsTextDocumentSyncKind::Incremental;
            caps.textDocumentSync = std::make_pair(optional<lsTextDocumentSyncKind>{},
                                                   optional<lsTextDocumentSyncOptions>(sync_opts));

            // Simple bool providers
            caps.hoverProvider = true;
            caps.documentFormattingProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.documentRangeFormattingProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.definitionProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.referencesProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});
            caps.workspaceSymbolProvider =
                std::make_pair(optional<bool>(true), optional<WorkDoneProgressOptions>{});

            // Completion: trigger on '.' and '$'
            lsCompletionOptions comp_opts;
            comp_opts.triggerCharacters = std::vector<std::string>{".", "$"};
            comp_opts.resolveProvider = false;
            caps.completionProvider = comp_opts;

            // Signature help: trigger on '(' and ','
            lsSignatureHelpOptions sig_opts;
            sig_opts.triggerCharacters = {"(", ","};
            caps.signatureHelpProvider = sig_opts;

            // Rename: advertise prepareRename support
            RenameOptions rename_opts;
            rename_opts.prepareProvider = true;
            caps.renameProvider =
                std::make_pair(optional<bool>{}, optional<RenameOptions>(rename_opts));

            // Code action
            caps.codeActionProvider =
                std::make_pair(optional<bool>(true), optional<CodeActionOptions>{});

            // Extract workspace root from initialize params
            auto uri_to_path = [](const std::string& uri) -> std::filesystem::path {
                if (uri.starts_with("file://"))
                    return {uri.substr(7)};
                return {uri};
            };
            if (req.params.rootUri && !req.params.rootUri->raw_uri_.empty()) {
                auto p = uri_to_path(req.params.rootUri->raw_uri_);
                if (std::filesystem::exists(p)) {
                    root_ = p;
                    config_ = load_config(root_);
                    analyzer_.set_extra_files(load_vcode_files(root_, config_));
                }
            } else if (req.params.rootPath && !req.params.rootPath->empty()) {
                std::filesystem::path p(*req.params.rootPath);
                if (std::filesystem::exists(p)) {
                    root_ = p;
                    config_ = load_config(root_);
                    analyzer_.set_extra_files(load_vcode_files(root_, config_));
                }
            }

            // Inlay hints
            caps.inlayHintProvider = std::make_pair(optional<bool>(config_.inlay_hint.enable),
                                                    optional<InlayHintOptions>{});

            // Execute command — 16 server-side commands
            lsExecuteCommandOptions exec_opts;
            exec_opts.commands = {
                "lazyverilogpy.rtlTree",
                "lazyverilogpy.rtlTreeReverse",
                "lazyverilogpy.autowire",
                "lazyverilogpy.autowirepreview",
                "lazyverilogpy.connectInfo",
                "lazyverilogpy.connectApply",
                "lazyverilogpy.connectApplyPreview",
                "lazyverilogpy.autoffPreview",
                "lazyverilogpy.autoffApply",
                "lazyverilogpy.autoffAllPreview",
                "lazyverilogpy.autoffAllApply",
                "lazyverilogpy.interface",
                "lazyverilogpy.interfaceConnect",
                "lazyverilogpy.interfaceDisconnect",
                "lazyverilogpy.singleInterface",
                "lazyverilogpy.lint",
            };
            caps.executeCommandProvider = exec_opts;
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] initialize error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── initialized ───────────────────────────────────────────────────────────
    ep.registerHandler([&](const Notify_InitializedNotification::notify&) {
        // no-op: client signals ready, server can dynamically register here
    });

    // ── shutdown ──────────────────────────────────────────────────────────────
    ep.registerHandler([&](const td_shutdown::request& req) {
        td_shutdown::response rsp;
        rsp.id = req.id;
        // LSP spec: shutdown result must be null, not omitted
        lsp::Any null_result;
        null_result.SetJsonString("null", lsp::Any::kNullType);
        rsp.result = std::move(null_result);
        return rsp;
    });

    // ── exit ──────────────────────────────────────────────────────────────────
    ep.registerHandler([&, remote](const Notify_Exit::notify&) {
        try {
            remote->stop();
        } catch (...) {
        }
        impl_->exit_event.notify(std::make_unique<bool>(true));
    });

    // ── workspace/didChangeConfiguration ─────────────────────────────────────
    ep.registerHandler([&](const Notify_WorkspaceDidChangeConfiguration::notify& note) {
        try {
            // Re-read config from disk on every configuration change
            config_ = load_config(root_);
            analyzer_.set_extra_files(load_vcode_files(root_, config_));
            (void)note; // settings in note.params.settings parsed lazily
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didChangeConfiguration error: " << e.what() << "\n";
        }
    });

    // Helper: convert pre-formatted ParseDiagInfo → LSP publishDiagnostics notification
    auto publish_diags = [&, remote](const std::string& uri) {
        try {
            auto state = analyzer_.get_state(uri);
            Notify_TextDocumentPublishDiagnostics::notify notif;
            notif.params.uri.raw_uri_ = uri;
            if (state) {
                // Merge parse diagnostics + lint diagnostics
                auto all_diags = state->parse_diagnostics;
                auto lint_diags = run_lint(*state, config_.lint);
                all_diags.insert(all_diags.end(), lint_diags.begin(), lint_diags.end());
                for (const auto& d : all_diags) {
                    lsDiagnostic ld;
                    ld.range.start = lsPosition(d.line, d.col);
                    ld.range.end = ld.range.start;
                    switch (d.severity) {
                    case 1:
                        ld.severity = lsDiagnosticSeverity::Error;
                        break;
                    case 2:
                        ld.severity = lsDiagnosticSeverity::Warning;
                        break;
                    default:
                        ld.severity = lsDiagnosticSeverity::Information;
                        break;
                    }
                    ld.source = std::string("lazyverilog");
                    ld.message = d.message;
                    notif.params.diagnostics.push_back(std::move(ld));
                }
            }
            remote->sendNotification(notif);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] publishDiagnostics error: " << e.what() << "\n";
        }
    };

    // ── textDocument/didOpen ──────────────────────────────────────────────────
    ep.registerHandler([&, publish_diags](const Notify_TextDocumentDidOpen::notify& note) {
        try {
            const auto& td = note.params.textDocument;
            // Walk up from file to find lazyverilog.toml if not already found
            if (!std::filesystem::exists(root_ / "lazyverilog.toml")) {
                auto uri = td.uri.raw_uri_;
                if (uri.starts_with("file://"))
                    uri = uri.substr(7);
                auto found = find_config_root(uri);
                if (!found.empty()) {
                    root_ = found;
                    config_ = load_config(root_);
                    analyzer_.set_extra_files(load_vcode_files(root_, config_));
                }
            }
            analyzer_.open(td.uri.raw_uri_, td.text);
            publish_diags(td.uri.raw_uri_);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didOpen error: " << e.what() << "\n";
        }
    });

    // ── textDocument/didChange ────────────────────────────────────────────────
    ep.registerHandler([&, publish_diags](const Notify_TextDocumentDidChange::notify& note) {
        try {
            const auto& uri = note.params.textDocument.uri.raw_uri_;
            if (!note.params.contentChanges.empty()) {
                auto state = analyzer_.get_state(uri);
                std::string text = state ? state->text : "";
                for (const auto& chg : note.params.contentChanges)
                    text = apply_incremental_change(std::move(text), chg);
                analyzer_.change(uri, text);
            }
            publish_diags(uri);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didChange error: " << e.what() << "\n";
        }
    });

    // ── textDocument/didClose ─────────────────────────────────────────────────
    ep.registerHandler([&](const Notify_TextDocumentDidClose::notify& note) {
        try {
            analyzer_.close(note.params.textDocument.uri.raw_uri_);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] didClose error: " << e.what() << "\n";
        }
    });

    // ── textDocument/formatting ───────────────────────────────────────────────
    ep.registerHandler([&, show_warning](const td_formatting::request& req) {
        td_formatting::response rsp;
        rsp.id = req.id;
        try {
            if (config_.format.enable_format_on_save) {
                const auto& uri = req.params.textDocument.uri.raw_uri_;
                auto state = analyzer_.get_state(uri);
                if (state) {
                    std::string formatted = format_source(state->text, config_.format);
                    if (formatted != state->text) {
                        // Replace the entire document with one edit
                        lsTextEdit edit;
                        // Count lines in original
                        int line_count = 0;
                        int last_nl = -1;
                        for (int k = 0; k < (int)state->text.size(); ++k) {
                            if (state->text[k] == '\n') {
                                ++line_count;
                                last_nl = k;
                            }
                        }
                        int last_line_len = (int)state->text.size() - last_nl - 1;
                        edit.range.start = lsPosition(0, 0);
                        edit.range.end = lsPosition(line_count, last_line_len);
                        edit.newText = formatted;
                        rsp.result.push_back(std::move(edit));
                    }
                }
            }
        } catch (const SafeModeError& e) {
            show_warning(e.what());
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] formatting error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/hover ────────────────────────────────────────────────────
    ep.registerHandler([&](const td_hover::request& req) {
        td_hover::response rsp;
        rsp.id = req.id;
        try {
            if (auto hover = provide_hover(analyzer_, req.params))
                rsp.result = std::move(*hover);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] hover error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/definition ───────────────────────────────────────────────
    ep.registerHandler([&](const td_definition::request& req) {
        td_definition::response rsp;
        rsp.id = req.id;
        try {
            auto loc = provide_definition(analyzer_, req.params);
            if (loc) {
                rsp.result =
                    std::make_pair(optional<std::vector<lsLocation>>(std::vector<lsLocation>{*loc}),
                                   optional<std::vector<LocationLink>>{});
            }
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] definition error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/references ───────────────────────────────────────────────
    ep.registerHandler([&](const td_references::request& req) {
        td_references::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_references(analyzer_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] references error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/prepareRename ────────────────────────────────────────────
    ep.registerHandler([&](const td_prepareRename::request& req) {
        td_prepareRename::response rsp;
        rsp.id = req.id;
        try {
            if (auto prepared = prepare_rename(analyzer_, req.params))
                rsp.result = std::make_pair(optional<lsRange>{},
                                            optional<PrepareRenameResult>(std::move(*prepared)));
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] prepareRename error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/rename ───────────────────────────────────────────────────
    ep.registerHandler([&](const td_rename::request& req) {
        td_rename::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_rename(analyzer_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] rename error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/completion ───────────────────────────────────────────────
    ep.registerHandler([&](const td_completion::request& req) {
        td_completion::response rsp;
        rsp.id = req.id;
        try {
            const auto& uri = req.params.textDocument.uri.raw_uri_;
            auto state = analyzer_.get_state(uri);
            CompletionList cl;
            cl.isIncomplete = false;
            // SV keywords
            static const std::vector<std::string> kKeywords = {
                "module",  "endmodule", "input",       "output",      "inout",        "logic",
                "wire",    "reg",       "always_ff",   "always_comb", "always_latch", "always",
                "begin",   "end",       "if",          "else",        "case",         "endcase",
                "for",     "generate",  "endgenerate", "parameter",   "localparam",   "assign",
                "posedge", "negedge",   "integer"};
            for (const auto& kw : kKeywords) {
                lsCompletionItem item;
                item.label = kw;
                item.kind = optional<lsCompletionItemKind>(lsCompletionItemKind::Keyword);
                cl.items.push_back(std::move(item));
            }
            // Module and signal names from SyntaxIndex
            if (state && state->tree) {
                auto idx = SyntaxIndex::build(*state->tree, state->text);
                for (const auto& m : idx.modules) {
                    lsCompletionItem item;
                    item.label = m.name;
                    item.kind = optional<lsCompletionItemKind>(lsCompletionItemKind::Module);
                    cl.items.push_back(std::move(item));
                    for (const auto& p : m.ports) {
                        lsCompletionItem pi;
                        pi.label = p.name;
                        pi.kind = optional<lsCompletionItemKind>(lsCompletionItemKind::Variable);
                        pi.detail = optional<std::string>(p.direction);
                        cl.items.push_back(std::move(pi));
                    }
                }
            }
            rsp.result = std::move(cl);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] completion error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/signatureHelp ────────────────────────────────────────────
    ep.registerHandler([&](const td_signatureHelp::request& req) {
        td_signatureHelp::response rsp;
        rsp.id = req.id;
        try {
            if (auto help = provide_signature_help(analyzer_, req.params))
                rsp.result = std::move(*help);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] signatureHelp error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/inlayHint ────────────────────────────────────────────────
    ep.registerHandler([&](const td_inlayHint::request& req) {
        td_inlayHint::response rsp;
        rsp.id = req.id;
        try {
            if (!config_.inlay_hint.enable)
                return rsp;
            const auto& uri = req.params.textDocument.uri.raw_uri_;
            rsp.result = provide_inlay_hints(analyzer_, uri, req.params.range.start.line,
                                             req.params.range.end.line);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] inlayHint error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── workspace/symbol ──────────────────────────────────────────────────────
    ep.registerHandler([&](const wp_symbol::request& req) {
        wp_symbol::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_workspace_symbols(analyzer_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] workspaceSymbol error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── textDocument/codeAction ───────────────────────────────────────────────
    ep.registerHandler([&](const td_codeActionCode::request& req) {
        td_codeActionCode::response rsp;
        rsp.id = req.id;
        try {
            rsp.result = provide_code_actions(analyzer_, config_, req.params);
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] codeAction error: " << e.what() << "\n";
        }
        return rsp;
    });

    // ── workspace/executeCommand ──────────────────────────────────────────────
    ep.registerHandler([&](const wp_executeCommand::request& req) {
        wp_executeCommand::response rsp;
        rsp.id = req.id;
        try {
            const auto& cmd = req.params.command;
            const auto& args = req.params.arguments;

            auto get_string = [&](size_t i) -> std::string {
                if (!args || i >= args->size()) return {};
                std::string s;
                const_cast<lsp::Any&>((*args)[i]).Get(s);
                return s;
            };
            auto get_int = [&](size_t i) -> int {
                if (!args || i >= args->size()) return 0;
                int v = 0;
                const_cast<lsp::Any&>((*args)[i]).Get(v);
                return v;
            };

            auto apply_ff_edits = [&](const AutoffResult& result, const std::string& uri) {
                if (result.has_error || (result.edits.empty() && result.warn)) {
                    // Return null result
                    lsp::Any null_result;
                    null_result.SetJsonString("null", lsp::Any::kNullType);
                    rsp.result = std::move(null_result);
                    return;
                }
                if (result.edits.empty()) return;

                auto state = analyzer_.get_state(uri);
                if (!state) return;

                // Apply edits in reverse order to build new text
                auto lines = [&]() {
                    std::vector<std::string> ls;
                    size_t start = 0;
                    const std::string& t = state->text;
                    while (start <= t.size()) {
                        size_t end = t.find('\n', start);
                        if (end == std::string::npos) { ls.push_back(t.substr(start)); break; }
                        ls.push_back(t.substr(start, end - start));
                        start = end + 1;
                    }
                    return ls;
                }();

                // Insert edits (already sorted in reverse line order)
                for (const auto& edit : result.edits) {
                    if (edit.line >= 0 && edit.line <= (int)lines.size()) {
                        // Split edit.text into lines and insert before lines[edit.line]
                        std::vector<std::string> new_lines;
                        std::string remaining = edit.text;
                        size_t pos = 0;
                        while (pos <= remaining.size()) {
                            size_t nl = remaining.find('\n', pos);
                            if (nl == std::string::npos) {
                                if (pos < remaining.size())
                                    new_lines.push_back(remaining.substr(pos));
                                break;
                            }
                            new_lines.push_back(remaining.substr(pos, nl - pos));
                            pos = nl + 1;
                        }
                        lines.insert(lines.begin() + edit.line, new_lines.begin(), new_lines.end());
                    }
                }

                std::string new_text;
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (i > 0) new_text += "\n";
                    new_text += lines[i];
                }

                // Build workspace edit JSON and set as result
                lsWorkspaceEdit we;
                lsTextEdit text_edit;
                text_edit.range.start = lsPosition(0, 0);
                text_edit.range.end = lsPosition(999999, 0);
                text_edit.newText = new_text;
                we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
                (*we.changes)[uri] = {text_edit};

                // Serialize WorkspaceEdit manually to JSON
                std::string json = "{\"changes\":{\"" + uri + "\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":999999,\"character\":0}},\"newText\":";
                // JSON-encode the newText
                std::string escaped_text = "\"";
                for (char c : new_text) {
                    if (c == '"') escaped_text += "\\\"";
                    else if (c == '\\') escaped_text += "\\\\";
                    else if (c == '\n') escaped_text += "\\n";
                    else if (c == '\r') escaped_text += "\\r";
                    else if (c == '\t') escaped_text += "\\t";
                    else escaped_text += c;
                }
                escaped_text += "\"";
                json += escaped_text + "}]}}";
                rsp.result.SetJsonString(json, lsp::Any::kObjectType);
            };

            if (cmd == "lazyverilogpy.autoffPreview" || cmd == "lazyverilogpy.autoffApply") {
                std::string uri = get_string(0);
                int ff_line = get_int(1);
                auto state = analyzer_.get_state(uri);
                if (state) {
                    auto result = autoff(*state, ff_line, config_.lint.naming.register_pattern);
                    apply_ff_edits(result, uri);
                }
            } else if (cmd == "lazyverilogpy.autoffAllPreview" || cmd == "lazyverilogpy.autoffAllApply") {
                std::string uri = get_string(0);
                auto state = analyzer_.get_state(uri);
                if (state) {
                    auto result = autoff_all(*state, config_.lint.naming.register_pattern);
                    apply_ff_edits(result, uri);
                }
            } else if (cmd == "lazyverilogpy.autowire" || cmd == "lazyverilogpy.autowirepreview") {
                std::string uri = get_string(0);
                auto state = analyzer_.get_state(uri);
                if (state && state->tree) {
                    auto idx = SyntaxIndex::build(*state->tree, state->text);
                    if (cmd == "lazyverilogpy.autowirepreview") {
                        auto preview = autowire_preview(*state, idx, config_.autowire);
                        // Return preview lines as JSON array of strings
                        std::string json = "[";
                        for (size_t i = 0; i < preview.size(); ++i) {
                            if (i > 0) json += ",";
                            json += "\"";
                            for (char c : preview[i]) {
                                if (c == '"') json += "\\\"";
                                else if (c == '\\') json += "\\\\";
                                else if (c == '\n') json += "\\n";
                                else json += c;
                            }
                            json += "\"";
                        }
                        json += "]";
                        rsp.result.SetJsonString(json, lsp::Any::kUnKnown);
                    } else {
                        std::string new_source = autowire_apply(*state, idx, config_.autowire);
                        if (new_source != state->text) {
                            lsWorkspaceEdit we;
                            lsTextEdit text_edit;
                            text_edit.range.start = lsPosition(0, 0);
                            text_edit.range.end = lsPosition(999999, 0);
                            text_edit.newText = new_source;
                            we.changes = std::map<std::string, std::vector<lsTextEdit>>{};
                            (*we.changes)[uri] = {text_edit};
                            // Serialize WorkspaceEdit manually to JSON
                            std::string json = "{\"changes\":{\"" + uri + "\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":999999,\"character\":0}},\"newText\":";
                            std::string esc = "\"";
                            for (char c : new_source) {
                                if (c == '"') esc += "\\\"";
                                else if (c == '\\') esc += "\\\\";
                                else if (c == '\n') esc += "\\n";
                                else if (c == '\r') esc += "\\r";
                                else if (c == '\t') esc += "\\t";
                                else esc += c;
                            }
                            esc += "\"";
                            json += esc + "}]}}";
                            rsp.result.SetJsonString(json, lsp::Any::kObjectType);
                        }
                    }
                }
            }
            // Other commands (rtlTree, connect, interface, lint) — return null for now
            if (rsp.result.GetType() == lsp::Any::Type::kUnKnown) {
                lsp::Any null_result;
                null_result.SetJsonString("null", lsp::Any::kNullType);
                rsp.result = std::move(null_result);
            }
        } catch (const std::exception& e) {
            std::cerr << "[lazyverilog] executeCommand error: " << e.what() << "\n";
        }
        return rsp;
    });
}
