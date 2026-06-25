#include "config.hpp"
#include "filelist.hpp"

#include <catch2/catch_test_macros.hpp>

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
    setenv("LV_FILELIST_ENV_DIR", env_dir.c_str(), 1);

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
