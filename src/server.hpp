#pragma once
#include "analyzer.hpp"
#include "config.hpp"
#include <memory>
#include <string>

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
    void configure_background_compiler();
    void schedule_background_compilation();

    std::filesystem::path root_;
    Config config_;
    Analyzer analyzer_;
    std::unique_ptr<BackgroundCompiler> background_compiler_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
