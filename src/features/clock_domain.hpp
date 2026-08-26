#pragma once

#include <slang/ast/SemanticFacts.h>
#include <string>
#include <vector>

namespace slang::ast {
class InstanceSymbol;
class ProceduralBlockSymbol;
class ValueSymbol;
} // namespace slang::ast

namespace clock_domain {

/// One edge-triggered signal from a procedural block's event control.
struct ClockEdge {
    /// The resolved signal, or null when the event expression is not a simple
    /// reference to a value (`@(posedge a & b)`, for instance).
    const slang::ast::ValueSymbol* signal = nullptr;
    std::string name;
    slang::ast::EdgeKind edge = slang::ast::EdgeKind::None;
};

/// How a procedural block behaves for the purposes of a clock-domain trace.
enum class BlockKind {
    /// No edge events in the event control.  This is combinational logic, so a
    /// trace passes through it and keeps going.
    Combinational,
    /// At least one edge event.  This is a state endpoint: a trace stops here
    /// and reports the block's clocks.
    Sequential,
    /// `always_latch`.  A state endpoint with no event control to read, so
    /// there is no clock name to report.
    Latch,
};

struct BlockClocks {
    BlockKind kind = BlockKind::Combinational;
    /// Every edge-triggered signal in the event control, in source order.
    /// Non-empty only when `kind == BlockKind::Sequential`.
    std::vector<ClockEdge> clocks;
};

/// Classifies `block` and, when it is sequential, returns every edge-triggered
/// signal in its event control.
///
/// A block counts as sequential purely on the presence of an edge
/// (`EdgeKind::PosEdge` / `NegEdge` / `BothEdges`).  Level-sensitive entries
/// such as `always @(a or b)` are combinational sensitivity, not clocks, so
/// they are not reported and do not stop a trace.
///
/// Clocks and asynchronous resets are deliberately not separated:
/// `always_ff @(posedge clk or negedge rst_n)` reports both `clk` and `rst_n`.
/// Nothing in the AST distinguishes them, and every rule that tries is a
/// heuristic that silently mislabels some real designs.  Over-reporting is
/// deterministic and explainable; guessing is not.
BlockClocks classify_block(const slang::ast::ProceduralBlockSymbol& block);

/// What a trace found for one port of the analyzed instance.
struct PortDomain {
    std::string name;
    /// "in", "out", "inout", "ref", or "iface".
    std::string direction;
    /// Clock names reached by the trace: sorted and de-duplicated, so a port
    /// captured by two clocks lists both.
    std::vector<std::string> clocks;
    /// The port itself drives a procedural block's event control.
    bool is_clock = false;
    /// The trace reached an `always_latch`, which holds state but exposes no
    /// clock name.
    bool reached_latch = false;
    /// The trace ran out of logic without reaching any state element -- the
    /// port is combinational feedthrough, or unconnected.
    bool reached_nothing = false;
    /// The trace stopped at the hierarchy depth limit, so `clocks` may be
    /// incomplete.
    bool hit_depth_limit = false;
};

/// Reports the clock domain of every port of `instance`.
///
/// Input ports are traced forward through combinational logic to the first
/// state element that captures them; output ports are traced backward through
/// combinational logic to the state elements that drive them.
///
/// Combinational paths that cross into a child instance are followed into that
/// child's body, and back out again, until a state element is reached.  Clock
/// names are reported as spelled in `instance`: a flop inside a child clocked
/// by the child's own `clk` port is reported under whatever net `instance`
/// connects to it.
///
/// Termination comes from a visited set over (instance, value) pairs, not from
/// the depth limit -- the elaborated design is finite, so the limit only ever
/// trips on pathological hierarchies.
std::vector<PortDomain> analyze_instance(const slang::ast::InstanceSymbol& instance);

} // namespace clock_domain
