// Catch2 main — provides main() for the test binary
#define CATCH_CONFIG_MAIN
// With Catch2 v3 + CMake's Catch2WithMain, main is provided by the library.
// This file exists as a placeholder; actual tests are in test_*.cpp files.

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "cli_process.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

// Tests name their scratch directories under temp_directory_path() with fixed
// names, so two processes running the same test removed each other's files.
// Point the temp directory at one of this process's own instead, so every
// helper's name is unique per process without changing each call site.
class PerProcessTempDirectory : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        namespace fs = std::filesystem;
        dir_ = fs::temp_directory_path() /
               ("lazyverilog-tests-" + std::to_string(cli_process::current_process_id()));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
#ifdef _WIN32
        // GetTempPathW (temp_directory_path) reads TMP first.  Wide, so a
        // temp path outside the ANSI code page survives.
        _wputenv_s(L"TMP", dir_.c_str());
        _wputenv_s(L"TEMP", dir_.c_str());
#else
        setenv("TMPDIR", dir_.c_str(), 1);
#endif
    }

    // A tree past MAX_PATH would hang libstdc++'s remove_all(); the one test
    // that builds one ("a project deeper than MAX_PATH") renames it short
    // on every exit it can take, and a crash never reaches here.
    void testRunEnded(Catch::TestRunStats const&) override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

private:
    std::filesystem::path dir_;
};

} // namespace

CATCH_REGISTER_LISTENER(PerProcessTempDirectory)
