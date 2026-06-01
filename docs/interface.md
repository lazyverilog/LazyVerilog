# Interface View

`Interface` is an interactive Neovim view for inspecting and editing signal
interfaces between instances in the current design.

## Commands

- `:Interface <inst1> <inst2>` — show a two-instance interface table.
- `:Interface <inst>` — show a single-instance table with sibling connections.

The server-side commands are:

- `lazyverilog.interface`
- `lazyverilog.singleInterface`
- `lazyverilog.interfaceConnect`
- `lazyverilog.interfaceDisconnect`

## Two-instance view

The two-instance view displays three logical columns:

1. ports from the first instance
2. shared signal name/type
3. ports from the second instance

Rows are built from named port connections. If two ports use the same signal
name, Interface displays them on the same row. Ports that have no matching signal
on the other instance are displayed as unconnected rows.

Direction arrows are UI hints:

- `→` output direction
- `←` input direction
- `↔` inout direction
- `|` unknown / no matching side

From the floating window:

- `C` starts the connect flow
- `D` starts the disconnect flow
- `q` closes the view

## Connect flow from Interface

Pressing `C` asks for:

1. the row containing the first-instance port
2. the row containing the second-instance port
3. the bridge signal name

The Lua client sends `lazyverilog.interfaceConnect` with the selected instance
names, port names, requested signal name, and a UI fallback type. The C++ server
then re-resolves both ports from the syntax index and chooses the generated
declaration type from the selected **output** port.

Generated bridge declarations use `logic` for net-style output ports:

```systemverilog
output wire [5:0] o_data
// generated bridge signal:
logic [5:0] data32;
```

User-defined datatypes and symbolic dimensions are preserved:

```systemverilog
output payload_t [`BUS_W-1:0] payload
// generated bridge signal:
payload_t [`BUS_W-1:0] payload_w;
```

The returned workspace edit:

- replaces or adds `.port(signal)` on both selected instances
- adds the bridge declaration if it is not already present

After applying the edit, the Lua client refreshes the Interface view.

## Disconnect flow from Interface

Pressing `D` asks for one row number. The Lua client sends
`lazyverilog.interfaceDisconnect` with the row's two ports and signal name.

The server returns a workspace edit that:

- clears the selected first-instance connection when it exactly matches the row
  signal
- clears the selected second-instance connection when it exactly matches the row
  signal
- removes a standalone `wire`, `logic`, or `reg` declaration for that signal when
  found

Example:

```systemverilog
logic [5:0] data32;

memory u_mem2 (.o_data(data32));
memory u_mem3 (.i_data(data32));
```

Disconnect produces:

```systemverilog
memory u_mem2 (.o_data());
memory u_mem3 (.i_data());
```

See also: [Disconnect](disconnect.md).

## Single-instance view

The single-instance view lists each port, its connected signal, and sibling
instance ports that share the same signal. This is a read-only inspection view.

## Implementation notes and limitations

Interface uses slang-backed syntax-index information for modules, ports,
directions, datatypes, instances, and named port connections. It is not a full
semantic elaborator.

Text edits are still constructed as targeted source edits. Interface expects
ordinary module instances with named port connections. It does not elaborate
SystemVerilog `interface` constructs or modports, and it does not fully model
complex generate hierarchy, macro-generated instances/connections, or positional
connections.
