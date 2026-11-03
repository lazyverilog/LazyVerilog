#include "features/fix_slang.hpp"

#include "lsp_position.hpp"

#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Scope.h>
#include <slang/ast/types/AllTypes.h>
#include <slang/diagnostics/ExpressionsDiags.h>

#include <any>
#include <optional>
#include <string>
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

std::string_view range_text(const SourceManager& sm, SourceRange range) {
    return sm.getSourceText(range.start().buffer())
        .substr(range.start().offset(), range.end().offset() - range.start().offset());
}

// Whether the text before @p range ends in `=`: the right side of an
// assignment or initializer, never an output argument or port.
bool after_equals(const SourceManager& sm, SourceRange range) {
    auto text = sm.getSourceText(range.start().buffer()).substr(0, range.start().offset());
    auto last = text.find_last_not_of(" \t\r\n");
    return last != std::string_view::npos && text[last] == '=';
}

// What the diagnostic's scope binds at the operand's range: whether the range
// is an output/inout argument or port (bound as `actual = <placeholder>`), and
// the implicit conversion applied to it.
struct OperandFinder : ASTVisitor<OperandFinder, VisitFlags::AllGood> {
    explicit OperandFinder(SourceRange r) : range(r) {}

    void handle(const AssignmentExpression& expr) {
        if (expr.isLValueArg() && expr.left().sourceRange == range)
            lvalue = true;
        visitDefault(expr);
    }

    void handle(const ConversionExpression& expr) {
        if (!conversion && expr.isImplicit() && expr.sourceRange == range)
            conversion = &expr;
        visitDefault(expr);
    }

    // Only the connections belong to this scope; the body is another one.
    void handle(const InstanceSymbol& symbol) {
        for (auto connection : symbol.getPortConnections())
            if (auto expr = connection->getExpression())
                expr->visit(*this);
    }

    SourceRange range;
    bool lvalue = false;
    const ConversionExpression* conversion = nullptr;
};

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
std::vector<FixInfo> conversion_fixes(const SourceManager& sm, const Diagnostic& d) {
    const auto code = d.code;
    if (d.ranges.empty() || !editable(sm, d, d.ranges[0]))
        return {};
    const auto range = d.ranges[0];

    // The scope walk tells an output argument or port (no fix) from an input
    // one; the right side of `=` is neither, so it skips the walk.  A widening
    // walks anyway: an expression whose effective sign is "either" (a
    // comparison, a 1-bit system call) is spared sign-conversion, and an N'()
    // cast gives it a sign.  Such expressions are 1 bit wide, so a truncation
    // never meets one.
    const bool expand = code == diag::WidthExpand || code == diag::PortWidthExpand;
    OperandFinder finder(range);
    if (expand || !after_equals(sm, range)) {
        if (!d.symbol)
            return {};
        d.symbol->visit(finder);
        if (finder.lvalue)
            return {};
    }

    if (code == diag::WidthTruncate || code == diag::WidthExpand ||
        code == diag::PortWidthTruncate || code == diag::PortWidthExpand) {
        auto width = width_arg(d, 1);
        if (!width)
            return {};
        std::string open = std::to_string(*width) + "'(", close = ")";
        if (expand) {
            if (!finder.conversion)
                return {};
            const auto& operand = finder.conversion->unwrapImplicitConversions();
            const bool target_signed = finder.conversion->type->isSigned();
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

// analyzeOpTypes(): args are the operand types, ranges 1 and 2 the operands.
std::vector<FixInfo> operand_fixes(const SourceManager& sm, const Diagnostic& d) {
    auto lt = type_arg(d, 0), rt = type_arg(d, 1);
    if (!lt || !rt || d.ranges.size() < 3)
        return {};
    auto& left = lt->getCanonicalType();
    auto& right = rt->getCanonicalType();

    if (d.code == diag::SignCompare) {
        auto range = left.isSigned() ? d.ranges[1] : d.ranges[2];
        if (!editable(sm, d, range))
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
                "Cast " + std::string(range_text(sm, range)) + " to " + width + " bits");
}

} // namespace

std::vector<FixInfo> slang_fixes(const SourceManager& sm, const Diagnostic& d) try {
    const auto code = d.code;
    if (is_conversion(code))
        return conversion_fixes(sm, d);
    if (code == diag::ArithOpMismatch || code == diag::BitwiseOpMismatch ||
        code == diag::ComparisonMismatch || code == diag::SignCompare)
        return operand_fixes(sm, d);
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
