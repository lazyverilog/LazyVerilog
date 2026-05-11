#include "analyzer.hpp"
#include "syntax_index.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>

std::shared_ptr<DocumentState> Analyzer::make_state(const std::string& uri,
                                                    const std::string& text) const {
    // Use name-only overload; no path to avoid SourceManager path-uniqueness conflicts
    // when the same URI is re-parsed on didChange.
    auto tree = slang::syntax::SyntaxTree::fromText(std::string_view(text), std::string_view(uri));
    auto state = std::make_shared<DocumentState>(uri, text, std::move(tree));
    // Format diagnostics immediately while the SyntaxTree arena is alive.
    // Do NOT copy slang::Diagnostic objects — their ConstantValue args can
    // contain internal pointers that are not safely copyable.
    if (state->tree) {
        const auto& diags = state->tree->diagnostics();
        std::cerr << "A\n";
        auto& sm = state->tree->sourceManager();
        std::cerr << "B\n";
        slang::DiagnosticEngine engine(sm);
        std::cerr << "C\n";
        for (const auto& d : diags) {
            std::cerr << "D\n";
            ParseDiagInfo info;
            try {
                std::cerr << "EEE\n";
                std::cerr << "G\n";
                std::cerr << state->tree.get() << "\n";
                // auto loc = d.location.valid() ? sm.getFullyExpandedLoc(d.location) : d.location;
                std::cerr << "FF\n";
                std::cerr << "FFFF\n";
                auto loc = d.location; // Crashing code.
                std::cerr << "F\n";
                if (loc.valid() && sm.isFileLoc(loc)) {
                    std::cerr << "G\n";
                    size_t ln = sm.getLineNumber(loc);
                    std::cerr << "H\n";
                    size_t col = sm.getColumnNumber(loc);
                    std::cerr << "I\n";
                    info.line = ln > 0 ? (int)ln - 1 : 0;
                    std::cerr << "J\n";
                    info.col = col > 0 ? (int)col - 1 : 0;
                    std::cerr << "K\n";
                }
            } catch (...) {
            }
            auto sev = slang::getDefaultSeverity(d.code);
            std::cerr << "L\n";
            if (sev == slang::DiagnosticSeverity::Error || sev == slang::DiagnosticSeverity::Fatal)
                info.severity = 1;
            else if (sev == slang::DiagnosticSeverity::Warning)
                info.severity = 2;
            else
                info.severity = 3;
            try {
                info.message = engine.formatMessage(d);
            } catch (...) {
                info.message = "(diagnostic format error)";
            }
            state->parse_diagnostics.push_back(std::move(info));
        }
    }
    return state;
}

void Analyzer::open(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_[uri] = state;
}

void Analyzer::change(const std::string& uri, const std::string& text) {
    auto state = make_state(uri, text);
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_[uri] = state;
}

void Analyzer::close(const std::string& uri) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    docs_.erase(uri);
}

std::shared_ptr<const DocumentState> Analyzer::get_state(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return nullptr;
    return it->second;
}

// Extract identifier at (0-based line, 0-based col) from source text.
static std::string extract_ident(std::string_view src, int line, int col) {
    int cur = 0;
    size_t pos = 0;
    while (pos < src.size() && cur < line) {
        if (src[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur < line)
        return {};

    size_t ls = pos;
    size_t le = src.find('\n', pos);
    if (le == std::string_view::npos)
        le = src.size();

    if (col < 0 || (size_t)col >= le - ls)
        return {};
    size_t ip = ls + col;

    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    if (!is_id(src[ip]))
        return {};

    size_t start = ip;
    while (start > ls && is_id(src[start - 1]))
        --start;
    size_t end = ip;
    while (end < le && is_id(src[end]))
        ++end;

    return std::string(src.substr(start, end - start));
}

std::optional<SymbolInfo> Analyzer::symbol_at(const std::string& uri, int line, int col) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    auto ident = extract_ident(state->text, line, col);
    if (ident.empty())
        return std::nullopt;

    auto idx = SyntaxIndex::build(*state->tree, state->text);

    for (const auto& m : idx.modules) {
        if (m.name == ident)
            return SymbolInfo{ident, "module", "", m.line, 0};
        for (const auto& p : m.ports)
            if (p.name == ident)
                return SymbolInfo{ident, "port", p.direction, p.line, 0};
    }
    for (const auto& inst : idx.instances)
        if (inst.instance_name == ident)
            return SymbolInfo{ident, "instance", inst.module_name, inst.line, 0};

    return SymbolInfo{ident, "unknown", "", line, col};
}

std::optional<Location> Analyzer::definition_of(const std::string& uri, int line, int col) const {
    auto state = get_state(uri);
    if (!state || !state->tree)
        return std::nullopt;

    auto ident = extract_ident(state->text, line, col);
    if (ident.empty())
        return std::nullopt;

    auto idx = SyntaxIndex::build(*state->tree, state->text);

    // Resolve instance module type → module declaration
    for (const auto& inst : idx.instances) {
        if (inst.module_name == ident || inst.instance_name == ident) {
            for (const auto& m : idx.modules)
                if (m.name == inst.module_name)
                    return Location{uri, m.line, 0};
        }
    }

    // Resolve module name → declaration
    for (const auto& m : idx.modules)
        if (m.name == ident)
            return Location{uri, m.line, 0};

    return std::nullopt;
}

std::vector<std::pair<int, int>> Analyzer::find_occurrences(const std::string& uri,
                                                            const std::string& name) const {
    auto state = get_state(uri);
    if (!state || name.empty())
        return {};

    std::string_view src = state->text;
    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };

    // Build line-start offsets
    std::vector<size_t> ls;
    ls.push_back(0);
    for (size_t i = 0; i < src.size(); ++i)
        if (src[i] == '\n')
            ls.push_back(i + 1);

    std::vector<std::pair<int, int>> result;
    size_t pos = 0;
    while (pos < src.size()) {
        auto found = src.find(name, pos);
        if (found == std::string_view::npos)
            break;

        bool before_ok = (found == 0) || !is_id(src[found - 1]);
        bool after_ok = (found + name.size() >= src.size()) || !is_id(src[found + name.size()]);

        if (before_ok && after_ok) {
            auto it = std::upper_bound(ls.begin(), ls.end(), found);
            int line = (int)(it - ls.begin()) - 1; // 0-based
            int col = (int)(found - ls[(size_t)line]);
            result.push_back({line, col});
        }
        pos = found + 1;
    }
    return result;
}

void Analyzer::set_extra_files(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    extra_files_ = paths;
}

void Analyzer::refresh_if_stale(const std::string& /*uri*/) {
    // TODO: implement mtime check in US-011
}
