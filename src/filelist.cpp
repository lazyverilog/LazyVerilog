#include "filelist.hpp"
#include "string_utils.hpp"

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
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
    auto path = std::filesystem::path(expand_env_vars(text));
    if (path.is_relative())
        path = filelist_dir / path;
    return std::filesystem::absolute(path).lexically_normal();
}

std::vector<std::string> tokenize_filelist_line(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    bool escape = false;

    for (const char c : line) {
        if (escape) {
            current.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
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
    if (escape)
        current.push_back('\\');
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

void load_vcode_file(const std::filesystem::path& filelist,
                     VcodeResult& result,
                     std::unordered_set<std::string>& active_filelists) {
    const auto normalized_filelist =
        std::filesystem::absolute(filelist).lexically_normal().string();
    if (!active_filelists.insert(normalized_filelist).second)
        return;

    std::ifstream input(filelist);
    if (!input) {
        active_filelists.erase(normalized_filelist);
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
                                    result, active_filelists);
                continue;
            }
            if ((token.starts_with("-f") || token.starts_with("-F")) && token.size() > 2) {
                std::string_view nested_text(token);
                nested_text.remove_prefix(2);
                if (nested_text.starts_with("="))
                    nested_text.remove_prefix(1);
                if (!nested_text.empty()) {
                    load_vcode_file(resolve_from_filelist_dir(filelist_dir, nested_text),
                                    result, active_filelists);
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
                        result.include_dirs.push_back(
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
                    result.files.push_back(
                        resolve_from_filelist_dir(filelist_dir, tokens[++i]).string());
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

            result.files.push_back(resolve_from_filelist_dir(filelist_dir, token).string());
        }
    }

    active_filelists.erase(normalized_filelist);
}

} // namespace

std::string resolve_vcode_path(const std::filesystem::path& root, const Config& config) {
    if (config.design.vcode.empty())
        return {};
    auto filelist = std::filesystem::path(expand_env_vars(config.design.vcode));
    if (filelist.is_relative())
        filelist = root / filelist;
    return std::filesystem::absolute(filelist).lexically_normal().string();
}

VcodeResult load_vcode(const std::filesystem::path& root, const Config& config) {
    VcodeResult result;
    if (config.design.vcode.empty())
        return result;

    std::unordered_set<std::string> active_filelists;
    load_vcode_file(std::filesystem::path(resolve_vcode_path(root, config)),
                    result, active_filelists);
    return result;
}
