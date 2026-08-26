#include "compilation_builder.hpp"
#include "string_utils.hpp"
#include "syntax_index_shared.hpp"

#include <iostream>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/SyntaxTree.h>
#include <unordered_set>

BuiltCompilation build_compilation(const CompilationSnapshot& snapshot,
                                   const slang::ast::CompilationOptions& options,
                                   bool log_skipped) {
    BuiltCompilation built;
    built.source_manager = make_lsp_source_manager();
    for (const auto& dir : snapshot.include_dirs) {
        if (!dir.empty())
            (void)built.source_manager->addUserDirectories(dir);
    }

    slang::parsing::PreprocessorOptions preprocessor_options;
    preprocessor_options.predefines = snapshot.defines;

    built.bag = std::make_unique<slang::Bag>();
    built.bag->set(preprocessor_options);
    built.bag->set(options);

    built.compilation = std::make_unique<slang::ast::Compilation>(*built.bag);

    std::unordered_set<std::string> assigned_paths;
    size_t scanned_buffer_count = 0;

    auto add_new_assigned_paths = [&] {
        const auto buffers = built.source_manager->getAllBuffers();
        // SourceManager buffer IDs are append-only for this compilation.  Do
        // not clear and rebuild the whole set after each syntax tree: on large
        // filelists that turns a simple duplicate check into O(n²) allocator
        // churn.  Scan only buffers that appeared since the previous tree.
        for (size_t i = scanned_buffer_count; i < buffers.size(); ++i) {
            auto buffer = buffers[i];
            const auto& path = built.source_manager->getFullPath(buffer);
            if (!path.empty())
                assigned_paths.insert(normalize_filesystem_path(path).string());
        }
        scanned_buffer_count = buffers.size();
    };

    for (const auto& file : snapshot.files) {
        const auto normalized_path = normalize_filesystem_path(file.path).string();
        if (assigned_paths.contains(normalized_path))
            continue;

        std::string text = file.text ? *file.text : read_file_text_or_empty(file.path);
        if (text.empty() && !file.text)
            continue;

        if (built.first_uri.empty())
            built.first_uri = file.uri;

        try {
            auto tree = slang::syntax::SyntaxTree::fromText(
                std::string_view(text), *built.source_manager, std::string_view(file.uri),
                std::string_view(file.path), *built.bag);
            built.compilation->addSyntaxTree(std::move(tree));
            add_new_assigned_paths();
        } catch (const std::exception& e) {
            if (log_skipped) {
                std::cerr << "[lazyverilog] semantic compile skipped " << file.path << ": "
                          << e.what() << "\n";
            }
            add_new_assigned_paths();
        }
    }

    return built;
}
