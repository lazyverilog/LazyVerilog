#include "features/clock_domain.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Statement.h>
#include <slang/ast/TimingControl.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/SelectExpressions.h>
#include <slang/ast/statements/MiscStatements.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/MemberSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace clock_domain {

namespace {

using namespace slang::ast;

/// Safety valve only.  Termination is guaranteed by the visited set over
/// (frame, value) pairs; this just bounds pathological hierarchy expansion so a
/// runaway walk reports a limit instead of running forever.  Thirty-two module
/// boundaries on a single combinational path is already far past real RTL.
constexpr int kMaxHierDepth = 32;

/// Finds the event control that gates a procedural block.
///
/// `always_ff @(posedge clk) begin ... end` puts the TimedStatement at the top
/// of the body, but the timing may also sit inside a begin/end, so descend
/// through statement containers rather than only checking the root.
const TimedStatement* find_first_timed(const Statement& statement) {
    switch (statement.kind) {
    case StatementKind::Timed:
        return &statement.as<TimedStatement>();
    case StatementKind::Block:
        return find_first_timed(statement.as<BlockStatement>().body);
    case StatementKind::List:
        for (const auto* child : statement.as<StatementList>().list) {
            if (const auto* found = find_first_timed(*child))
                return found;
        }
        return nullptr;
    default:
        return nullptr;
    }
}

void collect_edges(const TimingControl& timing, std::vector<ClockEdge>& out) {
    switch (timing.kind) {
    case TimingControlKind::SignalEvent: {
        const auto& event = timing.as<SignalEventControl>();
        if (event.edge == EdgeKind::None)
            return; // Level-sensitive: combinational sensitivity, not a clock.

        ClockEdge clock;
        clock.edge = event.edge;
        if (const auto* symbol = event.expr.getSymbolReference()) {
            clock.name = std::string(symbol->name);
            clock.signal = symbol->as_if<ValueSymbol>();
        }
        if (clock.name.empty())
            clock.name = "<expr>";
        out.push_back(std::move(clock));
        return;
    }
    case TimingControlKind::EventList:
        for (const auto* event : timing.as<EventListControl>().events)
            collect_edges(*event, out);
        return;
    default:
        // ImplicitEventControl (`@*`) and delay controls carry no clock.
        return;
    }
}

/// Collects which values a piece of logic reads and which it writes.
///
/// Bit granularity is deliberately dropped: `q[3] <= d` records a write to all
/// of `q`.  slang's LSPVisitor exists to compute exact driven bit ranges, but
/// this tool unions a port's bits into a single reported domain anyway, so the
/// extra machinery (and its EvalContext) would buy nothing here.
struct RefCollector : ASTVisitor<RefCollector, true, true> {
    bool is_lvalue = false;
    std::vector<const ValueSymbol*> reads;
    std::vector<const ValueSymbol*> writes;

    void record(const ValueSymbol& symbol) {
        auto& target = is_lvalue ? writes : reads;
        if (std::find(target.begin(), target.end(), &symbol) == target.end())
            target.push_back(&symbol);
    }

    void handle(const NamedValueExpression& expr) { record(expr.symbol); }
    void handle(const HierarchicalValueExpression& expr) { record(expr.symbol); }

    // Note the `x.visit(*this)` form throughout: these members are typed as the
    // Expression/Statement base, and only the node's own visit() dispatches on
    // its runtime kind.  Calling the inherited visit(x) instead would resolve
    // against the static base type and trip ASTVisitor's non-leaf assertion.
    void handle(const AssignmentExpression& expr) {
        is_lvalue = true;
        expr.left().visit(*this);
        is_lvalue = false;
        if (!expr.isLValueArg())
            expr.right().visit(*this);
    }

    // In `q[i] <= d`, `q` is written but `i` is read.  Same for range selects:
    // the selected value inherits the lvalue flag, the selectors never do.
    void handle(const ElementSelectExpression& expr) {
        expr.value().visit(*this);
        const bool saved = std::exchange(is_lvalue, false);
        expr.selector().visit(*this);
        is_lvalue = saved;
    }

    void handle(const RangeSelectExpression& expr) {
        expr.value().visit(*this);
        const bool saved = std::exchange(is_lvalue, false);
        expr.left().visit(*this);
        expr.right().visit(*this);
        is_lvalue = saved;
    }

    void handle(const MemberAccessExpression& expr) { expr.value().visit(*this); }

    // The event control names the clock, not data flowing through the block.
    // classify_block() reads it separately; counting it as a read here would
    // make every clock look like a combinational input.
    void handle(const TimedStatement& statement) { statement.stmt.visit(*this); }
};

const ValueSymbol* internal_value(const Symbol* internal) {
    return internal ? internal->as_if<ValueSymbol>() : nullptr;
}

/// One port connection of a child instance, as seen from both sides.
struct ChildLink {
    const ValueSymbol* outer = nullptr; // net in the parent body
    const ValueSymbol* inner = nullptr; // value behind the child's port
    ArgumentDirection direction = ArgumentDirection::In;
};

/// One piece of logic in an instance body.
struct LogicNode {
    bool is_child_instance = false;
    BlockClocks clocks;
    std::vector<const ValueSymbol*> reads;
    std::vector<const ValueSymbol*> writes;
    const InstanceSymbol* child = nullptr;
    std::vector<ChildLink> links;
};

struct InstanceGraph {
    std::vector<LogicNode> nodes;
    std::unordered_map<const ValueSymbol*, std::vector<size_t>> writers;
    std::unordered_map<const ValueSymbol*, std::vector<size_t>> readers;
    /// Values that drive some block's event control, i.e. act as clocks.
    std::unordered_set<const ValueSymbol*> clock_signals;

    void add(LogicNode node) {
        const size_t index = nodes.size();
        for (const auto* symbol : node.reads)
            readers[symbol].push_back(index);
        for (const auto* symbol : node.writes)
            writers[symbol].push_back(index);
        for (const auto& clock : node.clocks.clocks) {
            if (clock.signal)
                clock_signals.insert(clock.signal);
        }
        nodes.push_back(std::move(node));
    }
};

/// Walks one instance body, gathering its procedural blocks, continuous
/// assignments, and child instance boundaries.  Child bodies are not descended
/// into here: each instance gets its own graph, built on demand, so that a
/// body shared by several instantiations is still traced per instance.
struct GraphBuilder : ASTVisitor<GraphBuilder, false, false> {
    InstanceGraph graph;

    void handle(const ProceduralBlockSymbol& block) {
        LogicNode node;
        node.clocks = classify_block(block);

        RefCollector refs;
        block.getBody().visit(refs);
        node.reads = std::move(refs.reads);
        node.writes = std::move(refs.writes);
        graph.add(std::move(node));
    }

    void handle(const ContinuousAssignSymbol& assign) {
        LogicNode node;

        RefCollector refs;
        assign.getAssignment().visit(refs);
        node.reads = std::move(refs.reads);
        node.writes = std::move(refs.writes);
        graph.add(std::move(node));
    }

    void handle(const InstanceSymbol& instance) {
        LogicNode node;
        node.is_child_instance = true;
        node.child = &instance;

        for (const auto* connection : instance.getPortConnections()) {
            if (!connection || connection->port.kind != SymbolKind::Port)
                continue;
            const auto* expression = connection->getExpression();
            if (!expression)
                continue;

            const auto& port = connection->port.as<PortSymbol>();
            const auto* inner = internal_value(port.internalSymbol);
            if (!inner)
                continue;

            // Collect the parent-side nets named by the connection expression.
            //
            // Both sides of the collector are merged on purpose: slang models
            // an output port connection as an assignment to the parent net, so
            // `.y(w)` lands `w` in `writes` while an input connection lands its
            // nets in `reads`.  Direction here comes from the port, which is
            // authoritative, so lvalue position carries no extra information.
            RefCollector refs;
            expression->visit(refs);

            std::vector<const ValueSymbol*> outers = std::move(refs.reads);
            for (const auto* written : refs.writes) {
                if (std::find(outers.begin(), outers.end(), written) == outers.end())
                    outers.push_back(written);
            }

            const bool drives_parent = port.direction == ArgumentDirection::Out ||
                                       port.direction == ArgumentDirection::InOut;
            for (const auto* outer : outers) {
                node.links.push_back(ChildLink{outer, inner, port.direction});
                if (drives_parent)
                    node.writes.push_back(outer);
                if (port.direction != ArgumentDirection::Out)
                    node.reads.push_back(outer);
            }
        }

        graph.add(std::move(node));
    }
};

/// A specific instance reached along a specific path.
///
/// The parent pointer matters because slang shares one InstanceBodySymbol
/// across every instantiation of a module: the body alone cannot say which
/// parent net a given port is tied to, so the path has to be carried.
struct Frame {
    const InstanceSymbol* instance = nullptr;
    const Frame* parent = nullptr;
    int depth = 0;
};

struct TraceResult {
    std::set<std::string> clocks;
    bool reached_latch = false;
    bool reached_state = false;
    bool hit_depth_limit = false;
    /// The traced value drives some procedural block's event control, at this
    /// level or in a child it is passed down to.
    bool used_as_clock = false;
};

class Tracer {
public:
    explicit Tracer(const InstanceSymbol& root) {
        frames_.push_back(Frame{&root, nullptr, 0});
        root_ = &frames_.front();
    }

    const Frame& root_frame() const { return *root_; }

    const InstanceGraph& graph_for(const InstanceSymbol& instance) {
        auto found = graphs_.find(&instance);
        if (found != graphs_.end())
            return found->second;

        GraphBuilder builder;
        instance.body.visit(builder);
        return graphs_.emplace(&instance, std::move(builder.graph)).first->second;
    }

    TraceResult run(const ValueSymbol* start, bool forward) {
        TraceResult result;
        if (!start)
            return result;

        std::set<std::pair<const Frame*, const ValueSymbol*>> visited;
        std::deque<std::pair<const Frame*, const ValueSymbol*>> pending;

        pending.emplace_back(root_, start);
        visited.emplace(root_, start);

        while (!pending.empty()) {
            const auto [frame, symbol] = pending.front();
            pending.pop_front();

            auto push = [&](const Frame& next_frame, const ValueSymbol* next) {
                if (next && visited.emplace(&next_frame, next).second)
                    pending.emplace_back(&next_frame, next);
            };

            step_within(*frame, symbol, forward, result, push);
            step_outward(*frame, symbol, forward, push);
        }

        return result;
    }

    /// Renders a clock as spelled in the analyzed instance.
    ///
    /// A flop inside a child clocked by that child's `clk` port is far more
    /// useful reported as the parent net actually driving it, so walk the port
    /// connection outward until the signal is no longer an input port.
    std::string clock_name(const Frame& frame, const ClockEdge& edge) {
        const Frame* current = &frame;
        const ValueSymbol* signal = edge.signal;

        while (signal && current->parent) {
            const auto* outer = map_outward(*current, signal);
            if (!outer)
                break;
            signal = outer;
            current = current->parent;
        }

        return signal ? std::string(signal->name) : edge.name;
    }

private:
    /// Follows logic inside `frame` that touches `symbol`.
    template<typename PushFn>
    void step_within(const Frame& frame, const ValueSymbol* symbol, bool forward,
                     TraceResult& result, PushFn&& push) {
        const auto& graph = graph_for(*frame.instance);

        // A clock port of the analyzed instance is usually not used in an event
        // control in that instance's own body -- it is handed down to children
        // and consumed there.  Checking at every frame the walk reaches is what
        // makes a top-level `clk_i` report as an edge source rather than as
        // combinational feedthrough.
        if (forward && graph.clock_signals.contains(symbol))
            result.used_as_clock = true;

        const auto& edges = forward ? graph.readers : graph.writers;
        const auto found = edges.find(symbol);
        if (found == edges.end())
            return;

        for (const size_t index : found->second) {
            const auto& node = graph.nodes[index];

            if (node.is_child_instance) {
                descend(frame, node, symbol, forward, result, push);
                continue;
            }

            if (node.clocks.kind == BlockKind::Sequential) {
                result.reached_state = true;
                for (const auto& clock : node.clocks.clocks)
                    result.clocks.insert(clock_name(frame, clock));
                continue; // State element: the trace ends here.
            }

            if (node.clocks.kind == BlockKind::Latch) {
                result.reached_state = true;
                result.reached_latch = true;
                continue;
            }

            for (const auto* next : forward ? node.writes : node.reads)
                push(frame, next);
        }
    }

    /// Crosses into a child instance along a port connection.
    template<typename PushFn>
    void descend(const Frame& frame, const LogicNode& node, const ValueSymbol* symbol, bool forward,
                 TraceResult& result, PushFn&& push) {
        for (const auto& link : node.links) {
            if (link.outer != symbol)
                continue;

            // Forward: the value feeds the child's input.  Backward: the
            // child's output drives the value.
            const bool usable = forward ? link.direction != ArgumentDirection::Out
                                        : link.direction != ArgumentDirection::In;
            if (!usable)
                continue;

            if (frame.depth + 1 > kMaxHierDepth) {
                result.hit_depth_limit = true;
                continue;
            }

            push(child_frame(frame, *node.child), link.inner);
        }
    }

    /// Leaves the current instance through one of its own ports, continuing in
    /// the parent.  The analyzed instance is the root frame and has no parent,
    /// which is what keeps the trace inside it.
    template<typename PushFn>
    void step_outward(const Frame& frame, const ValueSymbol* symbol, bool forward, PushFn&& push) {
        if (!frame.parent)
            return;

        const auto* link = find_link_inward(frame, symbol);
        if (!link)
            return;

        // Forward: a value reaching the child's output continues into the
        // parent.  Backward: a value reaching the child's input is driven from
        // the parent.
        const bool usable = forward ? link->direction != ArgumentDirection::In
                                    : link->direction != ArgumentDirection::Out;
        if (usable)
            push(*frame.parent, link->outer);
    }

    const Frame& child_frame(const Frame& parent, const InstanceSymbol& child) {
        const auto key = std::make_pair(&parent, &child);
        auto found = child_frames_.find(key);
        if (found != child_frames_.end())
            return *found->second;

        frames_.push_back(Frame{&child, &parent, parent.depth + 1});
        const Frame* created = &frames_.back();
        child_frames_.emplace(key, created);
        return *created;
    }

    /// Finds the connection of `frame`'s own instance whose inner value is
    /// `symbol`, i.e. the port through which the trace can leave the instance.
    const ChildLink* find_link_inward(const Frame& frame, const ValueSymbol* symbol) {
        const auto& parent_graph = graph_for(*frame.parent->instance);
        for (const auto& node : parent_graph.nodes) {
            if (!node.is_child_instance || node.child != frame.instance)
                continue;
            for (const auto& link : node.links) {
                if (link.inner == symbol)
                    return &link;
            }
        }
        return nullptr;
    }

    const ValueSymbol* map_outward(const Frame& frame, const ValueSymbol* symbol) {
        const auto* link = find_link_inward(frame, symbol);
        return link ? link->outer : nullptr;
    }

    std::unordered_map<const InstanceSymbol*, InstanceGraph> graphs_;
    std::deque<Frame> frames_;
    std::map<std::pair<const Frame*, const InstanceSymbol*>, const Frame*> child_frames_;
    const Frame* root_ = nullptr;
};

std::string_view direction_name(ArgumentDirection direction) {
    switch (direction) {
    case ArgumentDirection::In:
        return "in";
    case ArgumentDirection::Out:
        return "out";
    case ArgumentDirection::InOut:
        return "inout";
    case ArgumentDirection::Ref:
        return "ref";
    }
    return "?";
}

void fill_domain(PortDomain& domain, Tracer& tracer, const ValueSymbol* internal,
                 ArgumentDirection direction) {
    if (!internal) {
        domain.reached_nothing = true;
        return;
    }

    TraceResult result;
    // An inout port is both captured and driven, so it needs both walks.
    if (direction != ArgumentDirection::Out)
        result = tracer.run(internal, true);
    if (direction != ArgumentDirection::In) {
        const TraceResult backward = tracer.run(internal, false);
        result.clocks.insert(backward.clocks.begin(), backward.clocks.end());
        result.reached_latch |= backward.reached_latch;
        result.reached_state |= backward.reached_state;
        result.hit_depth_limit |= backward.hit_depth_limit;
    }

    domain.clocks.assign(result.clocks.begin(), result.clocks.end());
    domain.is_clock = result.used_as_clock;
    domain.reached_latch = result.reached_latch;
    domain.hit_depth_limit = result.hit_depth_limit;
    domain.reached_nothing = !result.reached_state && !result.used_as_clock;
}

} // namespace

BlockClocks classify_block(const slang::ast::ProceduralBlockSymbol& block) {
    BlockClocks result;

    if (block.procedureKind == ProceduralBlockKind::AlwaysLatch) {
        result.kind = BlockKind::Latch;
        return result;
    }

    const auto* timed = find_first_timed(block.getBody());
    if (!timed)
        return result;

    collect_edges(timed->timing, result.clocks);
    if (!result.clocks.empty())
        result.kind = BlockKind::Sequential;

    return result;
}

std::vector<PortDomain> analyze_instance(const slang::ast::InstanceSymbol& instance) {
    Tracer tracer(instance);

    std::vector<PortDomain> domains;
    for (const auto* port : instance.body.getPortList()) {
        if (!port)
            continue;

        PortDomain domain;
        domain.name = std::string(port->name);

        switch (port->kind) {
        case SymbolKind::Port: {
            const auto& typed = port->as<PortSymbol>();
            domain.direction = std::string(direction_name(typed.direction));
            fill_domain(domain, tracer, internal_value(typed.internalSymbol), typed.direction);
            break;
        }
        case SymbolKind::MultiPort: {
            const auto& typed = port->as<MultiPortSymbol>();
            domain.direction = std::string(direction_name(typed.direction));
            // A multi-port concatenates several internal values behind one
            // external name; union the domain of every constituent.
            for (const auto* part : typed.ports) {
                if (!part)
                    continue;
                PortDomain partial;
                fill_domain(partial, tracer, internal_value(part->internalSymbol),
                            typed.direction);
                domain.clocks.insert(domain.clocks.end(), partial.clocks.begin(),
                                     partial.clocks.end());
                domain.is_clock |= partial.is_clock;
                domain.reached_latch |= partial.reached_latch;
                domain.hit_depth_limit |= partial.hit_depth_limit;
                domain.reached_nothing |= partial.reached_nothing;
            }
            std::sort(domain.clocks.begin(), domain.clocks.end());
            domain.clocks.erase(std::unique(domain.clocks.begin(), domain.clocks.end()),
                                domain.clocks.end());
            break;
        }
        case SymbolKind::InterfacePort:
            domain.direction = "iface";
            break;
        default:
            continue;
        }

        domains.push_back(std::move(domain));
    }

    return domains;
}

} // namespace clock_domain
