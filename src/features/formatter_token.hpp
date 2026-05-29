#pragma once

#include "../config.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <slang/parsing/Token.h>
#include <slang/text/SourceLocation.h>

namespace svfmt {

static constexpr size_t npos = static_cast<size_t>(-1);

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
struct LexemeFacts {
    slang::parsing::TokenKind kind{slang::parsing::TokenKind::Unknown};

    std::string text;
    std::string lower_text;

    slang::SourceRange range;

    bool is_comment{false};
    bool is_directive{false};
    bool is_whitespace_sensitive{false};
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
    bool begins_line_construct{false};
    bool ends_line_construct{false};

    bool opens_indent_scope{false};
    bool closes_indent_scope{false};

    bool starts_argument_list{false};
    bool ends_argument_list{false};

    bool starts_parameter_list{false};
    bool starts_port_list{false};
};

// 4. CommentFacts: frozen comment classification.  Comments are classified once
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

// 5. InputTriviaFacts: observation of original whitespace.  These are facts
// about the input, useful as heuristics.  Correctness should not depend on them
// as rendered columns or indentation policy.
struct InputTriviaFacts {
    int original_spaces_before{0};
    int original_newlines_before{0};
    int original_indent{0};
    bool starts_original_line{false};
    int original_column{0};
};

// -----------------------------------------------------------------------------
// Mutable formatting data
// -----------------------------------------------------------------------------
// Every family below has exactly one writer pass.  These fields are formatter
// intent; only the renderer turns them into whitespace.
struct WrapMetadata { bool can_break_before{false}; bool must_break_before{false}; bool can_break_after{false}; bool must_break_after{false}; bool continuation{false}; int wrap_group{-1}; };
struct IndentMetadata { int base_indent{0}; int continuation_indent{0}; size_t anchor_token{npos}; };
struct AlignMetadata { bool enabled{false}; int target_column{-1}; int alignment_group{-1}; };
struct SpaceMetadata { int spaces_before{1}; bool suppress_space{false}; };
struct CommentMetadata { bool preserve_internal_indent{true}; bool force_own_line{false}; int relative_indent{0}; };
struct BlankLineMetadata { int before{0}; int after{0}; };
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
    CommentFacts comment;
    InputTriviaFacts input_trivia;
};

struct Tok {
    std::shared_ptr<const LexemeFacts> lex;

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
