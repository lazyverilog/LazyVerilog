#pragma once
#include "analyzer.hpp"
#include "config.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Forward declarations to avoid pulling in LspCpp headers here
class RemoteEndPoint;
class BackgroundCompiler;

class LazyVerilogServer {
  public:
    explicit LazyVerilogServer();
    ~LazyVerilogServer();

    /// Block until server exits (stdin closed or exit notification received).
    void run();

  private:
    void register_handlers();
    void publish_diagnostics(const std::string& uri);
    void publish_config_diagnostic(const ConfigWarning* warning);
    void request_inlay_hint_refresh();
    void configure_background_compiler();
    void schedule_background_compilation();

    std::filesystem::path root_;
    bool config_found_{false};
    std::string config_diagnostic_uri_;
    Config config_;
    Analyzer analyzer_;
    std::unique_ptr<BackgroundCompiler> background_compiler_;
    // Last observed textDocument version per open URI.  The server does not
    // predict WorkspaceEdits; it waits for the client to apply them and report
    // the resulting text through normal didChange notifications.
    std::unordered_map<std::string, int> document_versions_;
    std::unordered_map<std::string, std::unordered_set<std::string>> diagnostic_uris_by_owner_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
