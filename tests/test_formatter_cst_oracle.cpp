// The formatter decides what a token is from the tokens around it; slang's
// parser decides from the grammar.  For every corpus file slang parses
// cleanly, this checks the facts SyntaxPass and WrapPass derive against the
// syntax node that owns the same source token.  The formatter keeps its own
// heuristics at run time -- it must format code that does not parse -- and
// this test is what keeps those heuristics honest on code that does.
// (docs/dev/formatter-contracts.md)
#include <catch2/catch_test_macros.hpp>

#include "config.hpp"
#include "features/formatter_lexer.hpp"
#include "features/formatter_renderer.hpp"
#include "formatter_corpus.hpp"

#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

using slang::parsing::TokenKind;
using slang::syntax::SyntaxKind;
using slang::syntax::SyntaxNode;

// The syntax node a source token belongs to, and that node's own parent.
// slang v12 flattens lists into their owner, so no list node sits between.
struct Owner {
    TokenKind kind;
    SyntaxKind owner;
    SyntaxKind grandparent;
};

void collect(const SyntaxNode& node, const slang::SourceManager& sm, std::map<size_t, Owner>& out) {
    const SyntaxNode* owner = &node;
    for (size_t i = 0; i < node.getChildCount(); ++i) {
        if (const SyntaxNode* child = node.childNode(i)) {
            collect(*child, sm, out);
            continue;
        }
        const slang::parsing::Token tok = node.childToken(i);
        if (!tok || tok.isMissing() || !owner)
            continue;
        const slang::SourceLocation loc = tok.location();
        if (sm.isMacroLoc(loc) || !sm.isFileLoc(loc))
            continue;
        const SyntaxNode* gp = owner->parent;
        out[loc.offset()] = {tok.kind, owner->kind, gp ? gp->kind : SyntaxKind::Unknown};
    }
}

struct Fact {
    const char* name;
    // What the formatter decided for the token.
    std::function<bool(const svfmt::Tok&)> formatter;
    // What the grammar says.
    std::function<bool(const Owner&)> grammar;
    // The formatter's fact is wider than the grammar's node on purpose
    // (`starts_argument_list` is any call-shaped `(`, instances and gate
    // terminals included); only a grammar `yes` it missed is a mismatch.
    bool one_way = false;
};

bool is_unit_keyword(TokenKind k) {
    return k == TokenKind::ModuleKeyword || k == TokenKind::MacromoduleKeyword || k == TokenKind::InterfaceKeyword ||
           k == TokenKind::ProgramKeyword || k == TokenKind::PackageKeyword;
}

bool is_subroutine_keyword(TokenKind k) { return k == TokenKind::FunctionKeyword || k == TokenKind::TaskKeyword; }

const std::vector<Fact>& facts() {
    static const std::vector<Fact> table = {
        // Every unit with an end keyword of its own, classes and checkers
        // included.
        {"opens_design_unit", [](const svfmt::Tok& t) { return t.immutable.topology.opens_design_unit; },
         [](const Owner& o) {
             switch (o.kind) {
             case TokenKind::ClassKeyword: return o.owner == SyntaxKind::ClassDeclaration;
             case TokenKind::CheckerKeyword: return o.owner == SyntaxKind::CheckerDeclaration;
             case TokenKind::PrimitiveKeyword: return o.owner == SyntaxKind::UdpDeclaration;
             case TokenKind::ConfigKeyword: return o.owner == SyntaxKind::ConfigDeclaration;
             default:
                 return is_unit_keyword(o.kind) &&
                        (o.owner == SyntaxKind::ModuleHeader || o.owner == SyntaxKind::ProgramHeader ||
                         o.owner == SyntaxKind::PackageHeader || o.owner == SyntaxKind::InterfaceHeader);
             }
         }},
        // A `function`/`task` with no body: extern, DPI, pure virtual, a
        // modport's import.  A `typedef class c;` forward declaration too.
        {"is_prototype", [](const svfmt::Tok& t) { return t.immutable.topology.is_prototype; },
         [](const Owner& o) {
             if (o.kind == TokenKind::ClassKeyword)
                 return o.owner == SyntaxKind::ForwardTypedefDeclaration;
             if (!is_subroutine_keyword(o.kind))
                 return false;
             return o.owner == SyntaxKind::DPIExport ||
                    (o.owner == SyntaxKind::FunctionPrototype && o.grandparent != SyntaxKind::FunctionDeclaration &&
                     o.grandparent != SyntaxKind::TaskDeclaration);
         }},
        // A unit header's ports; a modport's list is laid out as a modport.
        {"starts_port_list", [](const svfmt::Tok& t) { return t.immutable.topology.starts_port_list; },
         [](const Owner& o) {
             return o.kind == TokenKind::OpenParenthesis && o.grandparent != SyntaxKind::ModportItem &&
                    (o.owner == SyntaxKind::AnsiPortList || o.owner == SyntaxKind::NonAnsiPortList);
         }},
        {"is_case_item_colon", [](const svfmt::Tok& t) { return t.immutable.topology.is_case_item_colon; },
         [](const Owner& o) {
             return o.kind == TokenKind::Colon &&
                    (o.owner == SyntaxKind::StandardCaseItem || o.owner == SyntaxKind::DefaultCaseItem ||
                     o.owner == SyntaxKind::PatternCaseItem || o.owner == SyntaxKind::RandCaseItem);
         }},
        // The inner `{` of `{4{a}}` and of `'{2{x}}`.
        {"is_replication_brace", [](const svfmt::Tok& t) { return t.immutable.topology.is_replication_brace; },
         [](const Owner& o) {
             return o.kind == TokenKind::OpenBrace &&
                    ((o.owner == SyntaxKind::ConcatenationExpression &&
                      o.grandparent == SyntaxKind::MultipleConcatenationExpression) ||
                     o.owner == SyntaxKind::ReplicatedAssignmentPattern);
         }},
        {"enum body",
         [](const svfmt::Tok& t) {
             return t.lex.kind == TokenKind::OpenBrace && t.mutable_.wrap.list_kind == svfmt::WrapListKind::EnumBody;
         },
         [](const Owner& o) { return o.kind == TokenKind::OpenBrace && o.owner == SyntaxKind::EnumType; }},
        {"is_block_name_colon", [](const svfmt::Tok& t) { return t.immutable.topology.is_block_name_colon; },
         [](const Owner& o) { return o.kind == TokenKind::Colon && o.owner == SyntaxKind::NamedBlockClause; }},
        {"starts_argument_list", [](const svfmt::Tok& t) { return t.immutable.topology.starts_argument_list; },
         [](const Owner& o) { return o.kind == TokenKind::OpenParenthesis && o.owner == SyntaxKind::ArgumentList; },
         true},
        // `#(` also opens a delay, which the formatter lays out the same way.
        {"starts_parameter_list", [](const svfmt::Tok& t) { return t.immutable.topology.starts_parameter_list; },
         [](const Owner& o) {
             return o.kind == TokenKind::OpenParenthesis &&
                    (o.owner == SyntaxKind::ParameterPortList || o.owner == SyntaxKind::ParameterValueAssignment);
         },
         true},
    };
    return table;
}

} // namespace

TEST_CASE("formatter facts agree with the slang syntax tree", "[formatter][cst-oracle]") {
    std::vector<std::string> mismatches;
    int checked_files = 0;
    for (const lvtest::CorpusFile& f : lvtest::corpus()) {
        auto tree = slang::syntax::SyntaxTree::fromText(f.text);
        bool clean = true;
        for (const auto& d : tree->diagnostics())
            clean = clean && !d.isError();
        if (!clean)
            continue;
        ++checked_files;
        std::map<size_t, Owner> owners;
        collect(tree->root(), tree->sourceManager(), owners);

        FormatOptions opts;
        svfmt::TokenStream tokens = svfmt::TokenCollector(f.text, opts).collect();
        svfmt::run_format_passes(tokens, opts);
        for (const svfmt::Tok& t : tokens) {
            if (!svfmt::is_code_token(t) || !t.lex.range.start().valid())
                continue;
            const auto it = owners.find(t.lex.range.start().offset());
            // Inactive `ifdef branches and macro bodies have no syntax node.
            if (it == owners.end() || it->second.kind != t.lex.kind)
                continue;
            for (const Fact& fact : facts()) {
                const bool ours = fact.formatter(t), theirs = fact.grammar(it->second);
                if (ours == theirs || (fact.one_way && ours) || mismatches.size() >= 60)
                    continue;
                const size_t off = t.lex.range.start().offset();
                const size_t bol = f.text.rfind('\n', off == 0 ? 0 : off - 1);
                const size_t eol = f.text.find('\n', off);
                const std::string line = f.text.substr(bol == std::string::npos ? 0 : bol + 1,
                                                       (eol == std::string::npos ? f.text.size() : eol) -
                                                           (bol == std::string::npos ? 0 : bol + 1));
                mismatches.push_back(std::string(fact.name) + (ours ? " set, grammar says no" : " missing") +
                                     ": `" + t.lex.text + "` in " + f.name + " (" +
                                     std::string(slang::syntax::toString(it->second.owner)) + "): " + line);
            }
        }
    }
    INFO("files checked: " << checked_files);
    std::string text;
    for (const auto& m : mismatches) text += m + "\n";
    INFO(text);
    CHECK(checked_files >= 10);
    CHECK(mismatches.empty());
}
