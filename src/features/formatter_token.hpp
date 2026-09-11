#pragma once

#include "../config.hpp"
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <slang/parsing/Token.h>
#include <slang/syntax/SyntaxKind.h>
#include <slang/text/SourceLocation.h>

namespace svfmt {

static constexpr size_t npos = static_cast<size_t>(-1);

inline std::string lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

// -----------------------------------------------------------------------------
// Immutable fact layers
// -----------------------------------------------------------------------------
// Rule: a field belongs here only if it is a fact, not a formatter decision.
// These facts are derived from lexing/parsing/source observation, do not depend
// on formatter options, and are stable across pass ordering.  Formatting passes
// may read them freely, but only the early collection/analysis stage populates
// them.

// 1. LexemeFacts: pure lexer output / lexical identity.  This replaces the old
// previous flat lexeme holder because these fields are immutable facts about
// the lexeme itself.
enum class CommentLexemeKind {
    None,
    Line,
    Block,
};

struct LexemeFacts {
    slang::parsing::TokenKind kind{slang::parsing::TokenKind::Unknown};

    std::string text;
    std::string lower_text;
    slang::syntax::SyntaxKind directive_kind{slang::syntax::SyntaxKind::Unknown};

    slang::SourceRange range;

    bool is_directive{false};
    bool is_whitespace_sensitive{false};

    // An escaped identifier (`\\data[0] `) is terminated by whitespace, not by
    // the end of its own spelling, so the separator that follows it belongs to
    // the name.  Spacing rules that would otherwise close the gap must leave at
    // least one separator here, or re-lexing glues the next token onto the
    // identifier and the safety net aborts the whole file.
    bool is_escaped_identifier{false};

    // Part of an attribute instance `(* ... *)`, delimiters included.  slang's
    // lexer has no `(*` token -- the parser reconstructs attributes from an
    // OpenParenthesis immediately followed by a Star -- so the adjacency is
    // recorded here, where byte positions are known, rather than re-derived by
    // a pass from input trivia.
    bool in_attribute_instance{false};

    // Comment spelling is a lexical fact.  Formatting passes should not peek at
    // token text to distinguish `//` from `/* ... */`; doing so couples policy
    // to source spelling and has caused non-idempotent comment handling bugs.
    CommentLexemeKind comment_kind{CommentLexemeKind::None};

    // Format on/off markers are also lexical facts collected at the same place
    // that recognizes the configured marker comments.  Downstream layers should
    // not re-run regex/string matching over immutable token text.
    bool is_format_off_marker{false};
    bool is_format_on_marker{false};

    // Raw body between a format-off marker and the matching format-on marker.
    // This is stronger than ordinary whitespace-sensitive macro text: the
    // formatter must not interpret comments, directives, macro calls, or blank
    // lines inside this region at all.  The renderer emits this token verbatim.
    bool is_disabled_region_body{false};
};

// 2. SyntaxFacts: parser-ish truth.  These are not formatting policy; matching
// delimiters, statement ranges, and syntactic containment are true independent of
// where the formatter later chooses to put spaces or newlines.
struct SyntaxFacts {
    size_t matching_token{npos};
    size_t stmt_begin{npos};
    size_t stmt_end{npos};
    size_t parent_construct{npos};

    int paren_depth{0};
    int bracket_depth{0};
    int brace_depth{0};

    bool in_function_decl{false};
    bool in_task_decl{false};
    bool in_class_decl{false};
    bool in_covergroup{false};
    bool in_modport{false};
};

// 3. TopologyFacts: stable graph-ish structural labels that make later passes
// simpler.  Example: an opening parenthesis can be known to start an argument
// list without deciding whether that list is rendered on one line or many lines.
struct TopologyFacts {
    bool opens_indent_scope{false};
    bool closes_indent_scope{false};

    // `{` is overloaded in SystemVerilog: it opens a constraint or coverage
    // body, but it also opens a concatenation, a set-membership list, an
    // assignment pattern and a streaming expression.  Only a statement block
    // holds a `;` at its own depth, so that is what separates the two -- a
    // TokenKind fact, not a lookbehind on which keyword happens to precede it,
    // which would have to enumerate `constraint`, `coverpoint`, `cross`, `dist`
    // and every `foreach`/`if` nested inside a constraint body.  Set on the
    // opening brace only.
    bool opens_brace_block{false};

    bool starts_argument_list{false};
    bool ends_argument_list{false};

    bool starts_parameter_list{false};
    bool starts_port_list{false};

    // True for tokens strictly between a starts_argument_list OpenParen and its
    // matching CloseParenthesis (exclusive on both ends).  Precomputed by
    // SyntaxPass to replace O(n) backward scans.
    bool inside_argument_list{false};
};

// 4. InputTriviaFacts: observation of original whitespace.  These are facts
// about the input, useful as heuristics.  Correctness should not depend on them
// as rendered columns or indentation policy.
struct InputTriviaFacts {
    int original_spaces_before{0};
    int original_newlines_before{0};
    int original_indent{0};
    bool starts_original_line{false};
    int original_column{0};
};


// 5. CommentFacts: frozen comment classification.  Comments are classified once
// before formatting decisions.  That prevents comment layout from oscillating
// when a later pass moves surrounding code.
enum class CommentRole {
    None,
    Leading,
    Trailing,
    OwnLine,
    InterstitialLeading,
    InterstitialTrailing,
    Detached,
    PreprocessorAdjacent,
};

struct CommentFacts {
    CommentRole role{CommentRole::None};
    size_t anchor_token{npos};
    bool inside_expression{false};
    bool inside_arg_list{false};
};

// -----------------------------------------------------------------------------
// Mutable formatting data
// -----------------------------------------------------------------------------
// Every family below has exactly one writer pass.  These fields are formatter
// intent; only the renderer turns them into whitespace.
enum class WrapListKind {
    None,
    FunctionBlock,
    FunctionHanging,
    ModuleParametersBlock,
    ModuleParametersHanging,
    ModulePorts,
    InstancePorts,
    FunctionDeclBlock,
    FunctionDeclHanging,
    EnumBody,
    BraceBlock,
    ModportBody,
};

struct WrapMetadata {
    bool can_break_before{false};
    bool must_break_before{false};
    bool can_break_after{false};
    bool must_break_after{false};
    bool continuation{false};
    int wrap_group{-1};

    // Written only by WrapPass; read by Indent/Align/Spacing to assign the
    // concrete layout for already-classified multiline delimiter groups.
    WrapListKind list_kind{WrapListKind::None};
    size_t list_open{npos};
};
struct IndentMetadata { int base_indent{0}; int continuation_indent{0}; size_t anchor_token{npos}; };
struct AlignMetadata { bool enabled{false}; int target_column{-1}; int alignment_group{-1}; };
struct SpaceMetadata { int spaces_before{1}; bool suppress_space{false}; };
struct CommentMetadata { bool preserve_internal_indent{true}; bool force_own_line{false}; int relative_indent{0}; };
struct BlankLineMetadata { int before{0}; };
struct MacroMetadata { bool passthrough{false}; bool suppress_alignment{false}; bool suppress_wrapping{false}; bool opens_indent_scope{false}; bool closes_indent_scope{false}; bool force_line_break{false}; };

struct MutableData {
    WrapMetadata wrap;
    IndentMetadata indent;
    AlignMetadata align;
    SpaceMetadata space;
    CommentMetadata comment;
    BlankLineMetadata blank;
    MacroMetadata macro;
};

// One immutable aggregate keeps the formatter's fact model explicit: these
// fields describe what the input is, not what the formatter decided to do.
struct ImmutableData {
    SyntaxFacts syntax;
    TopologyFacts topology;
    InputTriviaFacts input_trivia;
    CommentFacts comment;
};

struct Tok {
    // Lexeme facts have exactly one owner: the token itself.  They used to live
    // behind a shared_ptr, which made every collected token pay for a heap
    // allocation, a control block, and atomic refcount operations.  The facts
    // are immutable by convention after collection, but they are not shared
    // across tokens or snapshots, so embedding them keeps ownership explicit and
    // removes the allocator hot spot from large repeated formatting runs.
    LexemeFacts lex;

    ImmutableData immutable;
    MutableData mutable_;
};

using TokenStream = std::vector<Tok>;

class IFormatPass {
public:
    virtual ~IFormatPass() = default;
    virtual const char* name() const = 0;
    virtual void run(TokenStream& tokens) = 0;
};

} // namespace svfmt
