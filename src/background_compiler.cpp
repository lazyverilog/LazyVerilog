#include "background_compiler.hpp"
#include "lsp_position.hpp"
#include "cpu_budget.hpp"
#include "syntax_index_shared.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <slang/ast/Compilation.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <slang/util/Bag.h>
#include <unordered_set>

namespace {

using Clock = std::chrono::steady_clock;

// Keep optional semantic compilation conservative.  This is a background
// diagnostic path, not a throughput benchmark; every worker compiles the whole
// design, so extra workers multiply peak memory rather than splitting work.
constexpr int kMaxBackgroundCompilerThreads = 4;

// Semantic compilation is the most deferrable work the server does: nothing
// waits on it, and it is the heaviest thing running.  Yield harder than the
// project indexer, which gates when cross-file features start answering.
constexpr int kBackgroundCompilerNiceValue = 10;

static std::string diagnostic_uri(const slang::SourceManager& sm, const std::string& fallback_uri,
                                  slang::SourceLocation location) {
    if (!location.valid())
        return fallback_uri;

    try {
        auto uri = uri_from_source_location(sm, location);
        return uri.empty() ? fallback_uri : uri;
    } catch (...) {
        return fallback_uri;
    }
}

static ParseDiagInfo convert_diagnostic(const slang::SourceManager& sm,
                                        slang::DiagnosticEngine& engine,
                                        const slang::Diagnostic& diagnostic,
                                        const std::string& fallback_uri, std::string& uri) {
    ParseDiagInfo info;
    try {
        auto loc = diagnostic.location.valid() ? sm.getFullyExpandedLoc(diagnostic.location)
                                               : diagnostic.location;
        uri = diagnostic_uri(sm, fallback_uri, loc);
        if (loc.valid() && sm.isFileLoc(loc)) {
            const size_t line = sm.getLineNumber(loc);
            info.line = line > 0 ? static_cast<int>(line) - 1 : 0;
            info.col = lsp_column(sm, loc);
        }
    } catch (...) {
        uri = fallback_uri;
    }

    auto sev = slang::getDefaultSeverity(diagnostic.code);
    if (sev == slang::DiagnosticSeverity::Error || sev == slang::DiagnosticSeverity::Fatal)
        info.severity = 1;
    else if (sev == slang::DiagnosticSeverity::Warning)
        info.severity = 2;
    else
        info.severity = 3;

    try {
        info.message = engine.formatMessage(diagnostic);
    } catch (...) {
        info.message = "(semantic diagnostic format error)";
    }
    return info;
}

} // namespace

struct BackgroundCompiler::WorkerSlot {
    explicit WorkerSlot(size_t worker_id) : id(worker_id) {}

    // Monotonic debug identity.  Do not use this as the desired worker index:
    // workers can retire and be erased, then later workers may be spawned with
    // larger ids.  The explicit retire flag is the source of truth.
    size_t id{0};

    // Guarded by BackgroundCompiler::mutex_.  configure() sets retire when the
    // configured worker count shrinks.  The worker checks it only between jobs,
    // so currently-running slang compilation is allowed to finish cleanly.
    bool retire{false};

    // Guarded by BackgroundCompiler::mutex_.  Set immediately before the worker
    // thread returns; configure() can then join and erase the thread without
    // blocking on long background compilation work.
    bool exited{false};

    std::thread thread;
};

BackgroundCompiler::BackgroundCompiler(SnapshotCallback snapshot_callback,
                                       ResultCallback result_callback)
    : snapshot_callback_(std::move(snapshot_callback)),
      result_callback_(std::move(result_callback)) {}

BackgroundCompiler::~BackgroundCompiler() { stop(); }

std::vector<std::thread> BackgroundCompiler::collect_exited_workers_locked() {
    std::vector<std::thread> exited_threads;

    auto it = workers_.begin();
    while (it != workers_.end()) {
        auto& slot = *it;
        if (!slot->exited) {
            ++it;
            continue;
        }

        if (slot->thread.joinable())
            exited_threads.push_back(std::move(slot->thread));
        it = workers_.erase(it);
    }

    return exited_threads;
}

void BackgroundCompiler::configure(BackgroundCompilerConfig config) {
    config.thread_count = std::clamp(config.thread_count, 1, kMaxBackgroundCompilerThreads);
    config.debounce_ms = std::max(0, config.debounce_ms);

    std::vector<std::thread> exited_threads;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        enabled_ = config.enabled;
        log_timing_ = config.log_timing;
        error_limit_ = config.error_limit;
        debounce_ms_ = config.debounce_ms;

        exited_threads = collect_exited_workers_locked();

        if (!enabled_) {
            pending_ = false;
            ++latest_generation_;
            lock.unlock();
            cv_.notify_all();
        } else {
            // If the user raises the count again before previously-retired
            // workers have exited, keep those workers instead of spawning an
            // unnecessary replacement.  Retire requests are only a graceful
            // shrink mechanism, not an irreversible cancellation.
            if (static_cast<int>(workers_.size()) <= config.thread_count) {
                for (auto& slot : workers_)
                    slot->retire = false;
            }

            while (static_cast<int>(workers_.size()) < config.thread_count) {
                auto slot = std::make_shared<WorkerSlot>(next_worker_id_++);
                slot->thread = std::thread([this, slot] {
                    apply_background_thread_nice(kBackgroundCompilerNiceValue);
                    worker_loop(std::move(slot));
                });
                workers_.push_back(std::move(slot));
            }

            // Graceful shrink: mark newest excess workers for retirement.  An
            // idle worker exits immediately after the notify below; a busy
            // worker exits after publishing/skipping its current generation and
            // before taking another pending snapshot.
            int kept = 0;
            for (auto& slot : workers_) {
                if (slot->exited)
                    continue;
                if (kept < config.thread_count) {
                    ++kept;
                    continue;
                }
                slot->retire = true;
            }

            lock.unlock();
            cv_.notify_all();
        }
    }

    // Join only workers that have already reported exit.  This keeps config
    // reload responsive: lowering the thread count does not block on a large
    // in-progress semantic compile; cleanup happens on a later configure() or
    // stop().
    for (auto& worker : exited_threads) {
        if (worker.joinable())
            worker.join();
    }
}

void BackgroundCompiler::schedule() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || stopping_)
        return;

    pending_ = true;
    due_time_ = Clock::now() + std::chrono::milliseconds(debounce_ms_);
    ++latest_generation_;
    cv_.notify_all();
}

void BackgroundCompiler::stop() {
    std::vector<std::thread> workers_to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        enabled_ = false;
        pending_ = false;
        ++latest_generation_;

        // Move thread handles out while holding mutex_ so configure() cannot
        // concurrently erase or append workers_ while stop() is deciding what
        // must be joined.  The worker threads keep their WorkerSlot alive via
        // the shared_ptr captured by the thread function; clearing this vector
        // only drops the server's bookkeeping references.
        workers_to_join.reserve(workers_.size());
        for (auto& slot : workers_) {
            if (slot->thread.joinable())
                workers_to_join.push_back(std::move(slot->thread));
        }
        workers_.clear();
    }
    cv_.notify_all();
    for (auto& worker : workers_to_join) {
        if (worker.joinable())
            worker.join();
    }
}

void BackgroundCompiler::worker_loop(std::shared_ptr<WorkerSlot> slot) {
    while (true) {
        uint64_t generation = 0;
        CompilationSnapshot snapshot;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return stopping_ || slot->retire || (enabled_ && pending_);
            });
            if (stopping_ || slot->retire) {
                slot->exited = true;
                return;
            }

            while (!stopping_ && !slot->retire && pending_) {
                if (cv_.wait_until(lock, due_time_) == std::cv_status::timeout)
                    break;
            }
            if (stopping_ || slot->retire) {
                slot->exited = true;
                return;
            }
            if (!enabled_ || !pending_)
                continue;

            generation = latest_generation_;
            pending_ = false;
        }

        // Build the full design snapshot only after debounce has elapsed.  This
        // avoids walking the whole filelist/open-document set for every
        // keystroke when rapid edits will be coalesced anyway.
        if (snapshot_callback_)
            snapshot = snapshot_callback_();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || generation != latest_generation_)
                continue;
        }

        auto result = compile(generation, std::move(snapshot));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || generation != latest_generation_)
                continue;
        }
        result_callback_(std::move(result));
    }
}

/// Compile one group into @p result.
///
/// @p group references @p snapshot's files by index; the snapshot owns them.
///
/// The snapshot's parse inputs are only consulted for the ungrouped fallback,
/// where the files can span projects; a real project group is one project by
/// construction, so it needs no source libraries to keep names apart.  An empty
/// `group.root` is what says which of the two this is.
void BackgroundCompiler::compile_group(const CompilationSnapshot& snapshot,
                                       const CompilationGroup& group,
                                       BackgroundCompileResult& result) const {
    const auto start = Clock::now();
    const auto& parse_inputs = snapshot.parse_inputs;
    const auto file_at = [&](uint32_t index) -> const CompilationSourceFile& {
        return snapshot.files[index];
    };
    auto source_manager = make_lsp_source_manager();
    for (const auto& dir : group.include_dirs) {
        if (!dir.empty())
            (void)source_manager->addUserDirectories(dir);
    }

    slang::parsing::PreprocessorOptions preprocessor_options;
    preprocessor_options.predefines = group.defines;

    slang::ast::CompilationOptions compilation_options;
    compilation_options.flags |= slang::ast::CompilationFlags::LintMode;
    // slang treats 0 as "no limit", which is exactly the encoding callers use,
    // so this passes straight through.
    compilation_options.errorLimit = error_limit_.load(std::memory_order_relaxed);

    slang::Bag bag;
    // Moved, not copied: `Bag::set` stores by value into a `std::any`, and
    // PreprocessorOptions carries the whole `+incdir+` list.  Neither is read
    // again after this.
    bag.set(std::move(preprocessor_options));
    bag.set(std::move(compilation_options));

    // One slang source library per project, on the ungrouped fallback only.
    //
    // That path is a single Compilation over every file the analyzer knows
    // about -- semantic compilation cannot be per file, because it has one
    // preprocessor for all of it -- and SystemVerilog's module namespace is flat
    // and global.  Two projects that both declare `fifo` are therefore a
    // redefinition to slang: it says so and keeps one of them, and the other
    // project's semantic diagnostics disappear along with its definition --
    // measured, not feared; see the `[module-proximity]` compilation case.
    //
    // Libraries are the language's own answer, and slang implements it: a name
    // declared in two libraries is legal and kept in priority order, and only a
    // duplicate *within* one library is reported (Compilation::createDefinition).
    //
    // `isDefault` is set on every one of them, which is not the flag's usual
    // sense and is load-bearing.  slang skips library definitions when it picks
    // what to elaborate -- "Library definitions are never automatically
    // instantiated in any capacity" -- so naming a library without this would
    // leave a multi-project session with no top-level modules and therefore no
    // semantic diagnostics at all, which is a far worse answer than the warning
    // this removes.
    //
    // Priority follows sorted root order, not the order files arrive in: the
    // snapshot's open buffers come out of a hash map, and priority is what
    // decides which definition a lookup takes, so seeding it from iteration
    // order would let one session disagree with the next about which `fifo` a
    // diagnostic is about.
    //
    // Below two projects nothing is assigned at all.  A lone project's files go
    // into slang's own default library exactly as they did before, so the
    // overwhelmingly common session is bit-for-bit unchanged.
    //
    // Declared *before* the Compilation, and that order is load-bearing: locals
    // are destroyed in reverse, and the Compilation holds pointers into these
    // for its whole life, its destructor included.
    std::unordered_map<std::string, const slang::SourceLibrary*> library_by_path;
    std::vector<std::unique_ptr<slang::SourceLibrary>> libraries;
    // Only the ungrouped fallback can span projects.  A group with a root is one
    // project by construction -- that is what the grouping decided -- so asking
    // the resolver about its files could only ever rediscover that root, at the
    // price of an upward directory walk and a path normalization per filelist
    // entry on every debounced compile.
    if (parse_inputs && group.root.empty()) {
        // Sorted, so iteration order *is* priority order: the snapshot's open
        // buffers come out of a hash map, and priority decides which definition
        // a lookup takes.
        // One resolve per file, kept.  The set is the dedup and the priority
        // order at once, and root_of_file remembers the answer so the
        // assignment below does not walk the filesystem a second time for
        // every file.
        std::set<std::string> roots;
        std::vector<std::string> root_of_file;
        root_of_file.reserve(group.files.size());
        for (const auto index : group.files) {
            auto root = parse_inputs->project_root_for(file_at(index).path).string();
            if (!root.empty())
                roots.insert(root);
            root_of_file.push_back(std::move(root));
        }

        if (roots.size() > 1) {
            std::unordered_map<std::string, const slang::SourceLibrary*> library_by_root;
            libraries.reserve(roots.size());
            for (const auto& root : roots) {
                auto library = std::make_unique<slang::SourceLibrary>(
                    std::string(root), static_cast<int>(libraries.size()));
                library->isDefault = true;
                library_by_root.emplace(root, library.get());
                libraries.push_back(std::move(library));
            }
            for (size_t slot = 0; slot < group.files.size(); ++slot) {
                const auto it = library_by_root.find(root_of_file[slot]);
                if (it == library_by_root.end())
                    continue;
                // Keyed the way the parse loop below spells the same file, so a
                // path that arrives unnormalized cannot silently miss its library.
                library_by_path.emplace(
                    normalize_filesystem_path(file_at(group.files[slot]).path).string(), it->second);
            }
        }
    }

    slang::ast::Compilation compilation(bag);

    std::string first_uri;
    std::unordered_set<std::string> assigned_paths;
    size_t scanned_buffer_count = 0;

    auto add_new_assigned_paths = [&] {
        const auto buffers = source_manager->getAllBuffers();
        // SourceManager buffer IDs are append-only for this compilation.  Do
        // not clear and rebuild the whole set after each syntax tree: on large
        // filelists that turns a simple duplicate check into O(n²) allocator
        // churn.  Scan only buffers that appeared since the previous tree.
        for (size_t i = scanned_buffer_count; i < buffers.size(); ++i) {
            auto buffer = buffers[i];
            const auto& path = source_manager->getFullPath(buffer);
            if (!path.empty())
                assigned_paths.insert(normalize_filesystem_path(path).string());
        }
        scanned_buffer_count = buffers.size();
    };

    for (const auto index : group.files) {
        const auto& file = file_at(index);
        const auto normalized_path = normalize_filesystem_path(file.path).string();
        if (assigned_paths.contains(normalized_path))
            continue;

        std::string text = file.text ? *file.text : read_file_text_or_empty(file.path);
        if (text.empty() && !file.text)
            continue;

        if (first_uri.empty())
            first_uri = file.uri;

        try {
            const auto library_it = library_by_path.find(normalized_path);
            auto tree = slang::syntax::SyntaxTree::fromText(
                std::string_view(text), *source_manager, std::string_view(file.uri),
                std::string_view(file.path), bag,
                library_it == library_by_path.end() ? nullptr : library_it->second);
            compilation.addSyntaxTree(std::move(tree));
            add_new_assigned_paths();
        } catch (const std::exception& e) {
            const bool log_timing = log_timing_.load(std::memory_order_relaxed);
            if (log_timing) {
                std::cerr << "[lazyverilog] semantic compile skipped " << file.path << ": "
                          << e.what() << "\n";
            }
            add_new_assigned_paths();
        }
    }

    if (!first_uri.empty()) {
        slang::DiagnosticEngine engine(*source_manager);
        try {
            for (const auto& diagnostic : compilation.getSemanticDiagnostics()) {
                std::string uri;
                auto info = convert_diagnostic(*source_manager, engine, diagnostic, first_uri, uri);
                result.diagnostics_by_uri[uri].push_back(std::move(info));
            }
        } catch (const std::exception& e) {
            const bool log_timing = log_timing_.load(std::memory_order_relaxed);
            if (log_timing)
                std::cerr << "[lazyverilog] semantic diagnostics failed: " << e.what() << "\n";
        }
    }

    const bool group_log_timing = log_timing_.load(std::memory_order_relaxed);
    if (group_log_timing) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        std::cerr << "[lazyverilog][compilation] project "
                  << (group.root.empty() ? std::string("(none)") : group.root)
                  << " files=" << group.files.size() << ": " << elapsed.count() << "ms\n";
    }
}

BackgroundCompileResult BackgroundCompiler::compile(uint64_t generation,
                                                    CompilationSnapshot snapshot) const {
    const auto start = Clock::now();
    BackgroundCompileResult result;
    result.generation = generation;
    result.open_uris = std::move(snapshot.open_uris);

    // One slang Compilation per project, run one at a time.
    //
    // A Compilation has a single preprocessor and a single flat module
    // namespace, so compiling every open project's files together gave one
    // elaboration two projects' `fifo` and handed one project's `define` to the
    // other project's parse.  What decides which declaration an instantiation
    // binds to is the set of files being compiled, and that set is what a `.f`
    // names -- so the grouping is the language's own scope, not a heuristic.
    //
    // Sequentially, deliberately: peak memory is the binding resource here, and
    // N projects compiled at once would multiply it.  Compiled one after the
    // other, N projects cost N times the wall clock and one project's memory.
    //
    // No branch for the "no registered project" session: compilation_snapshot()
    // emits that as a group with an empty root, so `groups` is the only answer
    // to what gets compiled.
    size_t compiled_file_count = 0;
    for (const auto& group : snapshot.groups) {
        compiled_file_count += group.files.size();
        compile_group(snapshot, group, result);
    }

    // A file two projects' filelists both name is compiled once per project, so
    // an identical diagnostic can arrive twice.  Two *different* diagnostics for
    // one file are kept: shared IP really does mean different things under two
    // projects' defines, and that is worth seeing.
    //
    // One group cannot produce a duplicate -- its own parse loop skips a path it
    // has already assigned -- so the single-project session and the fallback both
    // skip a pass that rebuilds every vector and hashes every diagnostic.
    if (snapshot.groups.size() > 1) {
        for (auto& [uri, diags] : result.diagnostics_by_uri)
            dedup_parse_diagnostics(diags);
    }

    result.uri_versions = std::move(snapshot.uri_versions);

    const bool log_timing = log_timing_.load(std::memory_order_relaxed);
    if (log_timing) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        std::cerr << "[lazyverilog][compilation] semantic compilation files="
                  << compiled_file_count << ": " << elapsed.count() << "ms\n";
    }

    return result;
}
