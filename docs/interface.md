# Interface

A view of the signals shared between instances, where you can also connect and disconnect them.
Neovim: `:Interface <inst>` or `:Interface <inst1> <inst2>`. VS Code: `LazyVerilog: Interface`.

## One instance

Read-only. Lists each port, the signal it is connected to, and the sibling ports on the same signal.

## Two instances

A table with the ports of the first instance, the shared signal, and the ports of the second. Ports
with the same signal name share a row. A port with no partner gets its own row. Arrows show direction:
`→` output, `←` input, `↔` inout, `|` no match.

In Neovim's floating window, `C` connects, `D` disconnects, and `q` closes.

### Connect

Press `C`, then choose the row of the first port, the row of the second, and a wire name. The edit
sets `.port(signal)` on both instances and declares the wire if needed. The wire type follows the
output port, as in [Connect](connect.md#wire-type).

### Disconnect

Press `D` and choose a row. The edit clears `.port(signal)` on both instances when they still match
the row, and deletes the signal's declaration if it is a simple `wire`, `logic`, or `reg`.

```systemverilog
logic [5:0] data32;
memory u_mem2 (.o_data(data32));
memory u_mem3 (.i_data(data32));
```

becomes

```systemverilog
memory u_mem2 (.o_data());
memory u_mem3 (.i_data());
```

A declaration with several names, an assignment, or another keyword is left for you to remove.

## Limits

Interface works on syntax, not elaboration. It expects ordinary instances with named connections, and
does not handle SystemVerilog `interface` or `modport`, complex `generate` hierarchy, macro-generated
instances, or positional connections.
