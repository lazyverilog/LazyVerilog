#include "filelist.hpp"
#include "string_utils.hpp"

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace {

static std::optional<size_t> find_filelist_slash_comment(std::string_view line) {
    // Filelists use // for comments, but LSP/VS Code users sometimes paste
    // file URIs such as file:///C:/repo/rtl/top.sv.  Treat a double slash that
    // immediately follows ':' as part of a URI/scheme (or a Windows-ish path)
    // rather than as a comment opener.  Ordinary trailing comments still work:
    //
    //   rtl/top.sv // comment       -> comment
    //   file:///C:/repo/top.sv      -> path
    for (size_t pos = line.find("//"); pos != std::string_view::npos;
         pos = line.find("//", pos + 2)) {
        if (pos > 0 && line[pos - 1] == ':')
            continue;
        return pos;
    }
    return std::nullopt;
}

std::string expand_env_vars(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        if (text[i] != '$') {
            out.push_back(text[i++]);
            continue;
        }

        const size_t dollar = i++;
        std::string name;
        if (i < text.size() && text[i] == '{') {
            const size_t begin = ++i;
            while (i < text.size() && text[i] != '}')
                ++i;
            if (i >= text.size()) {
                out.append(text.substr(dollar));
                break;
            }
            name = std::string(text.substr(begin, i - begin));
            ++i;
        } else {
            const size_t begin = i;
            while (i < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (!(std::isalnum(c) || text[i] == '_'))
                    break;
                ++i;
            }
            if (begin == i) {
                out.push_back('$');
                continue;
            }
            name = std::string(text.substr(begin, i - begin));
        }

        if (const char* value = std::getenv(name.c_str()))
            out += value;
        else
            out.append(text.substr(dollar, i - dollar));
    }

    return out;
}

std::filesystem::path resolve_from_filelist_dir(const std::filesystem::path& filelist_dir,
                                                std::string_view text) {
    auto path = std::filesystem::path(path_from_file_uri(expand_env_vars(text)));
    if (path.is_relative())
        path = filelist_dir / path;
    return std::filesystem::absolute(path).lexically_normal();
}

// Backslashes are literal path characters (Windows paths such as
// C:\repo\top.sv), not escapes.  Line continuations are stripped before
// tokenizing; paths containing whitespace must be quoted.
std::vector<std::string> tokenize_filelist_line(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';

    for (const char c : line) {
        if (quote != '\0') {
            if (c == quote)
                quote = '\0';
            else
                current.push_back(c);
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        tokens.push_back(std::move(current));
    return tokens;
}

bool strip_line_continuation(std::string& line) {
    size_t end = line.find_last_not_of(" \t\r");
    if (end == std::string::npos || line[end] != '\\')
        return false;
    line.erase(end);
    return true;
}

bool option_takes_non_source_argument(std::string_view option) {
    static const std::unordered_set<std::string_view> options = {
        "-assert", "-cm",       "-fprofile-dir", "-kdb",    "-l",
        "-libmap", "-Mdir",     "-ntb_opts",     "-o",      "-P",
        "-top",    "-timescale", "-work",         "-y",
    };
    return options.contains(option);
}

struct VcodeLoader {
    VcodeResult result;
    std::unordered_set<std::string> visited_filelists;
    std::unordered_set<std::string> seen_files;
    std::unordered_set<std::string> seen_include_dirs;
};

void warn_missing(const std::filesystem::path& path, const std::filesystem::path& filelist) {
    std::cerr << "[lazyverilog] warning: " << path.string() << " (referenced in "
              << filelist.string() << ") not found on disk\n";
}

void add_file(VcodeLoader& loader, std::string path) {
    if (loader.seen_files.insert(path).second)
        loader.result.files.push_back(std::move(path));
}

void add_file_checked(VcodeLoader& loader, const std::filesystem::path& path,
                      const std::filesystem::path& filelist) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        warn_missing(path, filelist);
    add_file(loader, path.string());
}

void add_include_dir(VcodeLoader& loader, std::string dir) {
    if (loader.seen_include_dirs.insert(dir).second)
        loader.result.include_dirs.push_back(std::move(dir));
}

void load_vcode_file(const std::filesystem::path& filelist, VcodeLoader& loader) {
    const auto normalized_filelist =
        std::filesystem::absolute(filelist).lexically_normal().string();
    // The visited set is persistent, not a recursion stack: it breaks -f
    // cycles and also skips re-parsing a filelist reachable from two parents.
    if (!loader.visited_filelists.insert(normalized_filelist).second)
        return;

    std::ifstream input(filelist);
    if (!input) {
        std::cerr << "[lazyverilog] warning: filelist " << filelist.string()
                  << " not found or unreadable\n";
        return;
    }

    const auto filelist_dir = filelist.parent_path();
    std::string line;
    std::string logical_line;
    while (std::getline(input, line)) {
        if (strip_line_continuation(line)) {
            logical_line += line;
            logical_line += ' ';
            continue;
        }
        if (!logical_line.empty()) {
            logical_line += line;
            line = std::move(logical_line);
            logical_line.clear();
        }

        // Strip // and # comments
        if (auto pos = find_filelist_slash_comment(line))
            line.erase(*pos);
        if (auto pos = line.find('#'); pos != std::string::npos)
            line.erase(pos);

        auto item = trim_copy(line);
        if (item.empty())
            continue;

        const auto tokens = tokenize_filelist_line(item);
        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            if (token.empty())
                continue;

            // Nested filelists:
            //   -f child.f
            //   -F child.f
            //   -fchild.f
            //   -Fchild.f
            //   -f=child.f
            // Paths are resolved relative to the filelist that contains the -f.
            if (token == "-f" || token == "-F") {
                if (i + 1 < tokens.size())
                    load_vcode_file(resolve_from_filelist_dir(filelist_dir, tokens[++i]),
                                    loader);
                continue;
            }
            if ((token.starts_with("-f") || token.starts_with("-F")) && token.size() > 2) {
                std::string_view nested_text(token);
                nested_text.remove_prefix(2);
                if (nested_text.starts_with("="))
                    nested_text.remove_prefix(1);
                if (!nested_text.empty()) {
                    load_vcode_file(resolve_from_filelist_dir(filelist_dir, nested_text),
                                    loader);
                    continue;
                }
            }

            // Recognize simulator-style include-directory entries in the
            // filelist. The directory is not parsed as a source file; it is
            // passed to slang's SourceManager so explicit source files can
            // resolve `include "...".
            //
            // Supported forms:
            //   +incdir+rtl/include
            //   +incdir+/abs/include
            //   +incdir+dir_a+dir_b
            if (token.starts_with("+incdir+")) {
                std::string_view rest(token);
                rest.remove_prefix(std::string_view("+incdir+").size());
                while (!rest.empty()) {
                    const auto plus = rest.find('+');
                    auto dir_text =
                        plus == std::string_view::npos ? rest : rest.substr(0, plus);
                    if (!dir_text.empty()) {
                        add_include_dir(
                            loader,
                            resolve_from_filelist_dir(filelist_dir, dir_text).string());
                    }
                    if (plus == std::string_view::npos)
                        break;
                    rest.remove_prefix(plus + 1);
                }
                continue;
            }

            // `-v file.v` names a Verilog library source file.  Index it as a
            // source file because it can contain module/package declarations
            // needed by navigation and lint.
            if (token == "-v") {
                if (i + 1 < tokens.size())
                    add_file_checked(loader, resolve_from_filelist_dir(filelist_dir, tokens[++i]),
                                     filelist);
                continue;
            }

            // Skip other compiler options / flags.  Some options have a
            // following non-source argument; consume those explicitly so they
            // are not accidentally treated as source paths.
            if (token.starts_with("-")) {
                if (option_takes_non_source_argument(token) && i + 1 < tokens.size())
                    ++i;
                continue;
            }
            if (token.starts_with("+"))
                continue;

            add_file_checked(loader, resolve_from_filelist_dir(filelist_dir, token), filelist);
        }
    }
}

} // namespace

std::string resolve_vcode_path(const std::filesystem::path& root, const Config& config) {
    if (config.design.vcode.empty())
        return {};
    auto filelist =
        std::filesystem::path(path_from_file_uri(expand_env_vars(config.design.vcode)));
    if (filelist.is_relative())
        filelist = root / filelist;
    return std::filesystem::absolute(filelist).lexically_normal().string();
}

VcodeResult load_vcode(const std::filesystem::path& root, const Config& config) {
    if (config.design.vcode.empty())
        return {};

    VcodeLoader loader;
    load_vcode_file(std::filesystem::path(resolve_vcode_path(root, config)), loader);
    return std::move(loader.result);
}
