// Catch2 main — provides main() for the test binary
#define CATCH_CONFIG_MAIN
// With Catch2 v3 + CMake's Catch2WithMain, main is provided by the library.
// This file exists as a placeholder; actual tests are in test_*.cpp files.

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = getpid();
#endif
        dir_ = fs::temp_directory_path() / ("lazyverilog-tests-" + std::to_string(pid));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
        const std::string value = dir_.string();
#ifdef _WIN32
        // GetTempPathW (temp_directory_path) reads TMP first.
        _putenv_s("TMP", value.c_str());
        _putenv_s("TEMP", value.c_str());
#else
        setenv("TMPDIR", value.c_str(), 1);
#endif
    }

    void testRunEnded(Catch::TestRunStats const&) override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

private:
    std::filesystem::path dir_;
};

} // namespace

CATCH_REGISTER_LISTENER(PerProcessTempDirectory)
