#pragma once

#include "formatter_token.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace svfmt {

using TK = slang::parsing::TokenKind;

inline bool text_is(const Tok& t, const char* s) { return t.lex->text == s; }
inline bool kind_is(const Tok& t, TK k) { return t.lex->kind == k; }
inline bool is_passthrough(const Tok& t) { return t.mutable_.macro.passthrough || t.lex->is_whitespace_sensitive; }
inline int token_width(const Tok& t) { return static_cast<int>(t.lex->text.size()); }

inline bool is_control_keyword(TK k) {
    return k == TK::IfKeyword || k == TK::ForKeyword || k == TK::ForeachKeyword ||
           k == TK::WhileKeyword || k == TK::CaseKeyword || k == TK::CaseXKeyword ||
           k == TK::CaseZKeyword || k == TK::RepeatKeyword;
}
inline bool is_open_block(TK k) {
    return k == TK::BeginKeyword || k == TK::ClassKeyword || k == TK::FunctionKeyword ||
           k == TK::TaskKeyword || k == TK::CaseKeyword || k == TK::CaseXKeyword ||
           k == TK::CaseZKeyword || k == TK::OpenBrace ||
           k == TK::GenerateKeyword || k == TK::CoverGroupKeyword ||
           k == TK::PropertyKeyword || k == TK::SequenceKeyword || k == TK::CheckerKeyword ||
           k == TK::ClockingKeyword || k == TK::ConfigKeyword || k == TK::PrimitiveKeyword ||
           k == TK::SpecifyKeyword;
}
inline bool is_outer_open(TK k) {
    return k == TK::ModuleKeyword || k == TK::InterfaceKeyword || k == TK::PackageKeyword ||
           k == TK::MacromoduleKeyword || k == TK::ProgramKeyword;
}
inline bool is_close_block(TK k) {
    return k == TK::EndKeyword || k == TK::EndClassKeyword || k == TK::EndFunctionKeyword ||
           k == TK::EndTaskKeyword || k == TK::EndCaseKeyword || k == TK::CloseBrace ||
           k == TK::EndGenerateKeyword || k == TK::EndGroupKeyword || k == TK::EndPropertyKeyword ||
           k == TK::EndSequenceKeyword || k == TK::EndCheckerKeyword || k == TK::EndClockingKeyword ||
           k == TK::EndConfigKeyword || k == TK::EndPrimitiveKeyword || k == TK::EndSpecifyKeyword ||
           k == TK::EndTableKeyword;
}
inline bool is_outer_close(TK k) {
    return k == TK::EndModuleKeyword || k == TK::EndInterfaceKeyword || k == TK::EndPackageKeyword ||
           k == TK::EndProgramKeyword;
}
inline bool is_assignment_op(TK k) {
    return k == TK::Equals || k == TK::LessThanEquals ||
           k == TK::PlusEqual || k == TK::MinusEqual ||
           k == TK::StarEqual || k == TK::SlashEqual || k == TK::PercentEqual ||
           k == TK::AndEqual || k == TK::OrEqual || k == TK::XorEqual ||
           k == TK::LeftShiftEqual || k == TK::RightShiftEqual ||
           k == TK::TripleLeftShiftEqual || k == TK::TripleRightShiftEqual;
}
inline bool is_binary_op(TK k) {
    return k == TK::Plus || k == TK::Minus || k == TK::Star || k == TK::Slash || k == TK::Percent ||
           k == TK::DoubleEquals || k == TK::ExclamationEquals || k == TK::LessThan || k == TK::GreaterThan ||
           k == TK::LessThanEquals || k == TK::GreaterThanEquals || k == TK::DoubleAnd || k == TK::DoubleOr ||
           k == TK::And || k == TK::Or || k == TK::Xor || k == TK::LeftShift || k == TK::RightShift ||
           k == TK::TripleLeftShift || k == TK::TripleRightShift ||
           k == TK::TildeAnd || k == TK::TildeOr || k == TK::TildeXor || k == TK::XorTilde ||
           k == TK::InsideKeyword;
}
inline bool no_space_before(TK k) {
    return k == TK::CloseParenthesis || k == TK::CloseBracket || k == TK::CloseBrace ||
           k == TK::Semicolon || k == TK::Comma || k == TK::Dot || k == TK::DoubleColon ||
           k == TK::PlusColon || k == TK::MinusColon;
}
inline bool no_space_after(TK k) {
    return k == TK::OpenParenthesis || k == TK::OpenBracket || k == TK::OpenBrace ||
           k == TK::ApostropheOpenBrace || k == TK::IntegerBase ||
           k == TK::Dot || k == TK::DoubleColon || k == TK::Hash || k == TK::Apostrophe;
}
inline bool wants_before(const std::string& mode) { return mode == "before" || mode == "both"; }
inline bool wants_after(const std::string& mode) { return mode == "after" || mode == "both"; }

inline bool is_single_stmt_control(TK k) {
    return k == TK::IfKeyword || k == TK::ForKeyword || k == TK::ForeachKeyword ||
           k == TK::WhileKeyword || k == TK::RepeatKeyword;
}
inline bool is_unary_op(TK k) {
    return k == TK::Tilde || k == TK::Exclamation ||
           k == TK::TildeAnd || k == TK::TildeOr || k == TK::TildeXor || k == TK::XorTilde ||
           k == TK::DoublePlus || k == TK::DoubleMinus;
}
inline bool is_type_keyword(TK k) {
    return k == TK::LogicKeyword || k == TK::WireKeyword || k == TK::RegKeyword ||
           k == TK::BitKeyword || k == TK::ByteKeyword || k == TK::ShortIntKeyword ||
           k == TK::IntKeyword || k == TK::LongIntKeyword || k == TK::IntegerKeyword ||
           k == TK::RealKeyword || k == TK::RealTimeKeyword || k == TK::ShortRealKeyword ||
           k == TK::TimeKeyword || k == TK::StringKeyword || k == TK::CHandleKeyword ||
           k == TK::EventKeyword || k == TK::VoidKeyword ||
           k == TK::SignedKeyword || k == TK::UnsignedKeyword || k == TK::PackedKeyword;
}
inline bool is_port_direction(TK k) {
    return k == TK::InputKeyword || k == TK::OutputKeyword ||
           k == TK::InOutKeyword || k == TK::RefKeyword;
}
inline bool is_numeric(const Tok& t) {
    return t.lex->kind == TK::IntegerLiteral || t.lex->kind == TK::IntegerBase ||
           t.lex->kind == TK::UnbasedUnsizedLiteral || t.lex->kind == TK::RealLiteral ||
           t.lex->kind == TK::TimeLiteral;
}
inline bool is_identifier_like(const Tok& t) {
    return t.lex->kind == TK::Identifier || t.lex->kind == TK::SystemIdentifier ||
           t.lex->kind == TK::MacroUsage;
}

inline bool is_code_token(const Tok& t) {
    return !t.lex->is_comment && !t.lex->is_directive && !is_passthrough(t);
}

inline size_t prev_code(const TokenStream& tokens, size_t before) {
    before = std::min(before, tokens.size());
    for (size_t n = before; n > 0; --n) {
        size_t i = n - 1;
        if (is_code_token(tokens[i]))
            return i;
    }
    return npos;
}

inline size_t next_code(const TokenStream& tokens, size_t first, size_t end) {
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i)
        if (is_code_token(tokens[i]))
            return i;
    return npos;
}

inline bool is_declaration_keyword(TK k) {
    return is_port_direction(k) || is_type_keyword(k) || k == TK::ParameterKeyword ||
           k == TK::LocalParamKeyword || k == TK::VarKeyword || k == TK::ConstKeyword;
}

inline bool starts_module_like_header(TK k) {
    return k == TK::ModuleKeyword || k == TK::InterfaceKeyword ||
           k == TK::MacromoduleKeyword || k == TK::ProgramKeyword;
}

inline int snap_to_grid(int value, int indent_size) {
    if (indent_size <= 0) return value;
    return ((value + indent_size - 1) / indent_size) * indent_size;
}

inline int option_width(int value, const FormatOptions& opts) {
    return opts.tab_align ? snap_to_grid(value, opts.indent_size) : value;
}

inline int compact_width(const TokenStream& tokens, size_t first, size_t end) {
    int w = 0;
    bool need_space = false;
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i) {
        const Tok& t = tokens[i];
        if (!is_code_token(t)) continue;
        if (need_space && !no_space_before(t.lex->kind))
            ++w;
        w += token_width(t);
        need_space = !no_space_after(t.lex->kind);
    }
    return w;
}

inline int canonical_space_between(const Tok& left, const Tok& right) {
    if (no_space_before(right.lex->kind) || no_space_after(left.lex->kind))
        return 0;
    if (kind_is(left, TK::Dot) || kind_is(right, TK::Dot) ||
        kind_is(left, TK::DoubleColon) || kind_is(right, TK::DoubleColon))
        return 0;
    if (kind_is(right, TK::OpenBracket) &&
        (is_identifier_like(left) || kind_is(left, TK::CloseBracket) ||
         kind_is(left, TK::CloseParenthesis)))
        return 0;
    if (kind_is(right, TK::OpenParenthesis) &&
        (is_identifier_like(left) || is_control_keyword(left.lex->kind) ||
         kind_is(left, TK::CloseParenthesis) || kind_is(left, TK::Hash)))
        return 0;
    return 1;
}

inline int canonical_width(const TokenStream& tokens, size_t first, size_t end) {
    int w = 0;
    size_t prev = npos;
    end = std::min(end, tokens.size());
    for (size_t i = first; i < end; ++i) {
        const Tok& t = tokens[i];
        if (!is_code_token(t)) continue;
        if (prev != npos)
            w += canonical_space_between(tokens[prev], t);
        w += token_width(t);
        prev = i;
    }
    return w;
}

inline int line_prefix_width(const TokenStream& tokens, size_t token_idx) {
    size_t line_start = 0;
    for (size_t i = token_idx; i > 0; --i) {
        if (tokens[i].mutable_.wrap.must_break_before || tokens[i - 1].mutable_.wrap.must_break_after) {
            line_start = i;
            break;
        }
    }
    return compact_width(tokens, line_start, token_idx);
}

struct ListItem {
    size_t first{npos};
    size_t last{npos};   // inclusive
    size_t comma{npos};
};

inline std::vector<ListItem> top_level_list_items(const TokenStream& tokens, size_t first, size_t close) {
    std::vector<ListItem> out;
    size_t start = next_code(tokens, first, close);
    size_t last = npos;
    int pd = 0, bd = 0, brd = 0;
    for (size_t i = first; i < close && i < tokens.size(); ++i) {
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::OpenParenthesis)) ++pd;
        else if (kind_is(tokens[i], TK::CloseParenthesis) && pd > 0) --pd;
        else if (kind_is(tokens[i], TK::OpenBracket)) ++bd;
        else if (kind_is(tokens[i], TK::CloseBracket) && bd > 0) --bd;
        else if (kind_is(tokens[i], TK::OpenBrace) || kind_is(tokens[i], TK::ApostropheOpenBrace)) ++brd;
        else if (kind_is(tokens[i], TK::CloseBrace) && brd > 0) --brd;

        if (kind_is(tokens[i], TK::Comma) && pd == 0 && bd == 0 && brd == 0) {
            if (start != npos && last != npos)
                out.push_back({start, last, i});
            start = next_code(tokens, i + 1, close);
            last = npos;
        } else {
            last = i;
        }
    }
    if (start != npos && last != npos)
        out.push_back({start, last, npos});
    return out;
}

inline size_t find_header_keyword_before(const TokenStream& tokens, size_t open) {
    int pd = 0;
    size_t direct_prev = prev_code(tokens, open);
    bool open_after_parameter_list = false;
    if (direct_prev != npos && kind_is(tokens[direct_prev], TK::CloseParenthesis)) {
        size_t param_open = tokens[direct_prev].immutable.syntax.matching_token;
        open_after_parameter_list = param_open != npos &&
                                    tokens[param_open].immutable.topology.starts_parameter_list;
    }
    bool open_follows_header_import =
        (direct_prev != npos && kind_is(tokens[direct_prev], TK::Semicolon)) ||
        open_after_parameter_list;
    for (size_t n = open; n > 0; --n) {
        size_t i = n - 1;
        if (!is_code_token(tokens[i])) continue;
        if (kind_is(tokens[i], TK::CloseParenthesis)) ++pd;
        else if (kind_is(tokens[i], TK::OpenParenthesis) && pd > 0) --pd;
        if (pd == 0 && starts_module_like_header(tokens[i].lex->kind))
            return i;
        if (pd == 0 && kind_is(tokens[i], TK::Semicolon) && !open_follows_header_import)
            break;
        if (pd == 0 && (is_outer_close(tokens[i].lex->kind) || is_close_block(tokens[i].lex->kind)))
            break;
    }
    return npos;
}

inline bool is_instance_port_open(const TokenStream& tokens, size_t open) {
    size_t inst = prev_code(tokens, open);
    if (inst == npos || !is_identifier_like(tokens[inst])) return false;
    size_t mod = prev_code(tokens, inst);
    if (mod == npos) return false;
    if (kind_is(tokens[mod], TK::CloseBracket)) {
        size_t br = tokens[mod].immutable.syntax.matching_token;
        if (br != npos) mod = prev_code(tokens, br);
    }
    if (mod == npos || !is_identifier_like(tokens[mod])) return false;
    size_t prev = prev_code(tokens, mod);
    return prev == npos || kind_is(tokens[prev], TK::Semicolon) ||
           kind_is(tokens[prev], TK::BeginKeyword) || kind_is(tokens[prev], TK::EndKeyword);
}

// SyntaxPass is the early fact-freeze pass.  It writes SyntaxFacts,
// TopologyFacts, and CommentFacts from immutable lexemes/input trivia.  These
// are parser-ish facts, not formatter decisions; downstream formatting-policy
// passes must not mutate them.
class SyntaxPass final : public IFormatPass {
public:
    const char* name() const override { return "syntax"; }
    void run(TokenStream& tokens) override {
        std::vector<size_t> parens, brackets, braces;
        int pd = 0, bd = 0, brd = 0;
        bool in_function_decl = false;
        bool in_task_decl = false;
        bool in_class_decl = false;
        bool in_modport = false;
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            t.immutable.syntax.paren_depth = pd;
            t.immutable.syntax.bracket_depth = bd;
            t.immutable.syntax.brace_depth = brd;
            t.immutable.syntax.in_function_decl = in_function_decl;
            t.immutable.syntax.in_task_decl = in_task_decl;
            t.immutable.syntax.in_class_decl = in_class_decl;
            t.immutable.syntax.in_modport = in_modport;
            // Freeze input-line topology before any wrap decision exists.
            t.immutable.topology.begins_line_construct = t.immutable.input_trivia.starts_original_line;
            t.immutable.topology.ends_line_construct = kind_is(t, TK::Semicolon) || kind_is(t, TK::Comma) ||
                                             (t.lex->is_comment && t.lex->text.rfind("//", 0) == 0);
            t.immutable.topology.opens_indent_scope = is_open_block(t.lex->kind) || is_outer_open(t.lex->kind);
            t.immutable.topology.closes_indent_scope = is_close_block(t.lex->kind) || is_outer_close(t.lex->kind);

            if (kind_is(t, TK::OpenParenthesis)) {
                size_t prev_i = prev_code(tokens, i);
                const Tok* prev = prev_i == npos ? nullptr : &tokens[prev_i];
                t.immutable.topology.starts_parameter_list = prev && kind_is(*prev, TK::Hash);
                t.immutable.topology.starts_argument_list = prev &&
                    (kind_is(*prev, TK::Identifier) || kind_is(*prev, TK::SystemIdentifier) ||
                     kind_is(*prev, TK::MacroUsage) || kind_is(*prev, TK::CloseParenthesis));
                t.immutable.topology.starts_port_list =
                    pd == 0 &&
                    find_header_keyword_before(tokens, i) != npos &&
                    !(prev && kind_is(*prev, TK::Hash));
            }
            // ends_argument_list is set later when matching token is known (see below)

            if (t.lex->is_comment) {
                bool comma_interstitial_block = false;
                if (t.immutable.input_trivia.starts_original_line &&
                    t.lex->text.rfind("/*", 0) == 0) {
                    size_t p = prev_code(tokens, i);
                    size_t nx = next_code(tokens, i + 1, tokens.size());
                    comma_interstitial_block =
                        p != npos && kind_is(tokens[p], TK::Comma) &&
                        nx != npos &&
                        tokens[nx].immutable.input_trivia.original_newlines_before == 0;
                }
                t.immutable.comment.role =
                    (t.immutable.input_trivia.starts_original_line && !comma_interstitial_block)
                    ? CommentRole::OwnLine : CommentRole::Trailing;
                t.immutable.comment.anchor_token = i == 0 ? npos : i - 1;
                t.immutable.comment.inside_expression = pd > 0 || bd > 0 || brd > 0;
                t.immutable.comment.inside_arg_list = pd > 0;
            }
            if (t.lex->is_directive) {
                (void)0; // pp-conditional tracking reserved for future use
            }
            if (kind_is(t, TK::FunctionKeyword)) in_function_decl = true;
            if (kind_is(t, TK::TaskKeyword)) in_task_decl = true;
            if (kind_is(t, TK::ClassKeyword)) in_class_decl = true;
            if (kind_is(t, TK::ModPortKeyword)) in_modport = true;
            if (kind_is(t, TK::OpenParenthesis)) { parens.push_back(i); ++pd; }
            else if (kind_is(t, TK::CloseParenthesis)) { if (!parens.empty()) { auto j = parens.back(); parens.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; t.immutable.topology.ends_argument_list = tokens[j].immutable.topology.starts_argument_list; } pd = std::max(0, pd - 1); }
            else if (kind_is(t, TK::OpenBracket)) { brackets.push_back(i); ++bd; }
            else if (kind_is(t, TK::CloseBracket)) { if (!brackets.empty()) { auto j = brackets.back(); brackets.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } bd = std::max(0, bd - 1); }
            else if (kind_is(t, TK::OpenBrace)) { braces.push_back(i); ++brd; }
            else if (kind_is(t, TK::CloseBrace)) { if (!braces.empty()) { auto j = braces.back(); braces.pop_back(); tokens[j].immutable.syntax.matching_token = i; t.immutable.syntax.matching_token = j; } brd = std::max(0, brd - 1); }
            if (kind_is(t, TK::Semicolon)) {
                in_function_decl = false;
                in_task_decl = false;
                in_modport = false;
            }
            if (kind_is(t, TK::EndClassKeyword))
                in_class_decl = false;
        }
        size_t stmt_start = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (kind_is(tokens[i], TK::Semicolon) || kind_is(tokens[i], TK::Comma)) {
                for (size_t j = stmt_start; j <= i && j < tokens.size(); ++j) { tokens[j].immutable.syntax.stmt_begin = stmt_start; tokens[j].immutable.syntax.stmt_end = i; }
                stmt_start = i + 1;
            }
        }
    }
};

// MacroClassifier + MacroRole — used by MacroPass to categorise macro tokens.
enum class MacroRole { ObjectLikeExpr, FunctionLikeExpr, StatementLike, DeclarationLike, ControlFlowLike, BlockBeginLike, BlockEndLike };

struct MacroClassifier {
    std::unordered_set<std::string> statement_like;
    std::unordered_set<std::string> declaration_like;
    std::unordered_set<std::string> control_flow_like;
    std::unordered_set<std::string> block_begin_like;
    std::unordered_set<std::string> block_end_like;
    std::unordered_set<std::string> whitespace_sensitive;

    explicit MacroClassifier(const MacroOptions& m) {
        auto add = [](std::unordered_set<std::string>& dst, const std::vector<std::string>& src) {
            for (const auto& n : src) {
                std::string s = n;
                if (!s.empty() && s[0] == '`') s = s.substr(1);
                dst.insert(s);
            }
        };
        add(statement_like,    m.statement_like);
        add(declaration_like,  m.declaration_like);
        add(control_flow_like, m.control_flow_like);
        add(block_begin_like,  m.block_begin_like);
        add(block_end_like,    m.block_end_like);
        add(whitespace_sensitive, m.whitespace_sensitive);
    }

    static std::string extract_name(const std::string& raw_text) {
        std::string name = raw_text;
        if (!name.empty() && name[0] == '`') name = name.substr(1);
        size_t end = 0;
        while (end < name.size() && (std::isalnum((unsigned char)name[end]) || name[end] == '_' || name[end] == '$'))
            ++end;
        return name.substr(0, end);
    }

    MacroRole classify(const std::string& raw_text) const {
        std::string name = extract_name(raw_text);
        if (block_end_like.count(name))    return MacroRole::BlockEndLike;
        if (block_begin_like.count(name))  return MacroRole::BlockBeginLike;
        if (control_flow_like.count(name)) return MacroRole::ControlFlowLike;
        if (declaration_like.count(name))  return MacroRole::DeclarationLike;
        if (statement_like.count(name))    return MacroRole::StatementLike;
        return MacroRole::ObjectLikeExpr;
    }

    bool is_whitespace_sensitive(const std::string& raw_text) const {
        return whitespace_sensitive.count(extract_name(raw_text)) > 0;
    }
};

// MacroPass owns MacroMetadata.  It never changes wrapping or spacing directly;
// it only marks regions/macros that downstream passes must treat conservatively.
class MacroPass final : public IFormatPass {
public:
    explicit MacroPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "macro"; }
    void run(TokenStream& tokens) override {
        MacroClassifier mc(opts_.macros);
        for (auto& t : tokens) {
            if (t.lex->is_whitespace_sensitive) {
                t.mutable_.macro.passthrough = true;
                t.mutable_.macro.suppress_alignment = true;
                t.mutable_.macro.suppress_wrapping = true;
            } else if (t.lex->is_directive) {
                t.mutable_.macro.suppress_alignment = true;
            } else if (t.lex->kind == TK::MacroUsage) {
                if (mc.is_whitespace_sensitive(t.lex->text)) {
                    t.mutable_.macro.passthrough = true;
                    t.mutable_.macro.suppress_alignment = true;
                    t.mutable_.macro.suppress_wrapping = true;
                } else {
                    t.mutable_.macro.suppress_alignment = true;
                    MacroRole role = mc.classify(t.lex->text);
                    if (role == MacroRole::StatementLike || role == MacroRole::DeclarationLike ||
                        role == MacroRole::ControlFlowLike || role == MacroRole::BlockBeginLike ||
                        role == MacroRole::BlockEndLike) {
                        t.mutable_.macro.suppress_wrapping = false;
                    }
                    if (role == MacroRole::BlockBeginLike)
                        t.mutable_.macro.opens_indent_scope = true;
                    if (role == MacroRole::BlockEndLike)
                        t.mutable_.macro.closes_indent_scope = true;
                    if (role == MacroRole::StatementLike || role == MacroRole::DeclarationLike ||
                        role == MacroRole::BlockBeginLike || role == MacroRole::BlockEndLike)
                        t.mutable_.macro.force_line_break = true;
                }
            }
        }
    }
private:
    const FormatOptions& opts_;
};

// WrapPass owns WrapMetadata and reads syntax/macro metadata.  It uses stable
// syntax/source facts only; there is no dependency on rendered columns.
class WrapPass final : public IFormatPass {
public:
    explicit WrapPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "wrap"; }
    void run(TokenStream& tokens) override {
        int group = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (t.mutable_.macro.suppress_wrapping) continue;
            t.mutable_.wrap.wrap_group = group;
            if (i == 0)
                t.mutable_.wrap.must_break_before = true;
            if (t.lex->is_comment && t.immutable.comment.role == CommentRole::OwnLine) {
                t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = true;
            }
            // PP directives (ifdef/endif/else/define/…) are always on their own line.
            if (t.lex->is_directive) {
                t.mutable_.wrap.must_break_before = true;
                t.mutable_.wrap.must_break_after = true;
            }
            // Force line break after statement-/declaration-/block-like macro invocations.
            // If the macro is followed by '(args)', break after the matching ')'; otherwise
            // break after the macro token itself.
            if (kind_is(t, TK::MacroUsage) && t.mutable_.macro.force_line_break) {
                size_t break_at = i;
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (tokens[j].lex->is_comment) continue;
                    if (kind_is(tokens[j], TK::OpenParenthesis) &&
                        tokens[j].immutable.syntax.matching_token != npos)
                        break_at = tokens[j].immutable.syntax.matching_token;
                    break;
                }
                tokens[break_at].mutable_.wrap.must_break_after = true;
            }
            if (kind_is(t, TK::MacroUsage) && !t.mutable_.macro.force_line_break) {
                size_t prev = prev_code(tokens, i);
                bool statement_position =
                    prev == npos || kind_is(tokens[prev], TK::Semicolon) ||
                    kind_is(tokens[prev], TK::BeginKeyword) || is_close_block(tokens[prev].lex->kind) ||
                    is_outer_close(tokens[prev].lex->kind);
                size_t open = next_code(tokens, i + 1, tokens.size());
                if (statement_position && open != npos && kind_is(tokens[open], TK::OpenParenthesis)) {
                    size_t close = tokens[open].immutable.syntax.matching_token;
                    if (close != npos && close < tokens.size())
                        tokens[close].mutable_.wrap.must_break_after = true;
                }
            }
            if (i > 0 && tokens[i - 1].lex->is_comment && tokens[i - 1].lex->text.rfind("//", 0) == 0) t.mutable_.wrap.must_break_before = true;
            if (kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth == 0) {
                size_t next = next_code(tokens, i + 1, tokens.size());
                t.mutable_.wrap.must_break_after = !(next != npos && kind_is(tokens[next], TK::Hash));
                ++group;
                // If a trailing comment immediately follows on the same line,
                // defer the line break to after the comment so it renders inline.
                for (size_t j = i + 1; j < tokens.size(); ++j) {
                    if (is_passthrough(tokens[j])) continue;
                    if (tokens[j].lex->is_comment &&
                        tokens[j].immutable.comment.role == CommentRole::Trailing) {
                        t.mutable_.wrap.must_break_after = false;
                        tokens[j].mutable_.wrap.must_break_after = true;
                    }
                    break;
                }
            }
            if (kind_is(t, TK::Comma)) t.mutable_.wrap.can_break_after = true;
            // Close-block keywords always start a new line; CloseBrace only when
            // not inside a parenthesised expression (e.g. `inside {A, B}`).
            if (is_outer_close(t.lex->kind) ||
                (is_close_block(t.lex->kind) &&
                 !(kind_is(t, TK::CloseBrace) && t.immutable.syntax.paren_depth > 0)))
                t.mutable_.wrap.must_break_before = true;
            size_t next_i = next_code(tokens, i + 1, tokens.size());
            bool followed_by_label_colon = next_i != npos && kind_is(tokens[next_i], TK::Colon);
            bool close_brace_before_decl_name =
                kind_is(t, TK::CloseBrace) && next_i != npos &&
                (kind_is(tokens[next_i], TK::Identifier) || kind_is(tokens[next_i], TK::SystemIdentifier));
            bool close_before_inline_else =
                kind_is(t, TK::CloseBrace) && next_i != npos && kind_is(tokens[next_i], TK::ElseKeyword) &&
                !opts_.statement.wrap_end_else_clauses;
            if (kind_is(t, TK::BeginKeyword) ||
                (is_outer_close(t.lex->kind) && !followed_by_label_colon) ||
                (is_close_block(t.lex->kind) && !followed_by_label_colon &&
                 !close_brace_before_decl_name &&
                 !close_before_inline_else &&
                 !(kind_is(t, TK::CloseBrace) && t.immutable.syntax.paren_depth > 0))) {
                t.mutable_.wrap.must_break_after = true;
            }
            if (is_identifier_like(t)) {
                size_t p = prev_code(tokens, i);
                size_t pp = p == npos ? npos : prev_code(tokens, p);
                if (p != npos && pp != npos && kind_is(tokens[p], TK::Colon) &&
                    (is_close_block(tokens[pp].lex->kind) || is_outer_close(tokens[pp].lex->kind)))
                    t.mutable_.wrap.must_break_after = true;
            }
            if (opts_.statement.begin_newline && (kind_is(t, TK::BeginKeyword) || kind_is(t, TK::OpenBrace))) t.mutable_.wrap.must_break_before = true;
            if (opts_.statement.wrap_end_else_clauses && kind_is(t, TK::ElseKeyword) && i > 0 && (kind_is(tokens[i - 1], TK::EndKeyword) || kind_is(tokens[i - 1], TK::CloseBrace))) t.mutable_.wrap.must_break_before = true;
            // else always breaks unless wrap_end_else_clauses handled it above
            if (kind_is(t, TK::ElseKeyword)) {
                bool prev_is_end_or_brace = (i > 0 && (kind_is(tokens[i-1], TK::EndKeyword) || kind_is(tokens[i-1], TK::CloseBrace)));
                if (!prev_is_end_or_brace)
                    t.mutable_.wrap.must_break_before = true;
            }
        }

        auto apply_list = [&](size_t open, WrapListKind kind, bool break_after_open,
                              bool break_before_close, bool break_first_item) {
            if (open >= tokens.size()) return;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos || close >= tokens.size()) return;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty()) return;
            tokens[open].mutable_.wrap.list_kind = kind;
            tokens[open].mutable_.wrap.list_open = open;
            if (break_after_open)
                tokens[open].mutable_.wrap.must_break_after = true;
            else
                tokens[open].mutable_.wrap.must_break_after = false;
            for (size_t n = 0; n < items.size(); ++n) {
                const auto& item = items[n];
                size_t leading_block_comment = npos;
                if (n > 0) {
                    for (size_t c = item.first; c > open + 1; --c) {
                        size_t p = c - 1;
                        if (tokens[p].lex->is_comment &&
                            tokens[p].lex->text.rfind("/*", 0) == 0) {
                            leading_block_comment = p;
                            break;
                        }
                        if (is_code_token(tokens[p]) && !kind_is(tokens[p], TK::Comma))
                            break;
                        if (kind_is(tokens[p], TK::Comma))
                            break;
                    }
                }
                tokens[item.first].mutable_.wrap.list_kind = kind;
                tokens[item.first].mutable_.wrap.list_open = open;
                if (leading_block_comment != npos) {
                    tokens[leading_block_comment].mutable_.wrap.list_kind = kind;
                    tokens[leading_block_comment].mutable_.wrap.list_open = open;
                    tokens[leading_block_comment].mutable_.wrap.must_break_before = true;
                    tokens[item.first].mutable_.wrap.must_break_before = false;
                } else if (break_first_item || n > 0)
                    tokens[item.first].mutable_.wrap.must_break_before = true;
                else {
                    bool preceded_by_own_line_comment = false;
                    for (size_t p = item.first; p > open + 1; --p) {
                        size_t c = p - 1;
                        if (tokens[c].lex->is_comment &&
                            tokens[c].immutable.comment.role == CommentRole::OwnLine) {
                            preceded_by_own_line_comment = true;
                            break;
                        }
                        if (is_code_token(tokens[c]))
                            break;
                    }
                    if (!preceded_by_own_line_comment)
                        tokens[item.first].mutable_.wrap.must_break_before = false;
                }
                if (item.comma != npos) {
                    size_t break_token = item.comma;
                    for (size_t c = item.comma + 1; c < tokens.size(); ++c) {
                        if (tokens[c].lex->is_comment &&
                            tokens[c].immutable.comment.role == CommentRole::Trailing &&
                            tokens[c].lex->text.rfind("//", 0) == 0) {
                            break_token = c;
                            break;
                        }
                        if (is_code_token(tokens[c]))
                            break;
                    }
                    tokens[break_token].mutable_.wrap.must_break_after = true;
                    tokens[item.comma].mutable_.wrap.list_kind = kind;
                    tokens[item.comma].mutable_.wrap.list_open = open;
                }
            }
            tokens[close].mutable_.wrap.list_kind = kind;
            tokens[close].mutable_.wrap.list_open = open;
            if (break_before_close)
                tokens[close].mutable_.wrap.must_break_before = true;
            else
                tokens[close].mutable_.wrap.must_break_before = false;

            size_t after_open = open + 1;
            if (after_open < close && tokens[after_open].lex->is_comment &&
                tokens[after_open].immutable.comment.role == CommentRole::Trailing) {
                tokens[open].mutable_.wrap.must_break_after = false;
                tokens[after_open].mutable_.wrap.must_break_after = true;
            }

            if (kind == WrapListKind::ModulePorts && !items.empty() &&
                !is_declaration_keyword(tokens[items.front().first].lex->kind)) {
                int per_line = 1;
                if (opts_.module.non_ansi_port_per_line_enabled)
                    per_line = std::max(1, opts_.module.non_ansi_port_per_line);
                else if (opts_.module.non_ansi_port_max_line_length_enabled)
                    per_line = static_cast<int>(items.size());
                int line_count = 0;
                int line_width = opts_.indent_size;
                std::vector<bool> keep_with_prev(items.size(), false);
                for (size_t n = 0; n < items.size(); ++n) {
                    int item_width = compact_width(tokens, items[n].first, items[n].last + 1);
                    bool keep = n > 0 && line_count < per_line;
                    if (keep && opts_.module.non_ansi_port_max_line_length_enabled &&
                        !opts_.module.non_ansi_port_per_line_enabled) {
                        int projected = line_width + 2 + item_width +
                                        (items[n].comma != npos ? 1 : 0);
                        keep = projected <= opts_.module.non_ansi_port_max_line_length;
                    }
                    if (keep) {
                        keep_with_prev[n] = true;
                        tokens[items[n].first].mutable_.wrap.must_break_before = false;
                        line_width += 2 + item_width + (items[n].comma != npos ? 1 : 0);
                        ++line_count;
                    } else {
                        line_width = opts_.indent_size + item_width + (items[n].comma != npos ? 1 : 0);
                        line_count = 1;
                    }
                }
                for (size_t n = 0; n + 1 < items.size(); ++n) {
                    if (items[n].comma != npos && keep_with_prev[n + 1]) {
                            tokens[items[n].comma].mutable_.wrap.must_break_after = false;
                            for (size_t c = items[n].comma + 1; c < tokens.size(); ++c) {
                                if (tokens[c].lex->is_comment &&
                                    tokens[c].immutable.comment.role == CommentRole::Trailing)
                                    tokens[c].mutable_.wrap.must_break_after = false;
                                if (is_code_token(tokens[c]))
                                    break;
                            }
                    }
                }
            }
        };

        auto contains_kind = [&](size_t first, size_t end, TK kind) {
            for (size_t j = first; j < end && j < tokens.size(); ++j)
                if (kind_is(tokens[j], kind))
                    return true;
            return false;
        };

        for (size_t open = 0; open < tokens.size(); ++open) {
            if (!kind_is(tokens[open], TK::OpenParenthesis) && !kind_is(tokens[open], TK::OpenBrace))
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos || close >= tokens.size())
                continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty())
                continue;

            if (kind_is(tokens[open], TK::OpenBrace)) {
                size_t prev = prev_code(tokens, open);
                bool enum_body = false;
                for (size_t j = open; j > 0; --j) {
                    size_t k = j - 1;
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::EnumKeyword)) { enum_body = true; break; }
                    if (kind_is(tokens[k], TK::Semicolon)) break;
                }
                (void)prev;
                if (enum_body) {
                    apply_list(open, WrapListKind::EnumBody, true, true, true);
                }
                continue;
            }

            if (tokens[open].immutable.topology.starts_parameter_list) {
                bool block = opts_.module.parameter_layout != "hanging";
                bool expand = block || items.size() > 1 ||
                              (line_prefix_width(tokens, open) + 1 +
                               compact_width(tokens, open + 1, close) + 1 >
                               opts_.function_declaration.line_length);
                if (expand) {
                    apply_list(open, block ? WrapListKind::ModuleParametersBlock
                                           : WrapListKind::ModuleParametersHanging,
                               block, block, block);
                }
                continue;
            }

            if (tokens[open].immutable.topology.starts_port_list) {
                apply_list(open, WrapListKind::ModulePorts, true, true, true);
                continue;
            }

            size_t prev = prev_code(tokens, open);
            if (prev != npos && kind_is(tokens[prev], TK::Identifier)) {
                size_t before_name = prev_code(tokens, prev);
                if (before_name != npos && kind_is(tokens[before_name], TK::ModPortKeyword)) {
                    apply_list(open, WrapListKind::ModportBody, true, true, true);
                    continue;
                }
            }

            if (is_instance_port_open(tokens, open) && opts_.instance.align) {
                apply_list(open, WrapListKind::InstancePorts, true, true, true);
                continue;
            }

            bool is_decl = false;
            for (size_t j = open; j > 0; --j) {
                size_t k = j - 1;
                if (!is_code_token(tokens[k])) continue;
                if (kind_is(tokens[k], TK::FunctionKeyword) || kind_is(tokens[k], TK::TaskKeyword)) {
                    is_decl = true;
                    break;
                }
                if (kind_is(tokens[k], TK::Semicolon)) break;
            }
            if (is_decl) {
                int approx = line_prefix_width(tokens, open) + 1 + compact_width(tokens, open + 1, close) + 1;
                if (approx > opts_.function_declaration.line_length) {
                    bool hanging = opts_.function_declaration.layout == "hanging";
                    apply_list(open, hanging ? WrapListKind::FunctionDeclHanging
                                             : WrapListKind::FunctionDeclBlock,
                               !hanging, !hanging, !hanging);
                }
                continue;
            }

            if (tokens[open].immutable.topology.starts_argument_list) {
                bool do_break = false;
                if (opts_.function.break_policy == "always")
                    do_break = items.size() > 1;
                else if (opts_.function.break_policy == "auto") {
                    do_break = (opts_.function.arg_count >= 0 &&
                                static_cast<int>(items.size()) >= opts_.function.arg_count);
                    int approx = line_prefix_width(tokens, open) + 1 + compact_width(tokens, open + 1, close) + 1;
                    if (approx > opts_.function.line_length)
                        do_break = true;
                }
                if (do_break && opts_.function.break_policy != "never") {
                    bool hanging = opts_.function.layout == "hanging";
                    apply_list(open, hanging ? WrapListKind::FunctionHanging
                                             : WrapListKind::FunctionBlock,
                               !hanging, !hanging, !hanging);
                }
            }
        }
    }
private: const FormatOptions& opts_;
};

// IndentPass owns IndentMetadata and reads wrap/source facts.  The level is a
// deterministic stack over token kinds, so format(format(x)) recomputes the same
// levels from canonical output.
class IndentPass final : public IFormatPass {
public:
    explicit IndentPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "indent"; }
    void run(TokenStream& tokens) override {
        int level = 0;
        // Declaration-qualifier tracking: `import "DPI-C" function/task` and
        // `extern function/task` are header-only declarations — their function/
        // task keyword must NOT open an indent scope.  `typedef class` forward
        // declarations must NOT open a class scope either.
        bool in_import  = false; // active from ImportKeyword until ;
        bool in_extern  = false; // active from ExternKeyword until ;
        bool in_typedef = false; // active from TypedefKeyword until ;

        // Single-statement control-flow indent state
        bool ctrl_expr_pending  = false;  // seen if/for/while/foreach/repeat, waiting for (
        int  ctrl_paren_open    = 0;      // paren_depth when ( was pushed for control expr
        bool single_stmt_pending = false; // control expr closed, waiting for begin or non-begin
        bool single_stmt_active  = false; // extra +1 indent applied; waiting for ;
        int  single_stmt_paren_depth = 0; // paren depth at single_stmt start (for ; detection)
        int  paren_depth = 0;

        bool ctrl_just_closed = false; // defers single_stmt_pending resolution by one token

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (is_passthrough(t)) continue;

            // Track parens for single-stmt detection
            if (kind_is(t, TK::OpenParenthesis)) ++paren_depth;
            else if (kind_is(t, TK::CloseParenthesis) && paren_depth > 0) --paren_depth;

            // Detect control expression start
            if (is_single_stmt_control(t.lex->kind)) {
                ctrl_expr_pending = true;
            }
            if (ctrl_expr_pending && kind_is(t, TK::OpenParenthesis)) {
                ctrl_expr_pending = false;
                ctrl_paren_open = paren_depth; // depth after increment above
            }
            // Detect control expression close — set pending but don't resolve yet (defer to next token)
            if (kind_is(t, TK::CloseParenthesis) && ctrl_paren_open > 0 && paren_depth == ctrl_paren_open - 1) {
                ctrl_paren_open = 0;
                single_stmt_pending = true;
                ctrl_just_closed = true;
            }

            // Track declaration-qualifier context
            if (kind_is(t, TK::ImportKeyword))  in_import  = true;
            if (kind_is(t, TK::ExternKeyword))  in_extern  = true;
            if (kind_is(t, TK::TypedefKeyword)) in_typedef = true;
            if (kind_is(t, TK::Semicolon))      { in_import = false; in_extern = false; in_typedef = false; }

            // Compute indent — close tokens first so they dedent before assignment
            bool closes = is_close_block(t.lex->kind) || is_outer_close(t.lex->kind) ||
                          t.mutable_.macro.closes_indent_scope;
            if (closes) level = std::max(0, level - 1);

            // Resolve single-stmt pending on first non-passthrough, non-comment token AFTER control expr close
            if (single_stmt_pending && !ctrl_just_closed && !t.lex->is_comment) {
                single_stmt_pending = false;
                bool is_block = kind_is(t, TK::BeginKeyword) || kind_is(t, TK::OpenBrace);
                if (!is_block && !closes) {
                    level++;
                    single_stmt_active = true;
                    single_stmt_paren_depth = paren_depth;
                    t.mutable_.wrap.must_break_before = true;
                }
            }
            ctrl_just_closed = false;

            t.mutable_.indent.base_indent = level * opts_.indent_size;
            if (is_outer_close(t.lex->kind))
                t.mutable_.indent.base_indent = 0;
            else if (is_outer_open(t.lex->kind) && opts_.default_indent_level_inside_outmost_block == 0)
                t.mutable_.indent.base_indent = 0; // OuterOpen itself is at outer level
            if (t.mutable_.wrap.continuation)
                t.mutable_.indent.continuation_indent = opts_.indent_size;

            // Open scope after assigning indent to the opener token.
            // Suppress for function/task used as qualifiers in import/extern
            // declarations, and for class used in typedef forward declarations.
            if (!closes) {
                bool is_qualifier_fn_task =
                    (in_import || in_extern) &&
                    (kind_is(t, TK::FunctionKeyword) || kind_is(t, TK::TaskKeyword));
                bool is_typedef_class =
                    in_typedef && kind_is(t, TK::ClassKeyword);
                if (!is_qualifier_fn_task && !is_typedef_class &&
                    (is_open_block(t.lex->kind) ||
                     (is_outer_open(t.lex->kind) && opts_.default_indent_level_inside_outmost_block > 0) ||
                     t.mutable_.macro.opens_indent_scope))
                    ++level;
            }

            // Close single-stmt at semicolon at the right depth
            if (single_stmt_active && kind_is(t, TK::Semicolon) && paren_depth == single_stmt_paren_depth) {
                level = std::max(0, level - 1);
                single_stmt_active = false;
            }
        }

        auto line_start_of = [&](size_t idx) {
            size_t s = 0;
            for (size_t n = idx; n > 0; --n) {
                size_t i = n - 1;
                if (tokens[n].mutable_.wrap.must_break_before || tokens[i].mutable_.wrap.must_break_after) {
                    s = n;
                    break;
                }
            }
            return s;
        };
        auto column_before = [&](size_t idx) {
            size_t s = line_start_of(idx);
            int base = tokens[s].mutable_.indent.base_indent;
            int col = base + compact_width(tokens, s, idx);
            size_t prev = prev_code(tokens, idx);
            bool call_or_hash_paren =
                kind_is(tokens[idx], TK::OpenParenthesis) && prev != npos &&
                (is_identifier_like(tokens[prev]) || kind_is(tokens[prev], TK::Hash));
            if (idx != s && prev != npos && prev >= s &&
                !call_or_hash_paren &&
                !no_space_before(tokens[idx].lex->kind) && !no_space_after(tokens[prev].lex->kind))
                ++col;
            return col;
        };
        auto set_item_indent = [&](size_t first, size_t last, int indent) {
            if (first >= tokens.size()) return;
            tokens[first].mutable_.indent.base_indent = std::max(0, indent);
            for (size_t k = first + 1; k <= last && k < tokens.size(); ++k) {
                if (tokens[k].mutable_.wrap.must_break_before ||
                    (tokens[k].lex->is_comment && tokens[k].immutable.comment.role == CommentRole::OwnLine))
                    tokens[k].mutable_.indent.base_indent = std::max(0, indent);
            }
        };

        for (size_t open = 0; open < tokens.size(); ++open) {
            WrapListKind kind = tokens[open].mutable_.wrap.list_kind;
            if (kind == WrapListKind::None || tokens[open].mutable_.wrap.list_open != open)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos || close >= tokens.size())
                continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty())
                continue;

            int base = tokens[line_start_of(open)].mutable_.indent.base_indent;
            int item_indent = base + opts_.indent_size;
            int close_indent = base;

            size_t name = prev_code(tokens, open);
            int name_col = (name == npos) ? base : column_before(name);
            int after_open_col = column_before(open) + token_width(tokens[open]);

            switch (kind) {
            case WrapListKind::FunctionBlock:
                item_indent = name_col + opts_.indent_size;
                close_indent = name_col;
                break;
            case WrapListKind::FunctionHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                break;
            case WrapListKind::ModuleParametersBlock:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::ModuleParametersHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                break;
            case WrapListKind::FunctionDeclBlock:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::FunctionDeclHanging:
                item_indent = after_open_col;
                close_indent = after_open_col;
                break;
            case WrapListKind::InstancePorts:
                item_indent = base + std::max(0, opts_.instance.port_indent_level) * opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::ModulePorts:
                if (size_t hdr = find_header_keyword_before(tokens, open); hdr != npos)
                    base = tokens[hdr].mutable_.indent.base_indent;
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::EnumBody:
            case WrapListKind::ModportBody:
                item_indent = base + opts_.indent_size;
                close_indent = base;
                break;
            case WrapListKind::None:
                break;
            }

            for (const auto& item : items)
                set_item_indent(item.first, item.last, item_indent);
            for (size_t k = open + 1; k < close; ++k) {
                if (tokens[k].lex->is_comment &&
                    (tokens[k].immutable.comment.role == CommentRole::OwnLine ||
                     tokens[k].mutable_.wrap.must_break_before))
                    tokens[k].mutable_.indent.base_indent = std::max(0, item_indent);
            }
            if (tokens[close].mutable_.wrap.must_break_before)
                tokens[close].mutable_.indent.base_indent = std::max(0, close_indent);
        }
    }
private: const FormatOptions& opts_;
};

// AlignPass owns AlignMetadata.  Groups consecutive lines with assignment
// operators at the same indent level and aligns them.
class AlignPass final : public IFormatPass {
public:
    explicit AlignPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "alignment"; }
    void run(TokenStream& tokens) override {
        struct Line {
            size_t first{npos};
            size_t end{npos};
            size_t assign_idx{npos};
            int    lhs_width{0};
            int    indent{0};
            bool   disabled{false};
        };

        std::vector<Line> lines;
        size_t cur_first = npos;
        size_t cur_start = 0;

        auto push_line = [&](size_t end_idx) {
            Line ln;
            ln.first = cur_first;
            ln.end = end_idx;
            if (cur_first != npos)
                ln.indent = tokens[cur_first].mutable_.indent.base_indent;
            if (cur_first != npos &&
                (is_type_keyword(tokens[cur_first].lex->kind) ||
                 is_port_direction(tokens[cur_first].lex->kind)))
                ln.disabled = true;
            // Find assignment op at depth 0
            int pd = 0, bd = 0, brd = 0;
            size_t scan_start = (cur_first != npos ? cur_first : cur_start);
            for (size_t k = scan_start; k < end_idx; ++k) {
                auto& tok = tokens[k];
                if (is_passthrough(tok)) { ln.disabled = true; continue; }
                if (tok.lex->is_comment) continue;
                if (kind_is(tok, TK::OpenParenthesis))  ++pd;
                else if (kind_is(tok, TK::CloseParenthesis) && pd > 0)  --pd;
                else if (kind_is(tok, TK::OpenBracket))   ++bd;
                else if (kind_is(tok, TK::CloseBracket)  && bd > 0)   --bd;
                else if (kind_is(tok, TK::OpenBrace))    ++brd;
                else if (kind_is(tok, TK::CloseBrace)   && brd > 0)   --brd;
                if (pd == 0 && bd == 0 && brd == 0 &&
                    !tok.mutable_.macro.suppress_alignment &&
                    is_assignment_op(tok.lex->kind)) {
                    ln.assign_idx = k;
                    break;
                }
            }
            // Compute LHS width
            if (ln.assign_idx != npos) {
                ln.lhs_width = canonical_width(tokens, scan_start, ln.assign_idx);
                int identifiers_before_assign = 0;
                for (size_t k = scan_start; k < ln.assign_idx; ++k) {
                    if (is_code_token(tokens[k]) && is_identifier_like(tokens[k]))
                        ++identifiers_before_assign;
                }
                if (identifiers_before_assign >= 2)
                    ln.disabled = true;
            }
            lines.push_back(ln);
            cur_first = npos;
            cur_start = end_idx;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& tok = tokens[i];
            if (is_passthrough(tok)) continue;
            if (i > 0 && (tok.mutable_.wrap.must_break_before ||
                          tokens[i - 1].mutable_.wrap.must_break_after)) {
                push_line(i);
            }
            if (cur_first == npos) cur_first = i;
        }
        if (cur_first != npos) push_line(tokens.size());

        if (opts_.statement.align) {
            // Group consecutive alignable lines and align
            const bool space_before = wants_before(opts_.spacing.assignment_operator_spacing);

            size_t li = 0;
            while (li < lines.size()) {
                if (lines[li].disabled || lines[li].assign_idx == npos) { ++li; continue; }
                int base_indent = lines[li].indent;
                // Collect group of consecutive lines with same indent that have assign ops
                size_t j = li;
                while (j < lines.size() && !lines[j].disabled && lines[j].assign_idx != npos && lines[j].indent == base_indent)
                    ++j;
                if (j - li >= 2) {
                    int max_lhs = opts_.statement.lhs_min_width;
                    for (size_t k = li; k < j; ++k)
                        max_lhs = std::max(max_lhs, lines[k].lhs_width);
                    int target = max_lhs + (opts_.statement.align_adaptive ? 0 : 1);
                    for (size_t k = li; k < j; ++k) {
                        tokens[lines[k].assign_idx].mutable_.align.enabled = true;
                        tokens[lines[k].assign_idx].mutable_.align.target_column =
                            lines[k].indent + target;
                    }
                } else if (j - li == 1 && opts_.statement.lhs_min_width > 0) {
                    int target = opts_.tab_align
                        ? snap_to_grid(lines[li].lhs_width + 1, opts_.indent_size)
                        : opts_.statement.lhs_min_width + (space_before ? 2 : 2);
                    tokens[lines[li].assign_idx].mutable_.align.enabled = true;
                    tokens[lines[li].assign_idx].mutable_.align.target_column =
                        lines[li].indent + target;
                }
                li = j;
            }
        }

        if (opts_.var_declaration.align) {
            int declaration_line_count = 0;
            int declaration_keyword_width = 0;
            for (const auto& ln : lines) {
                if (ln.first != npos &&
                    (is_type_keyword(tokens[ln.first].lex->kind) ||
                     is_port_direction(tokens[ln.first].lex->kind))) {
                    ++declaration_line_count;
                    declaration_keyword_width = std::max(declaration_keyword_width,
                                                         token_width(tokens[ln.first]));
                }
            }
            for (const auto& ln : lines) {
                if (ln.first == npos || ln.end <= ln.first)
                    continue;
                size_t first = ln.first;
                if (!is_type_keyword(tokens[first].lex->kind) &&
                    !is_port_direction(tokens[first].lex->kind))
                    continue;

                size_t semi = npos;
                size_t eq = npos;
                int pd = 0, bd = 0, brd = 0;
                for (size_t k = first; k < ln.end; ++k) {
                    if (!is_code_token(tokens[k])) continue;
                    if (kind_is(tokens[k], TK::OpenParenthesis)) ++pd;
                    else if (kind_is(tokens[k], TK::CloseParenthesis) && pd > 0) --pd;
                    else if (kind_is(tokens[k], TK::OpenBracket)) ++bd;
                    else if (kind_is(tokens[k], TK::CloseBracket) && bd > 0) --bd;
                    else if (kind_is(tokens[k], TK::OpenBrace) || kind_is(tokens[k], TK::ApostropheOpenBrace)) ++brd;
                    else if (kind_is(tokens[k], TK::CloseBrace) && brd > 0) --brd;
                    if (pd == 0 && bd == 0 && brd == 0) {
                        if (eq == npos && kind_is(tokens[k], TK::Equals))
                            eq = k;
                        if (kind_is(tokens[k], TK::Semicolon)) {
                            semi = k;
                            break;
                        }
                    }
                }
                if (semi == npos)
                    continue;

                size_t name = npos;
                size_t name_limit = eq == npos ? semi : eq;
                for (size_t n = name_limit; n > first + 1; --n) {
                    size_t k = n - 1;
                    if (is_code_token(tokens[k]) && is_identifier_like(tokens[k])) {
                        name = k;
                        break;
                    }
                }
                if (name == npos)
                    continue;

                size_t dim = npos;
                for (size_t k = first + 1; k < name; ++k) {
                    if (kind_is(tokens[k], TK::OpenBracket)) {
                        dim = k;
                        break;
                    }
                }

                const int base = tokens[first].mutable_.indent.base_indent;
                const int section1 = option_width(opts_.var_declaration.section1_min_width, opts_);
                const int section2 = option_width(opts_.var_declaration.section2_min_width, opts_);
                const int section3 = option_width(opts_.var_declaration.section3_min_width, opts_);
                const int section4 = option_width(opts_.var_declaration.section4_min_width, opts_);

                int name_target = base + token_width(tokens[first]) + section2 + 1;
                if (dim != npos && section1 > 0) {
                    tokens[dim].mutable_.align.enabled = true;
                    tokens[dim].mutable_.align.target_column = base + section1;
                    name_target = base + section1 + section2;
                } else if (dim != npos && declaration_line_count >= 2) {
                    tokens[dim].mutable_.align.enabled = true;
                    tokens[dim].mutable_.align.target_column = base + declaration_keyword_width + 1;
                    name_target = tokens[dim].mutable_.align.target_column + section2;
                }

                const bool align_name = declaration_line_count >= 2 || dim != npos ||
                                        opts_.var_declaration.section1_min_width > 0;
                if (align_name) {
                    tokens[name].mutable_.align.enabled = true;
                    tokens[name].mutable_.align.target_column = name_target;
                }

                if (eq != npos) {
                    tokens[eq].mutable_.align.enabled = true;
                    tokens[eq].mutable_.align.target_column = name_target + section3;
                    if (semi != npos && !opts_.var_declaration.align_adaptive) {
                        tokens[semi].mutable_.align.enabled = true;
                        tokens[semi].mutable_.align.target_column = tokens[eq].mutable_.align.target_column + section4;
                    }
                } else if (semi != npos) {
                    tokens[semi].mutable_.align.enabled = true;
                    tokens[semi].mutable_.align.target_column =
                        align_name ? (name_target + section3)
                                   : (base + canonical_width(tokens, first, semi) + section3 - 1);
                }
            }
        }

        auto align_declaration_items = [&](WrapListKind list_kind) {
            for (size_t open = 0; open < tokens.size(); ++open) {
                if (tokens[open].mutable_.wrap.list_kind != list_kind ||
                    tokens[open].mutable_.wrap.list_open != open)
                    continue;
                size_t close = tokens[open].immutable.syntax.matching_token;
                if (close == npos) continue;
                auto items = top_level_list_items(tokens, open + 1, close);
                struct Decl { size_t first, type_first, name; int typew; };
                std::vector<Decl> decls;
                int type_width = opts_.port_declaration.section2_min_width;
                for (const auto& item : items) {
                    if (!is_port_direction(tokens[item.first].lex->kind))
                        continue;
                    size_t name = npos;
                    int pd = 0, bd = 0, brd = 0;
                    for (size_t n = item.last + 1; n > item.first + 1; --n) {
                        size_t k = n - 1;
                        if (!is_code_token(tokens[k])) continue;
                        if (kind_is(tokens[k], TK::CloseParenthesis)) ++pd;
                        else if (kind_is(tokens[k], TK::OpenParenthesis) && pd > 0) --pd;
                        else if (kind_is(tokens[k], TK::CloseBracket)) ++bd;
                        else if (kind_is(tokens[k], TK::OpenBracket) && bd > 0) --bd;
                        else if (kind_is(tokens[k], TK::CloseBrace)) ++brd;
                        else if ((kind_is(tokens[k], TK::OpenBrace) ||
                                  kind_is(tokens[k], TK::ApostropheOpenBrace)) && brd > 0) --brd;
                        if (pd == 0 && bd == 0 && brd == 0 && is_identifier_like(tokens[k])) {
                            name = k;
                            break;
                        }
                    }
                    if (name == npos || name <= item.first)
                        continue;
                    size_t type_first = next_code(tokens, item.first + 1, name);
                    int tw = type_first == npos ? 0 : canonical_width(tokens, type_first, name);
                    type_width = std::max(type_width, tw);
                    decls.push_back({item.first, type_first, name, tw});
                }
                int base = decls.empty() ? 0 : tokens[decls.front().first].mutable_.indent.base_indent;
                int dir_target = base + option_width(opts_.port_declaration.section1_min_width, opts_);
                int name_target = dir_target + type_width;
                for (const auto& d : decls) {
                    if (d.type_first != npos) {
                        tokens[d.type_first].mutable_.align.enabled = true;
                        tokens[d.type_first].mutable_.align.target_column = dir_target;
                        name_target = std::max(name_target, dir_target + d.typew + 3);
                    }
                }
                for (const auto& d : decls) {
                    tokens[d.name].mutable_.align.enabled = true;
                    tokens[d.name].mutable_.align.target_column = name_target;
                }
            }
        };

        if (opts_.port_declaration.align)
            align_declaration_items(WrapListKind::ModulePorts);

        // Instance named-port alignment.  WrapPass owns the decision to expand
        // the list; AlignPass only assigns target columns for the connection
        // parens and optional inside padding.
        int group = 1000;
        for (size_t open = 0; open < tokens.size(); ++open) {
            if (tokens[open].mutable_.wrap.list_kind != WrapListKind::InstancePorts ||
                tokens[open].mutable_.wrap.list_open != open)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos) continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            int max_port = option_width(opts_.instance.instance_port_name_width, opts_);
            int max_sig = option_width(opts_.instance.instance_port_between_paren_width, opts_);
            struct Conn { size_t name, op, cl; int namew, sigw; };
            std::vector<Conn> conns;
            for (const auto& item : items) {
                size_t dot = item.first;
                size_t name = next_code(tokens, dot + 1, item.last + 1);
                size_t op = name == npos ? npos : next_code(tokens, name + 1, item.last + 1);
                if (name == npos || op == npos || !kind_is(tokens[dot], TK::Dot) ||
                    !kind_is(tokens[op], TK::OpenParenthesis))
                    continue;
                size_t cl = tokens[op].immutable.syntax.matching_token;
                if (cl == npos || cl > item.last) continue;
                int nw = token_width(tokens[name]);
                int sw = compact_width(tokens, op + 1, cl);
                max_port = std::max(max_port, nw);
                max_sig = std::max(max_sig, sw);
                conns.push_back({name, op, cl, nw, sw});
            }
            for (const auto& c : conns) {
                int item_indent = tokens[prev_code(tokens, c.name)].mutable_.indent.base_indent;
                int port_width = opts_.instance.align_adaptive
                    ? std::max(option_width(opts_.instance.instance_port_name_width, opts_), c.namew)
                    : max_port;
                int sig_width = opts_.instance.align_adaptive
                    ? std::max(option_width(opts_.instance.instance_port_between_paren_width, opts_), c.sigw)
                    : max_sig;
                tokens[c.op].mutable_.align.enabled = true;
                tokens[c.op].mutable_.align.alignment_group = group;
                int configured_port_width = option_width(opts_.instance.instance_port_name_width, opts_);
                int port_extra = opts_.instance.align_adaptive
                    ? (c.namew < configured_port_width ? 1 : 2)
                    : (max_port > configured_port_width ? 2 : 0);
                tokens[c.op].mutable_.align.target_column = item_indent + port_width + port_extra;
                tokens[c.cl].mutable_.align.enabled = true;
                tokens[c.cl].mutable_.align.alignment_group = group;
                tokens[c.cl].mutable_.align.target_column =
                    tokens[c.op].mutable_.align.target_column + 1 + sig_width;
            }
            ++group;
        }

        for (size_t open = 0; open < tokens.size(); ++open) {
            if (tokens[open].mutable_.wrap.list_open != open)
                continue;
            size_t close = tokens[open].immutable.syntax.matching_token;
            if (close == npos) continue;
            auto items = top_level_list_items(tokens, open + 1, close);
            if (items.empty()) continue;

            if (tokens[open].mutable_.wrap.list_kind == WrapListKind::EnumBody &&
                opts_.enum_declaration.align) {
                struct E { size_t first, eq, comma; int namew, valw; };
                std::vector<E> es;
                int namew = option_width(opts_.enum_declaration.enum_name_min_width, opts_);
                int valw = option_width(opts_.enum_declaration.enum_value_min_width, opts_);
                for (const auto& item : items) {
                    size_t eq = npos;
                    for (size_t k = item.first; k <= item.last; ++k)
                        if (kind_is(tokens[k], TK::Equals)) { eq = k; break; }
                    int nw = eq == npos ? compact_width(tokens, item.first, item.last + 1)
                                         : compact_width(tokens, item.first, eq);
                    int vw = eq == npos ? 0 : compact_width(tokens, eq + 1, item.last + 1);
                    namew = std::max(namew, nw);
                    valw = std::max(valw, vw);
                    es.push_back({item.first, eq, item.comma, nw, vw});
                }
                int base = tokens[items.front().first].mutable_.indent.base_indent;
                for (const auto& e : es) {
                    int local_namew = opts_.enum_declaration.align_adaptive
                        ? std::max(option_width(opts_.enum_declaration.enum_name_min_width, opts_), e.namew)
                        : namew;
                    int local_valw = opts_.enum_declaration.align_adaptive
                        ? std::max(option_width(opts_.enum_declaration.enum_value_min_width, opts_), e.valw)
                        : valw;
                    int eq_target = base + local_namew + 1;
                    if (e.eq != npos) {
                        tokens[e.eq].mutable_.align.enabled = true;
                        tokens[e.eq].mutable_.align.target_column = eq_target;
                    }
                    if (e.comma != npos) {
                        tokens[e.comma].mutable_.align.enabled = true;
                        tokens[e.comma].mutable_.align.target_column =
                            (e.eq == npos) ? (eq_target + 3 + local_valw)
                                           : (eq_target + 2 + local_valw);
                    }
                }
            }

            if (tokens[open].mutable_.wrap.list_kind == WrapListKind::ModportBody &&
                opts_.modport.align) {
                struct M { size_t dir, sig, comma; int dirw, sigw; };
                std::vector<M> ms;
                int dirw = option_width(opts_.modport.direction_min_width, opts_);
                int sigw = option_width(opts_.modport.signal_min_width, opts_);
                for (const auto& item : items) {
                    size_t sig = next_code(tokens, item.first + 1, item.last + 1);
                    if (sig == npos) continue;
                    int dw = token_width(tokens[item.first]);
                    int sw = compact_width(tokens, sig, item.last + 1);
                    dirw = std::max(dirw, dw + 1);
                    sigw = std::max(sigw, sw);
                    ms.push_back({item.first, sig, item.comma, dw, sw});
                }
                int base = tokens[items.front().first].mutable_.indent.base_indent;
                for (const auto& m : ms) {
                    int local_sigw = opts_.modport.align_adaptive
                        ? std::max(option_width(opts_.modport.signal_min_width, opts_), m.sigw)
                        : sigw;
                    tokens[m.sig].mutable_.align.enabled = true;
                    tokens[m.sig].mutable_.align.target_column = base + dirw;
                    if (m.comma != npos) {
                        tokens[m.comma].mutable_.align.enabled = true;
                        tokens[m.comma].mutable_.align.target_column = base + dirw + local_sigw;
                    }
                }
            }
        }
    }
private: const FormatOptions& opts_;
};

// CommentPass owns CommentMetadata and reads stable syntax comment roles.
class CommentPass final : public IFormatPass {
public:
    const char* name() const override { return "comment"; }
    void run(TokenStream& tokens) override {
        for (auto& t : tokens)
            if (t.lex->is_comment && t.immutable.comment.role == CommentRole::OwnLine)
                t.mutable_.comment.force_own_line = true;
    }
};

// SpacingPass owns SpaceMetadata.  It reads upstream wrap/comment metadata to
// avoid assigning in-line spaces before tokens that will start a line.
class SpacingPass final : public IFormatPass {
public:
    explicit SpacingPass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "spacing"; }
    void run(TokenStream& tokens) override {
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto& t = tokens[i];
            if (i == 0 || t.mutable_.wrap.must_break_before || t.mutable_.comment.force_own_line || is_passthrough(t)) {
                t.mutable_.space.spaces_before = 0;
                continue;
            }
            const Tok& L = tokens[i - 1];
            int spaces = 1;

            // Basic no-space rules
            if (no_space_before(t.lex->kind) || no_space_after(L.lex->kind)) spaces = 0;
            if (t.lex->is_comment && kind_is(L, TK::OpenParenthesis))
                spaces = 1;
            // Empty positional argument: `, ,` — keep one space so the slot is visible
            if (kind_is(t, TK::Comma) && kind_is(L, TK::Comma)) spaces = 1;

            // Unary ops: no space after.
            // Exception: `~ &a`, `~ |a`, `~ ^a` — tilde followed by a
            // reduction operator must keep a space so it isn't read as
            // the compound `~&` / `~|` / `~^` operator.
            if (is_unary_op(L.lex->kind)) {
                bool tilde_before_reduction = kind_is(L, TK::Tilde) &&
                    (kind_is(t, TK::And) || kind_is(t, TK::Or) || kind_is(t, TK::Xor));
                if (!tilde_before_reduction) {
                    t.mutable_.space.spaces_before = 0;
                    t.mutable_.space.suppress_space = true;
                    continue;
                }
            }

            // Hierarchy: . and ::
            if (kind_is(L, TK::Dot) || kind_is(L, TK::DoubleColon)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }
            if ((kind_is(t, TK::Dot) && kind_is(L, TK::Comma))) {
                spaces = 1;
            } else if (kind_is(t, TK::Dot) || kind_is(t, TK::DoubleColon)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // # hash: no space before next token
            if (kind_is(L, TK::Hash)) {
                t.mutable_.space.spaces_before = 0;
                t.mutable_.space.suppress_space = true;
                continue;
            }

            // No space before '[' when it's an index/dimension on an identifier or closer
            if (kind_is(t, TK::OpenBracket) &&
                (is_identifier_like(L) || kind_is(L, TK::CloseBracket) || kind_is(L, TK::CloseParenthesis)))
                spaces = 0;

            // Function/task call spacing
            if (kind_is(t, TK::OpenParenthesis) && (kind_is(L, TK::Identifier) || kind_is(L, TK::SystemIdentifier) || kind_is(L, TK::MacroUsage)))
                spaces = opts_.function.space_before_paren ? 1 : 0;
            if (kind_is(t, TK::OpenParenthesis) && t.mutable_.wrap.list_kind == WrapListKind::InstancePorts)
                spaces = 1;
            if (kind_is(t, TK::OpenParenthesis) && t.mutable_.wrap.list_kind == WrapListKind::ModportBody)
                spaces = 1;
            if (kind_is(t, TK::OpenParenthesis) && t.immutable.topology.starts_port_list &&
                kind_is(L, TK::CloseParenthesis))
                spaces = 0;
            if (kind_is(t, TK::OpenParenthesis) && is_control_keyword(L.lex->kind))
                spaces = opts_.spacing.control_keyword_space ? 1 : 0;
            if ((kind_is(L, TK::OpenParenthesis) || kind_is(t, TK::CloseParenthesis)) && opts_.spacing.space_inside_parens) spaces = 1;
            // function.space_inside_paren: space inside argument-list parens only
            if (kind_is(L, TK::OpenParenthesis) && L.immutable.topology.starts_argument_list && opts_.function.space_inside_paren) spaces = 1;
            if (kind_is(t, TK::CloseParenthesis) && t.immutable.topology.ends_argument_list && opts_.function.space_inside_paren) spaces = 1;
            if ((kind_is(L, TK::OpenBracket) || kind_is(t, TK::CloseBracket)) && opts_.spacing.space_inside_dimension_brackets) spaces = 1;

            // } brace: 1 space after (unless followed by ; or ,)
            if (kind_is(L, TK::CloseBrace) && !kind_is(t, TK::Semicolon) && !kind_is(t, TK::Comma)) spaces = 1;
            if (kind_is(L, TK::CloseBrace) && kind_is(t, TK::CloseParenthesis)) spaces = 0;

            // Apostrophe / cast: no space
            if (kind_is(t, TK::Apostrophe) || kind_is(L, TK::Apostrophe)) spaces = 0;
            // '{ casts (text starts with ')
            if (!t.lex->text.empty() && t.lex->text[0] == '\'') spaces = 0;

            // Postfix ++ / --: no space before when attached to an identifier, ], or )
            if ((kind_is(t, TK::DoublePlus) || kind_is(t, TK::DoubleMinus)) &&
                (is_identifier_like(L) || kind_is(L, TK::CloseBracket) || kind_is(L, TK::CloseParenthesis)))
                spaces = 0;

            // Assignment operators.
            // LessThanEquals is context-sensitive: inside parens it's a comparison, not
            // non-blocking assignment.  Treat it as a regular binary op in that context.
            auto is_assign = [&](const Tok& tok) {
                if (tok.lex->kind == TK::LessThanEquals && tok.immutable.syntax.paren_depth > 0)
                    return false;
                return is_assignment_op(tok.lex->kind);
            };
            if (is_assign(t)) spaces = wants_before(opts_.spacing.assignment_operator_spacing) ? 1 : 0;
            if (is_assign(L)) spaces = wants_after(opts_.spacing.assignment_operator_spacing) ? 1 : 0;

            // Binary operators (non-assignment).
            // Closing brackets carry depth=1 (before decrement), so exclude them from the
            // L-side dim check to avoid treating tokens after ] as inside a dimension.
            bool in_dim = t.immutable.syntax.bracket_depth > 0 ||
                          (L.immutable.syntax.bracket_depth > 0 && !kind_is(L, TK::CloseBracket));
            const std::string& bop_mode = in_dim ? opts_.spacing.dimension_binary_operator_spacing : opts_.spacing.binary_operator_spacing;
            if (is_binary_op(t.lex->kind) && !is_assign(t))
                spaces = wants_before(bop_mode) ? 1 : 0;
            if (is_binary_op(L.lex->kind) && !is_assign(L))
                spaces = wants_after(bop_mode) ? 1 : 0;
            // `inside` is a keyword operator — always needs spaces regardless of bop_mode
            if (kind_is(t, TK::InsideKeyword)) spaces = 1;
            if (kind_is(L, TK::InsideKeyword)) spaces = 1;

            // Range/part-select
            if (kind_is(t, TK::Colon) && in_dim) spaces = wants_before(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (kind_is(L, TK::Colon) && in_dim) spaces = wants_after(opts_.spacing.range_colon_spacing) ? 1 : 0;
            if (kind_is(t, TK::PlusColon) || kind_is(t, TK::MinusColon)) spaces = wants_before(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;
            if (kind_is(L, TK::PlusColon) || kind_is(L, TK::MinusColon)) spaces = wants_after(opts_.spacing.indexed_part_select_spacing) ? 1 : 0;

            // wait keyword: no space before ( (like a function call, not a control keyword)
            if (kind_is(t, TK::OpenParenthesis) && kind_is(L, TK::WaitKeyword)) spaces = 0;

            // @ event control spacing.
            // Standalone delay-control `@ (...)` appears after `;` — suppress spacing for it.
            // Only `always @`, `always_ff @`, `always_comb @`, `always_latch @`, `covergroup @`
            // etc. get the event-control spacing applied.
            if (kind_is(t, TK::At)) {
                bool standalone = kind_is(L, TK::Semicolon);
                spaces = (!standalone && wants_before(opts_.spacing.procedural_event_control_at_spacing)) ? 1 : 0;
            }
            if (kind_is(L, TK::At)) {
                bool standalone = i >= 2 && kind_is(tokens[i-2], TK::Semicolon);
                spaces = (!standalone && wants_after(opts_.spacing.procedural_event_control_at_spacing)) ? 1 : 0;
            }
            // space_inside_event_control_parens: add space inside ( ) of procedural event control.
            // Only applies when ( directly follows @ which is not a standalone delay control.
            if (opts_.spacing.space_inside_event_control_parens) {
                if (kind_is(L, TK::OpenParenthesis) && i >= 2 && kind_is(tokens[i-2], TK::At)) {
                    bool standalone = i >= 3 && kind_is(tokens[i-3], TK::Semicolon);
                    if (!standalone) spaces = 1;
                }
                if (kind_is(t, TK::CloseParenthesis) && t.immutable.syntax.matching_token != npos) {
                    size_t j = t.immutable.syntax.matching_token;
                    if (j >= 1 && j < tokens.size() && kind_is(tokens[j], TK::OpenParenthesis) &&
                        j >= 1 && kind_is(tokens[j-1], TK::At)) {
                        bool standalone = j >= 2 && kind_is(tokens[j-2], TK::Semicolon);
                        if (!standalone) spaces = 1;
                    }
                }
            }

            // End-label colon: `endclass: Foo`, `endfunction: bar`, etc. — no space before `:`
            if (kind_is(t, TK::Colon) && (is_close_block(L.lex->kind) || is_outer_close(L.lex->kind) ||
                                           kind_is(L, TK::BeginKeyword) || kind_is(L, TK::ForkKeyword)))
                spaces = 0;

            // semicolon_spacing: controls space before/after `;` inside for-loop headers
            // (paren_depth > 0 identifies the for(;;) context vs statement-ending `;`)
            if (kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth > 0)
                spaces = wants_before(opts_.spacing.semicolon_spacing) ? 1 : 0;
            if (kind_is(L, TK::Semicolon) && L.immutable.syntax.paren_depth > 0)
                spaces = wants_after(opts_.spacing.semicolon_spacing) ? 1 : 0;

            if ((kind_is(t, TK::Semicolon) && t.immutable.syntax.paren_depth == 0) ||
                (kind_is(t, TK::Comma) && !kind_is(L, TK::Comma)) ||
                (kind_is(t, TK::Dot) && !kind_is(L, TK::Comma)) || kind_is(t, TK::DoubleColon))
                spaces = 0;
            if (kind_is(t, TK::CloseParenthesis) &&
                !opts_.spacing.space_inside_parens &&
                !(t.immutable.topology.ends_argument_list && opts_.function.space_inside_paren))
                spaces = 0;

            t.mutable_.space.spaces_before = std::max(0, spaces);
            t.mutable_.space.suppress_space = spaces == 0;
        }
    }
private: const FormatOptions& opts_;
};

// BlankLinePass owns BlankLineMetadata.  It intentionally does not copy the
// original source's blank-line count: once a syntactic blank-line boundary is
// accepted, the rendered amount is canonical and config-driven.
class BlankLinePass final : public IFormatPass {
public:
    explicit BlankLinePass(const FormatOptions& opts) : opts_(opts) {}
    const char* name() const override { return "blank_line"; }
    void run(TokenStream& tokens) override {
        for (auto& t : tokens)
            if (t.immutable.input_trivia.original_newlines_before > 1 &&
                t.immutable.syntax.paren_depth == 0 &&
                t.immutable.syntax.brace_depth == 0)
                t.mutable_.blank.before = std::max(0, opts_.blank_lines_between_items);
    }
private: const FormatOptions& opts_;
};

} // namespace svfmt
