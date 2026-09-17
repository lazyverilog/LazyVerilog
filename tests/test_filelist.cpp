#include "config.hpp"
#include "filelist.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const std::string& name) {
    auto dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

std::string norm(const fs::path& path) {
    return fs::absolute(path).lexically_normal().string();
}

} // namespace

TEST_CASE("filelist: nested -f entries are resolved relative to parent filelist",
          "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_nested");
    write_text(root / "top.vc",
               "rtl/top.sv\n"
               "-f lists/child.vc\n");
    write_text(root / "lists/child.vc",
               "child.sv\n"
               "+incdir+inc\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "rtl/top.sv"));
    CHECK(result.files[1] == norm(root / "lists/child.sv"));
    REQUIRE(result.include_dirs.size() == 1);
    CHECK(result.include_dirs[0] == norm(root / "lists/inc"));
}

TEST_CASE("filelist: nested -F entries are accepted like -f", "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_upper_f");
    write_text(root / "top.vc", "-F lists/child.vc\n");
    write_text(root / "lists/child.vc", "child.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "lists/child.sv"));
}

TEST_CASE("filelist: nested -f and source paths expand environment variables",
          "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_env");
    const auto env_dir = root / "env";
#ifdef _WIN32
    _putenv_s("LV_FILELIST_ENV_DIR", env_dir.string().c_str());
#else
    setenv("LV_FILELIST_ENV_DIR", env_dir.c_str(), 1);
#endif

    write_text(root / "top.vc", "-f $LV_FILELIST_ENV_DIR/nested.vc\n");
    write_text(env_dir / "nested.vc", "${LV_FILELIST_ENV_DIR}/from_env.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(env_dir / "from_env.sv"));
}

TEST_CASE("filelist: recursive -f cycles do not loop forever", "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_cycle");
    write_text(root / "top.vc",
               "top.sv\n"
               "-f child.vc\n");
    write_text(root / "child.vc",
               "child.sv\n"
               "-f top.vc\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "top.sv"));
    CHECK(result.files[1] == norm(root / "child.sv"));
}

TEST_CASE("filelist: non -f compiler flags remain ignored", "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_flags");
    write_text(root / "top.vc",
               "-full64\n"
               "-timescale=1ns/1ps\n"
               "top.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "top.sv"));
}

TEST_CASE("filelist: source paths survive mixed compiler options on one line",
          "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_mixed_options");
    write_text(root / "top.vc",
               "-sv rtl/top.sv +incdir+inc -f lists/child.vc\n"
               "-timescale 1ns/1ps rtl/after_timescale.sv\n"
               "-v lib/libcell.v\n");
    write_text(root / "lists/child.vc",
               "-sverilog child.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 4);
    CHECK(result.files[0] == norm(root / "rtl/top.sv"));
    CHECK(result.files[1] == norm(root / "lists/child.sv"));
    CHECK(result.files[2] == norm(root / "rtl/after_timescale.sv"));
    CHECK(result.files[3] == norm(root / "lib/libcell.v"));
    REQUIRE(result.include_dirs.size() == 1);
    CHECK(result.include_dirs[0] == norm(root / "inc"));
}

TEST_CASE("filelist: quoted paths and line continuations are tokenized",
          "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_quotes_continuation");
    write_text(root / "top.vc",
               "-sv \"rtl/top file.sv\" \\\n"
               "    '+incdir+include dir'\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "rtl/top file.sv"));
    REQUIRE(result.include_dirs.size() == 1);
    CHECK(result.include_dirs[0] == norm(root / "include dir"));
}

TEST_CASE("filelist: file:// URI entries and vcode are converted to paths",
          "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_uri");
    write_text(root / "top.vc",
               "file://" + norm(root / "rtl/top.sv") + "\n"
               "+incdir+file://" + norm(root / "inc") + "\n");

    Config cfg;
    cfg.design.vcode = "file://" + norm(root / "top.vc");
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "rtl/top.sv"));
    REQUIRE(result.include_dirs.size() == 1);
    CHECK(result.include_dirs[0] == norm(root / "inc"));
}

TEST_CASE("filelist: backslashes in paths are literal, not escapes", "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_backslash");
    write_text(root / "top.vc", "C:\\repo\\top.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "C:\\repo\\top.sv"));
}

TEST_CASE("filelist: filelist included from two parents is loaded once",
          "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_diamond");
    write_text(root / "top.vc",
               "-f a.vc\n"
               "-f b.vc\n");
    write_text(root / "a.vc", "-f common.vc\n");
    write_text(root / "b.vc", "-f common.vc\n");
    write_text(root / "common.vc",
               "common.sv\n"
               "+incdir+inc\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "common.sv"));
    REQUIRE(result.include_dirs.size() == 1);
    CHECK(result.include_dirs[0] == norm(root / "inc"));
}

TEST_CASE("filelist: repeated source and incdir entries are deduplicated",
          "[filelist]") {
    const auto root = make_temp_dir("lv_filelist_dedup");
    write_text(root / "top.vc",
               "dup.sv\n"
               "+incdir+inc\n"
               "dup.sv\n"
               "+incdir+inc\n"
               "other.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "dup.sv"));
    CHECK(result.files[1] == norm(root / "other.sv"));
    REQUIRE(result.include_dirs.size() == 1);
    CHECK(result.include_dirs[0] == norm(root / "inc"));
}

TEST_CASE("filelist: each recorded source carries the size read while it was recorded",
          "[filelist]") {
    // The loader stats every entry it records, to warn about one that is not on
    // disk.  Keeping that number is what lets the background index queue sort
    // largest-first without a second metadata pass over the whole project, so
    // the two vectors have to stay aligned -- including across dedup, which
    // drops an entry from both.
    const auto root = make_temp_dir("lv_filelist_sizes");
    write_text(root / "small.sv", "module s; endmodule\n");
    write_text(root / "big.sv", std::string(4096, '\n'));
    write_text(root / "top.vc",
               "small.sv\n"
               "big.sv\n"
               "small.sv\n"   // duplicate: recorded once, stat'd once
               "missing.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 3);
    REQUIRE(result.file_sizes.size() == result.files.size());
    CHECK(result.files[0] == norm(root / "small.sv"));
    CHECK(result.files[1] == norm(root / "big.sv"));
    CHECK(result.files[2] == norm(root / "missing.sv"));

    CHECK(result.file_sizes[0] == fs::file_size(root / "small.sv"));
    CHECK(result.file_sizes[1] == fs::file_size(root / "big.sv"));
    // A path that cannot be stat'd sorts last rather than guessing a size.
    CHECK(result.file_sizes[2] == 0);
}

TEST_CASE("filelist: every filelist read is reported, -f chain included", "[filelist]") {
    // `.f` and `.vf` are in the watcher globs the server registers, so a client
    // has always reported a filelist edited by a branch switch or a generator.
    // Nothing knew which paths were filelists, though, so the report fell
    // through to the per-file shard refresh, which looked the path up as a
    // project source, found nothing, and dropped it -- and the project went on
    // indexing the list as it stood at launch for the rest of the session.
    //
    // The whole chain, not just the one the config names: a `-f` include is
    // just as capable of gaining or losing a source file.
    const auto root = make_temp_dir("lv_filelist_reported");
    write_text(root / "top.vc",
               "rtl/top.sv\n"
               "-f lists/child.vc\n");
    write_text(root / "lists/child.vc", "child.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.filelists.size() == 2);
    // Sorted, so two launches over one tree report them in one order.
    CHECK(std::is_sorted(result.filelists.begin(), result.filelists.end()));
    CHECK(std::find(result.filelists.begin(), result.filelists.end(), norm(root / "top.vc")) !=
          result.filelists.end());
    CHECK(std::find(result.filelists.begin(), result.filelists.end(),
                    norm(root / "lists" / "child.vc")) != result.filelists.end());
}
