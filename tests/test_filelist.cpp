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

TEST_CASE("filelist: * and ? expand to matching source files, sorted", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_star");
    write_text(root / "rtl/b.sv", "");
    write_text(root / "rtl/a.sv", "");
    write_text(root / "rtl/c.v", "");
    write_text(root / "rtl/ab.sv", "");
    write_text(root / "rtl/.hidden.sv", "");
    fs::create_directories(root / "rtl/dir.sv"); // a directory is not a source
    write_text(root / "top.vc",
               "rtl/*.sv\n"
               "rtl/?.v\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 4);
    CHECK(result.files[0] == norm(root / "rtl/a.sv"));
    CHECK(result.files[1] == norm(root / "rtl/ab.sv"));
    CHECK(result.files[2] == norm(root / "rtl/b.sv"));
    CHECK(result.files[3] == norm(root / "rtl/c.v"));
    REQUIRE(result.file_sizes.size() == result.files.size());
}

TEST_CASE("filelist: ** matches zero or more directories", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_globstar");
    write_text(root / "rtl/top.sv", "");
    write_text(root / "rtl/core/alu.sv", "");
    write_text(root / "rtl/core/deep/mul.sv", "");
    write_text(root / "rtl/core/deep/notes.txt", "");
    write_text(root / "rtl/.git/skip.sv", "");
    write_text(root / "top.vc", "rtl/**/*.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 3);
    CHECK(result.files[0] == norm(root / "rtl/core/alu.sv"));
    CHECK(result.files[1] == norm(root / "rtl/core/deep/mul.sv"));
    CHECK(result.files[2] == norm(root / "rtl/top.sv"));
}

TEST_CASE("filelist: a trailing ** names every file below it", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_trailing");
    write_text(root / "lib/a.sv", "");
    write_text(root / "lib/sub/b.sv", "");
    write_text(root / "top.vc", "-v lib/**\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "lib/a.sv"));
    CHECK(result.files[1] == norm(root / "lib/sub/b.sv"));
}

TEST_CASE("filelist: source globs keep only Verilog and SystemVerilog files",
          "[filelist][glob]") {
    // The examples of #212: `rtl/*` and `rtl/**` name the design's sources,
    // not every file beside them.  Headers are reached through `include.
    const auto root = make_temp_dir("lv_filelist_glob_sources");
    write_text(root / "rtl/top.sv", "");
    write_text(root / "rtl/legacy.v", "");
    write_text(root / "rtl/defs.svh", "");
    write_text(root / "rtl/README.md", "");
    write_text(root / "rtl/sub/counter.sv", "");
    write_text(root / "rtl/sub/notes.txt", "");
    write_text(root / "rtl/sub/files.f", "");
    write_text(root / "star.vc", "rtl/*\n");
    write_text(root / "globstar.vc", "-v rtl/**\n");
    write_text(root / "explicit.vc", "rtl/*.md\n");

    Config cfg;
    cfg.design.vcode = "star.vc";
    auto result = load_vcode(root, cfg);
    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "rtl/legacy.v"));
    CHECK(result.files[1] == norm(root / "rtl/top.sv"));

    cfg.design.vcode = "globstar.vc";
    result = load_vcode(root, cfg);
    REQUIRE(result.files.size() == 3);
    CHECK(result.files[0] == norm(root / "rtl/legacy.v"));
    CHECK(result.files[1] == norm(root / "rtl/sub/counter.sv"));
    CHECK(result.files[2] == norm(root / "rtl/top.sv"));

    // Even when asked for by name: a glob never makes a non-source a source.
    // A plain entry still names any file, as before.
    cfg.design.vcode = "explicit.vc";
    result = load_vcode(root, cfg);
    CHECK(result.files.empty());
}

TEST_CASE("filelist: bracket sets, ranges and negation", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_bracket");
    for (const char* name : {"u0.sv", "u1.sv", "u2.sv", "ua.sv", "u].sv"})
        write_text(root / "rtl" / name, "");
    write_text(root / "range.vc", "rtl/u[0-1].sv\n");
    write_text(root / "neg.vc", "rtl/u[!0-1].sv\n");
    write_text(root / "bracket.vc", "rtl/u[]a].sv\n");

    Config cfg;
    cfg.design.vcode = "range.vc";
    auto result = load_vcode(root, cfg);
    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "rtl/u0.sv"));
    CHECK(result.files[1] == norm(root / "rtl/u1.sv"));

    cfg.design.vcode = "neg.vc";
    result = load_vcode(root, cfg);
    REQUIRE(result.files.size() == 3);
    CHECK(result.files[0] == norm(root / "rtl/u2.sv"));
    CHECK(result.files[1] == norm(root / "rtl/u].sv"));
    CHECK(result.files[2] == norm(root / "rtl/ua.sv"));

    // A `]` first in the set is a member, not the end of it.
    cfg.design.vcode = "bracket.vc";
    result = load_vcode(root, cfg);
    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "rtl/u].sv"));
    CHECK(result.files[1] == norm(root / "rtl/ua.sv"));
}

TEST_CASE("filelist: a glob in a middle segment", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_middle");
    write_text(root / "ip/uart/rtl/top.sv", "");
    write_text(root / "ip/spi/rtl/top.sv", "");
    write_text(root / "ip/gpio/other/top.sv", "");
    write_text(root / "top.vc", "ip/*/rtl/top.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "ip/spi/rtl/top.sv"));
    CHECK(result.files[1] == norm(root / "ip/uart/rtl/top.sv"));
}

TEST_CASE("filelist: +incdir+ globs match directories only", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_incdir");
    fs::create_directories(root / "ip/uart/include");
    fs::create_directories(root / "ip/spi/include");
    write_text(root / "ip/gpio/include", ""); // a file, not a directory
    fs::create_directories(root / "common");
    write_text(root / "top.vc", "+incdir+ip/*/include+comm?n\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    CHECK(result.files.empty());
    REQUIRE(result.include_dirs.size() == 3);
    CHECK(result.include_dirs[0] == norm(root / "ip/spi/include"));
    CHECK(result.include_dirs[1] == norm(root / "ip/uart/include"));
    CHECK(result.include_dirs[2] == norm(root / "common"));
}

TEST_CASE("filelist: -f globs load every matching filelist", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_nested");
    write_text(root / "blocks/b/files.f", "b.sv\n");
    write_text(root / "blocks/a/files.f", "a.sv\n");
    write_text(root / "more/x.f", "x.sv\n");
    write_text(root / "top.vc",
               "-f blocks/*/files.f\n"
               "-f=more/*.f\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 3);
    CHECK(result.files[0] == norm(root / "blocks/a/a.sv"));
    CHECK(result.files[1] == norm(root / "blocks/b/b.sv"));
    CHECK(result.files[2] == norm(root / "more/x.sv"));
    // Each expanded filelist is reported, so a watcher event on it reloads.
    CHECK(result.filelists.size() == 4);
    CHECK(std::find(result.filelists.begin(), result.filelists.end(),
                    norm(root / "blocks/a/files.f")) != result.filelists.end());
}

TEST_CASE("filelist: a glob matching nothing contributes nothing", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_empty");
    write_text(root / "top.sv", "");
    write_text(root / "top.vc",
               "missing/*.sv\n"
               "nowhere/**/*.sv\n"
               "+incdir+none/*\n"
               "-f lists/*.f\n"
               "top.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "top.sv"));
    CHECK(result.include_dirs.empty());
    CHECK(result.filelists.size() == 1);
}

TEST_CASE("filelist: glob matches and explicit entries are deduplicated", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_dedup");
    write_text(root / "rtl/pkg.sv", "");
    write_text(root / "rtl/top.sv", "");
    write_text(root / "top.vc",
               "rtl/pkg.sv\n"
               "rtl/*.sv\n"
               "rtl/**/**/*.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "rtl/pkg.sv"));
    CHECK(result.files[1] == norm(root / "rtl/top.sv"));
}

TEST_CASE("filelist: a file literally named like a pattern is used when nothing matches",
          "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_literal");
    write_text(root / "rtl/top[0].sv", "");
    write_text(root / "top.vc", "rtl/top[0].sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "rtl/top[0].sv"));
}

TEST_CASE("filelist: globs combine with environment variables", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_env");
    const auto ext = root / "ext";
#ifdef _WIN32
    _putenv_s("LV_FILELIST_GLOB_DIR", ext.string().c_str());
#else
    setenv("LV_FILELIST_GLOB_DIR", ext.c_str(), 1);
#endif
    write_text(ext / "one.sv", "");
    write_text(ext / "two.sv", "");
    write_text(root / "top.vc", "${LV_FILELIST_GLOB_DIR}/*.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(ext / "one.sv"));
    CHECK(result.files[1] == norm(ext / "two.sv"));
}

TEST_CASE("filelist: wildcard characters in the filelist's own directory are literal",
          "[filelist][glob]") {
    // Only the entry as written is a pattern.  A checkout under `proj[v2]`
    // must neither turn plain entries into patterns (losing the missing-file
    // entry) nor make `rtl/*.sv` look for `projv` or `proj2`.
    const auto root = make_temp_dir("lv_filelist_glob_[v2]");
    write_text(root / "rtl/a.sv", "");
    write_text(root / "rtl/b.sv", "");
    write_text(root / "top.vc",
               "rtl/*.sv\n"
               "rtl/missing.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 3);
    CHECK(result.files[0] == norm(root / "rtl/a.sv"));
    CHECK(result.files[1] == norm(root / "rtl/b.sv"));
    // A plain entry is recorded even when missing, as before wildcards.
    CHECK(result.files[2] == norm(root / "rtl/missing.sv"));
}

TEST_CASE("filelist: .. after a wildcard needs the wildcard to match first",
          "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_dotdot");
    write_text(root / "ip/shared.sv", "");
    write_text(root / "top.vc", "ip/*/../shared.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    auto result = load_vcode(root, cfg);
    CHECK(result.files.empty()); // no directory under ip/ yet

    fs::create_directories(root / "ip/uart");
    result = load_vcode(root, cfg);
    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0] == norm(root / "ip/shared.sv"));
}

TEST_CASE("filelist: a glob ending in a separator matches directories only",
          "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_trailing_sep");
    write_text(root / "ip/a.sv", "");
    fs::create_directories(root / "ip/uart");
    write_text(root / "top.vc",
               "ip/*/\n"
               "+incdir+ip/*/\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    CHECK(result.files.empty());
    REQUIRE(result.include_dirs.size() == 1);
    CHECK(result.include_dirs[0] == norm(root / "ip/uart"));
}

TEST_CASE("filelist: non-ASCII names under a glob do not abort the filelist",
          "[filelist][glob]") {
    // On Windows a name outside the narrow code page cannot be spelled as a
    // std::string; meeting one during a walk must not lose the rest.
    const auto root = make_temp_dir("lv_filelist_glob_unicode");
    const auto unicode_dir = root / "rtl" / fs::path(u8"\uD55C\uAE00");
    fs::create_directories(unicode_dir);
    { std::ofstream(unicode_dir / fs::path(u8"\uBAA8\uB4C8.sv")); }
    write_text(root / "rtl/top.sv", "");
    write_text(root / "top.vc",
               "rtl/**/*.sv\n"
               "after.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    VcodeResult result;
    REQUIRE_NOTHROW(result = load_vcode(root, cfg));

    CHECK(std::find(result.files.begin(), result.files.end(), norm(root / "rtl/top.sv")) !=
          result.files.end());
    CHECK(std::find(result.files.begin(), result.files.end(), norm(root / "after.sv")) !=
          result.files.end());
}

TEST_CASE("filelist: ** through a link back to an ancestor ends and does not duplicate",
          "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_loop");
    write_text(root / "rtl/top.sv", "");
    write_text(root / "rtl/sub/alu.sv", "");
    std::error_code ec;
    fs::create_directory_symlink(root / "rtl", root / "rtl/sub/loop", ec);
#ifdef _WIN32
    // A symlink needs a privilege a junction does not, and a junction is the
    // link some runtimes report as a plain directory.
    if (ec) {
        const auto command = "mklink /J \"" + (root / "rtl" / "sub" / "loop").string() + "\" \"" +
                             (root / "rtl").string() + "\" >NUL";
        if (std::system(command.c_str()) == 0 && fs::exists(root / "rtl/sub/loop/top.sv"))
            ec.clear();
    }
#endif
    if (ec)
        SKIP("cannot create a directory link here: " << ec.message());
    write_text(root / "top.vc", "rtl/**/*.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0] == norm(root / "rtl/sub/alu.sv"));
    CHECK(result.files[1] == norm(root / "rtl/top.sv"));
}

#ifdef _WIN32
TEST_CASE("filelist: Windows globs ignore case, as the filesystem does", "[filelist][glob]") {
    const auto root = make_temp_dir("lv_filelist_glob_case");
    write_text(root / "RTL/Top.SV", "");
    write_text(root / "top.vc", "rtl/*.sv\n");

    Config cfg;
    cfg.design.vcode = "top.vc";
    const auto result = load_vcode(root, cfg);

    REQUIRE(result.files.size() == 1);
}
#endif
