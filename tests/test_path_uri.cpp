#include "string_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

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
