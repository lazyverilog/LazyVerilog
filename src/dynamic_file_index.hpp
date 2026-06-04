#pragma once

#include "document_state.hpp"
#include "syntax_index.hpp"

/// Build the clangd-style dynamic/opened-file index shard for one non-background
/// document snapshot.
///
/// This is used for files that are open in the editor but are *not* being
/// answered purely from the current request's cursor-local SyntaxTree query:
///
///   - another open buffer that participates in completion/navigation,
///   - a filelist file whose unsaved open-buffer contents should replace its
///     stale disk-backed background shard,
///   - explicit command features such as AutoInst/AutoWire/Connect/RTL tree
///     that intentionally need a whole-file structural view.
///
/// It deliberately lives outside DocumentState::index.  didOpen/didChange keep
/// only the parsed SyntaxTree for the current file; hot request paths query that
/// tree directly.  This function is the bridge for colder, project-oriented
/// features that still consume SyntaxIndex-shaped shards.
SyntaxIndex build_dynamic_file_index(const DocumentState& state);

/// Build a current-file structural view by walking the live SyntaxTree.
///
/// This is for broad command-style features that genuinely need the current
/// file's module/interface/package declarations and hierarchy instances
/// (AutoWire, Connect, InlayHints, RTL tree, stale-autoinst lint).  Unlike the
/// dynamic/opened-file shard, it intentionally skips imports/macros because
/// those point-query/completion concerns should query the current SyntaxTree
/// directly.
SyntaxIndex build_current_ast_structural_index(const DocumentState& state);
