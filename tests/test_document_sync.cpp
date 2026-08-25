#include <catch2/catch_test_macros.hpp>
#include "analyzer.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

static const std::string kUri = "file:///test.sv";
static const std::string kSrc1 = "module foo; endmodule\n";
static const std::string kSrc2 = "module bar; endmodule\n";

TEST_CASE("doc sync: open creates DocumentState with parsed SyntaxTree", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);
    auto state = a.get_state(kUri);
    REQUIRE(state != nullptr);
    CHECK(state->uri == kUri);
    CHECK(state->text == kSrc1);
    CHECK(state->tree != nullptr);
}

TEST_CASE("doc sync: change updates text and re-parses tree", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);
    auto s1 = a.get_state(kUri);
    REQUIRE(s1 != nullptr);

    a.change(kUri, kSrc2);
    auto s2 = a.get_state(kUri);
    REQUIRE(s2 != nullptr);
    CHECK(s2->text == kSrc2);
    // New snapshot — different object
    CHECK(s2.get() != s1.get());
}

TEST_CASE("doc sync: close removes DocumentState", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);
    REQUIRE(a.get_state(kUri) != nullptr);

    a.close(kUri);
    CHECK(a.get_state(kUri) == nullptr);
}

TEST_CASE("doc sync: concurrent reads are safe", "[sync]") {
    Analyzer a;
    a.open(kUri, kSrc1);

    std::atomic<int> errors{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < 100; ++j) {
                auto s = a.get_state(kUri);
                if (s && s->tree == nullptr) ++errors;
            }
        });
    }
    // One writer thread interleaving changes
    std::thread writer([&] {
        for (int j = 0; j < 20; ++j) {
            a.change(kUri, j % 2 == 0 ? kSrc1 : kSrc2);
        }
    });

    for (auto& t : readers) t.join();
    writer.join();

    CHECK(errors == 0);
    CHECK(a.get_state(kUri) != nullptr);
}

// The parse worker holds a filelist file's live project shard back until the
// parse queue has been idle for a moment, so a burst of keystrokes rebuilds it
// once instead of once per character.  Held back is not dropped: this pins that
// the shard does arrive once the typing stops.
TEST_CASE("doc sync: a deferred live shard still reaches the project index", "[sync]") {
    const auto dir = std::filesystem::temp_directory_path() / "lazyverilog-deferred-shard";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "blk.sv";
    {
        std::ofstream out(path, std::ios::binary);
        out << "module blk;\nendmodule\n";
    }
    const auto uri = "file://" + path.string();

    Analyzer a;
    a.set_project_config({}, {dir.string()}, {path.string()});
    a.wait_for_background_index_idle();

    a.enqueue_parse(uri, "module blk_renamed;\nendmodule\n");

    auto shard_has_module = [&](const std::string& name) {
        auto snapshot = a.extra_index_snapshot_ptr();
        for (const auto& entry : *snapshot) {
            if (entry.uri != uri)
                continue;
            for (const auto& module : entry.index_ref().modules) {
                if (module.name == name)
                    return true;
            }
        }
        return false;
    };

    // Generous: the deferral is time-based, and a loaded CI runner may need
    // several rounds before the worker sees an idle queue.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !shard_has_module("blk_renamed"))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK(shard_has_module("blk_renamed"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
