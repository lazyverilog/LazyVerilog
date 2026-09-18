#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

TEST_CASE("path utils: POSIX file URI decoding is unchanged", "[path][uri]") {
    CHECK(path_from_file_uri("file:///tmp/lazyverilog/top.sv") ==
          "/tmp/lazyverilog/top.sv");
    CHECK(path_from_file_uri("/tmp/lazyverilog/top.sv") ==
          "/tmp/lazyverilog/top.sv");
    CHECK(path_from_file_uri("file:///tmp/lazy%20verilog/top.sv") ==
          "/tmp/lazy verilog/top.sv");
}

TEST_CASE("path utils: Windows file URI loses URI-only leading slash", "[path][uri]") {
    CHECK(path_from_file_uri("file:///C:/repo/rtl/top.sv") ==
          "C:/repo/rtl/top.sv");
    CHECK(path_from_file_uri("file:///c:/repo/rtl/top.sv") ==
          "c:/repo/rtl/top.sv");
    CHECK(path_from_file_uri("file:///C:/repo%20space/rtl/top.sv") ==
          "C:/repo space/rtl/top.sv");
}

TEST_CASE("path utils: UNC file URI keeps UNC shape", "[path][uri]") {
    CHECK(path_from_file_uri("file://server/share/rtl/top.sv") ==
          "//server/share/rtl/top.sv");
}

TEST_CASE("path utils: localhost file URI is local path", "[path][uri]") {
    CHECK(path_from_file_uri("file://localhost/tmp/lazyverilog/top.sv") ==
          "/tmp/lazyverilog/top.sv");
    CHECK(path_from_file_uri("file://localhost/C:/repo/rtl/top.sv") ==
          "C:/repo/rtl/top.sv");
}

#ifndef _WIN32
TEST_CASE("path utils: URI formatting preserves native Linux behavior", "[path][uri]") {
    const auto native = normalize_filesystem_path(std::filesystem::temp_directory_path() /
                                                  "lazyverilog_uri_check.sv");
    CHECK(uri_from_path(native) == "file://" + native.string());
}
#endif

#ifdef _WIN32
TEST_CASE("path utils: URI formatting uses Windows drive URI form", "[path][uri]") {
    CHECK(uri_from_path(std::filesystem::path("C:/repo/rtl/top.sv"))
              .starts_with("file:///C:/"));
}
#endif

// utf16_length() skips ASCII eight bytes at a time via a high-bit mask, so the
// cases that matter are the ones where a multi-byte sequence straddles or
// abuts that window.
TEST_CASE("utf16_length counts UTF-16 code units", "[string_utils][utf16]") {
    SECTION("pure ASCII is one unit per byte") {
        CHECK(utf16_length("") == 0);
        CHECK(utf16_length("a") == 1);
        CHECK(utf16_length("12345678") == 8);          // exactly one word
        CHECK(utf16_length("123456789") == 9);         // one word plus a byte
        CHECK(utf16_length("        $display(") == 17);
    }

    SECTION("BMP characters are one unit, however many bytes they take") {
        CHECK(utf16_length("\xEA\xB3\x84") == 1);              // 3-byte Hangul
        CHECK(utf16_length("\xC3\xA9") == 1);                  // 2-byte e-acute
        CHECK(utf16_length("\xEA\xB3\x84\xEC\x88\x98") == 2);  // 6 bytes, 2 units
    }

    SECTION("astral characters are a surrogate pair") {
        CHECK(utf16_length("\xF0\x9F\x98\x80") == 2); // U+1F600, 4 bytes
    }

    SECTION("a multi-byte sequence at every offset around the word boundary") {
        // The 8-byte skip must never consume part of a sequence.
        for (int pad = 0; pad <= 16; ++pad) {
            const std::string s = std::string((size_t)pad, 'x') + "\xEA\xB3\x84" + "yy";
            INFO("pad = " << pad);
            CHECK(utf16_length(s) == (size_t)pad + 1 + 2);
        }
    }

    SECTION("malformed UTF-8 counts as one unit per byte and terminates") {
        CHECK(utf16_length("\xFF") == 1);
        CHECK(utf16_length("a\x80\x80z") == 4);   // stray continuation bytes
        CHECK(utf16_length("\xEA\xB3") == 2);     // truncated sequence
    }
}

TEST_CASE("utf16_col_to_byte_offset inverts utf16_length", "[string_utils][utf16]") {
    const std::string line = "    $display(\"\xEA\xB3\x84\xEC\x88\x98\xEA\xB8\xB0\", r_cnt);";

    SECTION("round-trips at every column") {
        const size_t units = utf16_length(line);
        for (int col = 0; col <= (int)units; ++col) {
            const size_t off = utf16_col_to_byte_offset(line, 0, col);
            INFO("col = " << col);
            CHECK(utf16_length(std::string_view(line).substr(0, off)) == (size_t)col);
        }
    }

    SECTION("stops at a newline rather than running into the next line") {
        const std::string two = "ab\ncd";
        CHECK(utf16_col_to_byte_offset(two, 0, 5) == 2);
    }

    SECTION("a column past the end clamps to the end of the line") {
        CHECK(utf16_col_to_byte_offset("abc", 0, 99) == 3);
    }
}

TEST_CASE("normalize_filesystem_path resolves through a memoized parent", "[path]") {
    // The prefix is walked once per directory rather than once per file, which
    // is what stops a project of N files at depth D costing N*D metadata calls
    // against D distinct answers (measured: 439 readlinks for 61 files, 943 for
    // the same files eight directories deeper).  What must not change is the
    // answer: the last component is still resolved on its own, because a
    // symlinked source file is the case this function exists to collapse.
    const auto root = std::filesystem::temp_directory_path() / "lazyverilog-normalize-parent";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "real", ec);
    REQUIRE_FALSE(ec);
    { std::ofstream out(root / "real" / "actual.sv"); out << "module m; endmodule\n"; }

    const auto real = std::filesystem::canonical(root / "real" / "actual.sv");

    CHECK(normalize_filesystem_path(root / "real" / "actual.sv") == real);
    // A "." component addresses the parent rather than naming one.
    CHECK(normalize_filesystem_path(root / "real" / "." / "actual.sv") == real);
    // A file that does not exist keeps its name, appended to a resolved parent.
    CHECK(normalize_filesystem_path(root / "real" / "missing.sv") ==
          real.parent_path() / "missing.sv");

    // Symlinks need privileges on Windows, so their absence is not a failure.
    std::error_code link_ec;
    std::filesystem::create_symlink(root / "real" / "actual.sv", root / "aliased.sv", link_ec);
    if (!link_ec)
        CHECK(normalize_filesystem_path(root / "aliased.sv") == real);

    std::filesystem::create_directory_symlink(root / "real", root / "linkdir", link_ec);
    if (!link_ec)
        CHECK(normalize_filesystem_path(root / "linkdir" / "actual.sv") == real);

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("path utils: the normalization memo can be dropped when the tree moves", "[path]") {
    // The memo's premise is that a path which resolves on disk does not change
    // spelling while the server is alive.  That is true of a file being edited
    // and false of a tree being rearranged: repointing a symlink leaves every
    // path under it resolving to where it used to go, and two code paths
    // reaching one file then disagree about its URI -- the one thing
    // normalize_filesystem_path() exists to prevent.
    const auto root = std::filesystem::temp_directory_path() / "lazyverilog-memo-invalidate";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "ip_v1", ec);
    std::filesystem::create_directories(root / "ip_v2", ec);
    { std::ofstream out(root / "ip_v1" / "fifo.sv"); out << "module fifo; endmodule\n"; }
    { std::ofstream out(root / "ip_v2" / "fifo.sv"); out << "module fifo; endmodule\n"; }

    std::error_code link_ec;
    std::filesystem::create_directory_symlink(root / "ip_v1", root / "ip", link_ec);
    if (link_ec) {
        SUCCEED("symlinks unavailable here");
    }
    else {
        const auto through_link = root / "ip" / "fifo.sv";
        CHECK(normalize_filesystem_path(through_link) ==
              normalize_filesystem_path(root / "ip_v1" / "fifo.sv"));

        // The branch switch.
        std::filesystem::remove(root / "ip", ec);
        std::filesystem::create_directory_symlink(root / "ip_v2", root / "ip", link_ec);
        REQUIRE_FALSE(link_ec);

        // Still the old answer: that is the memo, and it is why an invalidation
        // hook has to exist at all.
        CHECK(normalize_filesystem_path(through_link) ==
              normalize_filesystem_path(root / "ip_v1" / "fifo.sv"));

        invalidate_normalized_path_cache();

        CHECK(normalize_filesystem_path(through_link) ==
              normalize_filesystem_path(root / "ip_v2" / "fifo.sv"));
    }
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("path utils: the normalization memo does not grow without bound", "[path]") {
    // Nothing else releases it -- the memo has no expiry, by design -- so a
    // server left running across many trees would hold every spelling it ever
    // saw.  The cap is generous enough that a real project never reaches it.
    const auto base = std::filesystem::temp_directory_path() / "lazyverilog-memo-cap";
    for (size_t i = 0; i < NormalizedPathCache::kMaxEntries + 64; ++i)
        (void)normalize_filesystem_path(base / ("m" + std::to_string(i) + ".sv"));

    auto& cache = normalized_path_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    CHECK(cache.entries.size() <= NormalizedPathCache::kMaxEntries);
    // And it is still a cache, not a disabled one.
    CHECK(cache.entries.size() > 0);
}

TEST_CASE("read_file_text_optional survives a path that is not a regular file", "[path]") {
    // A filelist entry naming a directory is a typo, and the background compiler
    // reads filelist entries on its own thread.  libstdc++ opens a directory
    // successfully and reports LLONG_MAX as its size, so sizing a buffer from
    // that seek throws bad_alloc where nothing catches it -- which is what this
    // did, through reserve(), before the read path was rewritten.
    const auto directory = std::filesystem::temp_directory_path();
    const auto text = read_file_text_optional(directory);
    // Empty or absent both mean "nothing to parse"; crashing does not.
    CHECK((!text || text->empty()));

    const auto missing = std::filesystem::temp_directory_path() / "lazyverilog-no-such-file.sv";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    CHECK_FALSE(read_file_text_optional(missing).has_value());

    const auto empty = std::filesystem::temp_directory_path() / "lazyverilog-empty.sv";
    { std::ofstream out(empty); }
    const auto empty_text = read_file_text_optional(empty);
    REQUIRE(empty_text.has_value());
    CHECK(empty_text->empty());
    std::filesystem::remove(empty, ec);
}

#ifndef _WIN32
TEST_CASE("read_file_text_optional does not block on a FIFO", "[path]") {
    // The other hazard the kind check exists for, and the one that decides how
    // the file is opened: a FIFO opens successfully and then blocks forever on a
    // writer that never comes.  A filelist naming one would hang an index worker
    // with no diagnostic at all.
    //
    // Answered from the handle rather than from the path -- O_NONBLOCK returns
    // immediately, fstat() says it is not a regular file, and the read never
    // happens.  A separate is_regular_file() would answer the same question at
    // the cost of resolving the path twice, which on a shared filesystem is a
    // round trip per component of every file in the project.
    //
    // POSIX only: mkfifo has no Windows equivalent, and that build keeps the
    // path check.
    const auto fifo = std::filesystem::temp_directory_path() / "lazyverilog-fifo.sv";
    std::error_code ec;
    std::filesystem::remove(fifo, ec);
    if (::mkfifo(fifo.c_str(), 0600) != 0)
        SUCCEED("mkfifo unavailable here");
    else {
        // Returns rather than hangs; the test timing out is the failure mode.
        CHECK_FALSE(read_file_text_optional(fifo).has_value());
        std::filesystem::remove(fifo, ec);
    }
}
#endif
