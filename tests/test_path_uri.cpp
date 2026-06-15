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
