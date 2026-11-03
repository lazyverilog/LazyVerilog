#include "features/fix_slang.hpp"

#include "lsp_position.hpp"

#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Scope.h>
#include <slang/ast/types/AllTypes.h>
#include <slang/diagnostics/ExpressionsDiags.h>

#include <any>
#include <concepts>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace {

using namespace slang;
using namespace slang::ast;

const Type* type_arg(const Diagnostic& d, size_t index) {
    if (index >= d.args.size())
        return nullptr;
    auto custom = std::get_if<Diagnostic::CustomArgType>(&d.args[index]);
    if (!custom)
        return nullptr;
    auto type = std::any_cast<const Type*>(&custom->second);
    return type ? *type : nullptr;
}

std::optional<uint64_t> width_arg(const Diagnostic& d, size_t index) {
    if (index >= d.args.size())
        return std::nullopt;
    auto value = std::get_if<uint64_t>(&d.args[index]);
    return value ? std::optional(*value) : std::nullopt;
}

// A non-empty range written in the diagnostic's own file, outside any macro.
bool editable(const SourceManager& sm, const Diagnostic& d, SourceRange range) {
    auto start = range.start(), end = range.end();
    if (!start.valid() || !end.valid() || !d.location.valid())
        return false;
    if (!sm.isFileLoc(start) || !sm.isFileLoc(end) || start.buffer() != end.buffer() ||
        start.offset() >= end.offset())
        return false;
    return start.buffer() == sm.getFullyExpandedLoc(d.location).buffer();
}

FixEdit insert_at(const SourceManager& sm, SourceLocation loc, std::string text) {
    const int line = static_cast<int>(sm.getFileLineNumber(loc)) - 1;
    const int col = lsp_column(sm, loc);
    return {line, col, line, col, std::move(text)};
}

std::vector<FixInfo> wrap(const SourceManager& sm, SourceRange range, std::string open,
                          std::string close, std::string title) {
    FixInfo fix;
    fix.title = std::move(title);
    fix.edits.push_back(insert_at(sm, range.start(), std::move(open)));
    fix.edits.push_back(insert_at(sm, range.end(), std::move(close)));
    return {std::move(fix)};
}

// The operand's text for a title: whitespace runs collapsed, at most 40 chars.
std::string title_text(const SourceManager& sm, SourceRange range) {
    auto text = sm.getSourceText(range.start().buffer())
                    .substr(range.start().offset(), range.end().offset() - range.start().offset());
    std::string out;
    for (char c : text) {
        const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (!space)
            out += c;
        else if (!out.empty() && out.back() != ' ')
            out += ' ';
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    if (out.size() > 40)
        out = out.substr(0, 37) + "...";
    return out;
}

// What a scope binds at one source range: whether it is an output/inout
// argument or port (bound as `actual = <placeholder>`), the outermost implicit
// conversion there, and the outermost other expression there.
struct RangeFacts {
    bool lvalue = false;
    const ConversionExpression* conversion = nullptr;
    const Expression* expr = nullptr;
};

struct RangeHash {
    size_t operator()(SourceRange range) const {
        size_t h = 0;
        for (auto loc : {range.start(), range.end()})
            h = (h * 1000003) ^ (size_t(loc.buffer().getId()) * 31 + size_t(loc.offset()));
        return h;
    }
};

using ScopeFacts = std::unordered_map<SourceRange, RangeFacts, RangeHash>;

struct FactCollector : ASTVisitor<FactCollector, VisitFlags::AllGood> {
    explicit FactCollector(ScopeFacts& f) : facts(f) {}

    void handle(const AssignmentExpression& expr) {
        if (expr.isLValueArg())
            facts[expr.left().sourceRange].lvalue = true;
        visitDefault(expr);
    }

    void handle(const ConversionExpression& expr) {
        auto& f = facts[expr.sourceRange];
        if (!expr.isImplicit()) {
            if (!f.expr)
                f.expr = &expr;
        }
        else if (!f.conversion) {
            f.conversion = &expr;
        }
        visitDefault(expr);
    }

    template<std::derived_from<Expression> T>
    void handle(const T& expr) {
        auto& f = facts[expr.sourceRange];
        if (!f.expr)
            f.expr = &expr;
        visitDefault(expr);
    }

    // Only the connections belong to this scope; the body is another one.
    void handle(const InstanceSymbol& symbol) {
        for (auto connection : symbol.getPortConnections())
            if (auto expr = connection->getExpression())
                expr->visit(*this);
    }

    ScopeFacts& facts;
};

} // namespace

struct SlangFixContext::Impl {
    std::unordered_map<const Symbol*, ScopeFacts> scopes;
};

SlangFixContext::SlangFixContext() : impl(std::make_unique<Impl>()) {}
SlangFixContext::~SlangFixContext() = default;

namespace {

// The facts at @p range in the diagnostic's scope, walking it on first use.
// nullopt when the diagnostic names no scope.
std::optional<RangeFacts> facts_at(const Diagnostic& d, SourceRange range,
                                   SlangFixContext& context) {
    if (!d.symbol)
        return std::nullopt;
    auto [it, fresh] = context.impl->scopes.try_emplace(d.symbol);
    if (fresh) {
        try {
            FactCollector collector(it->second);
            d.symbol->visit(collector);
        } catch (...) {
            // A half-walked scope could miss an output argument.
            context.impl->scopes.erase(it);
            throw;
        }
    }
    auto found = it->second.find(range);
    return found == it->second.end() ? RangeFacts{} : found->second;
}

// The name to write @p type as in a cast at the diagnostic's scope: a typedef
// that its plain name finds there, or a built-in keyword type.
std::optional<std::string> type_name(const Type& type, const Symbol* where) {
    if (type.isAlias()) {
        auto scope = where ? where->as_if<Scope>() : nullptr;
        if (!scope || scope->lookupName(type.name) != &type)
            return std::nullopt;
        return std::string(type.name);
    }
    switch (type.kind) {
        case SymbolKind::PredefinedIntegerType: {
            auto& predefined = type.as<PredefinedIntegerType>();
            if (predefined.isSigned !=
                PredefinedIntegerType::isDefaultSigned(predefined.integerKind))
                return std::nullopt;
            return type.toString();
        }
        case SymbolKind::ScalarType:
            if (type.isSigned())
                return std::nullopt;
            return type.toString();
        case SymbolKind::FloatingType:
            return type.toString();
        default:
            return std::nullopt;
    }
}

bool is_conversion(DiagCode code) {
    return code == diag::WidthTruncate || code == diag::WidthExpand ||
           code == diag::PortWidthTruncate || code == diag::PortWidthExpand ||
           code == diag::ImplicitConvert || code == diag::PackedArrayConv ||
           code == diag::SignConversion || code == diag::FloatIntConv ||
           code == diag::IntFloatConv || code == diag::FloatNarrow || code == diag::FloatWiden;
}

// checkImplicitConversions(): range 0 is the converted operand.
std::vector<FixInfo> conversion_fixes(const SourceManager& sm, const Diagnostic& d,
                                      SlangFixContext& context) {
    const auto code = d.code;
    if (d.ranges.empty() || !editable(sm, d, d.ranges[0]))
        return {};
    const auto range = d.ranges[0];

    // The scope facts confirm range 0 is an operand: a copy of the warning
    // raised while a constant function is evaluated carries the `=` there.
    // They also tell an output argument or port (no fix) from an input one.
    auto found = facts_at(d, range, context);
    if (!found || found->lvalue || (!found->expr && !found->conversion))
        return {};
    const RangeFacts facts = *found;

    // An expression whose effective sign is "either" (a comparison, a 1-bit
    // system call) is spared sign-conversion, and an N'() cast gives it a
    // sign.  Such expressions are 1 bit wide, so only those widenings check.
    const bool width_code = code == diag::WidthTruncate || code == diag::WidthExpand ||
                            code == diag::PortWidthTruncate || code == diag::PortWidthExpand;
    const bool one_bit_expand = (code == diag::WidthExpand || code == diag::PortWidthExpand) &&
                                width_arg(d, 0) == 1u;

    if (width_code) {
        auto width = width_arg(d, 1);
        if (!width)
            return {};
        std::string open = std::to_string(*width) + "'(", close = ")";
        if (one_bit_expand) {
            if (!facts.conversion)
                return {};
            const auto& operand = facts.conversion->unwrapImplicitConversions();
            const bool target_signed = facts.conversion->type->isSigned();
            if (operand.getEffectiveSign(false) == Expression::EffectiveSign::Either &&
                operand.type->isSigned() != target_signed) {
                open = (target_signed ? "signed'(" : "unsigned'(") + open;
                close += ")";
            }
        }
        return wrap(sm, range, open, close, "Cast to " + std::to_string(*width) + " bits");
    }

    auto target = type_arg(d, 1);
    if (!target)
        return {};

    if (code == diag::SignConversion) {
        // Widening extends by the operand's sign; a sign cast would extend by
        // the target's.  The width-expand fix leaves an equal-width case.  The
        // operand's own width: slang's source type here is already widened.
        if (!facts.expr || target->getBitWidth() > facts.expr->type->getBitWidth())
            return {};
        const bool to_signed = target->isSigned();
        return wrap(sm, range, to_signed ? "signed'(" : "unsigned'(", ")",
                    to_signed ? "Cast to signed" : "Cast to unsigned");
    }
    if (code == diag::ImplicitConvert || code == diag::PackedArrayConv) {
        auto name = type_name(*target, d.symbol);
        if (!name)
            return {};
        return wrap(sm, range, *name + "'(", ")", "Cast to " + *name);
    }
    if (code == diag::FloatIntConv) {
        // No N'() fallback for an unnamed target: a size cast of a real is an
        // error (BadIntegerCast).
        auto name = type_name(*target, d.symbol);
        if (!name)
            return {};
        return wrap(sm, range, *name + "'(", ")", "Convert to " + *name);
    }
    // int-float-conv, float-widen, float-narrow: the target is real or shortreal.
    auto& canonical = target->getCanonicalType();
    const bool shortreal = canonical.kind == SymbolKind::FloatingType &&
                           canonical.as<FloatingType>().floatKind == FloatingType::ShortReal;
    if (code == diag::FloatNarrow || shortreal)
        return wrap(sm, range, "shortreal'(", ")", "Convert to shortreal");
    return wrap(sm, range, "real'(", ")", "Convert to real");
}

// An operand a cast cannot change the evaluation of: inside the comparison an
// operator's operands are extended unsigned, inside a cast by their own sign.
bool is_primary(const Expression& expr) {
    switch (expr.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
        case ExpressionKind::ElementSelect:
        case ExpressionKind::RangeSelect:
        case ExpressionKind::MemberAccess:
        case ExpressionKind::Concatenation:
        case ExpressionKind::Call:
            return true;
        default:
            return false;
    }
}

// analyzeOpTypes(): args are the operand types, ranges 1 and 2 the operands.
std::vector<FixInfo> operand_fixes(const SourceManager& sm, const Diagnostic& d,
                                   SlangFixContext& context) {
    auto lt = type_arg(d, 0), rt = type_arg(d, 1);
    if (!lt || !rt || d.ranges.size() < 3)
        return {};
    auto& left = lt->getCanonicalType();
    auto& right = rt->getCanonicalType();

    if (d.code == diag::SignCompare) {
        auto range = left.isSigned() ? d.ranges[1] : d.ranges[2];
        if (!editable(sm, d, range))
            return {};
        auto facts = facts_at(d, range, context);
        if (!facts || !facts->expr || !is_primary(*facts->expr))
            return {};
        return wrap(sm, range, "unsigned'(", ")", "Cast to unsigned");
    }

    // A width cast settles only two bit vectors of one sign; any other pair
    // stays mismatched after it.
    if (!left.isSimpleBitVector() || !right.isSimpleBitVector() ||
        left.isSigned() != right.isSigned() || left.getBitWidth() == right.getBitWidth())
        return {};
    const bool left_narrower = left.getBitWidth() < right.getBitWidth();
    auto range = left_narrower ? d.ranges[1] : d.ranges[2];
    if (!editable(sm, d, range))
        return {};
    const auto width = std::to_string(std::max(left.getBitWidth(), right.getBitWidth()));
    return wrap(sm, range, width + "'(", ")",
                "Cast " + title_text(sm, range) + " to " + width + " bits");
}

} // namespace

std::vector<FixInfo> slang_fixes(const SourceManager& sm, const Diagnostic& d,
                                 SlangFixContext& context) try {
    const auto code = d.code;
    if (is_conversion(code))
        return conversion_fixes(sm, d, context);
    if (code == diag::ArithOpMismatch || code == diag::BitwiseOpMismatch ||
        code == diag::ComparisonMismatch || code == diag::SignCompare)
        return operand_fixes(sm, d, context);
    if ((code == diag::IntBoolConv || code == diag::FloatBoolConv) && !d.ranges.empty() &&
        editable(sm, d, d.ranges[0])) {
        return code == diag::IntBoolConv
                   ? wrap(sm, d.ranges[0], "(", ") != '0", "Compare with '0")
                   : wrap(sm, d.ranges[0], "(", ") != 0.0", "Compare with 0.0");
    }
    return {};
} catch (...) {
    return {};
}
