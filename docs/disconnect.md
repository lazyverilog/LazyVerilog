# Disconnect

`Disconnect` is currently exposed through the two-instance `:Interface <inst1>
<inst2>` floating window. Press `D` on that view to clear an existing shared
signal connection between the displayed instances.

## Command

The server-side command is:

- `lazyverilog.interfaceDisconnect`

Lua sends the current file URI, both instance names, both selected port names,
and the signal name from the selected Interface row.

## Behavior

Disconnect generates a workspace edit that:

- clears `.port(signal)` on the selected first-instance port if the current
  signal exactly matches the selected row signal
- clears `.port(signal)` on the selected second-instance port if the current
  signal exactly matches the selected row signal
- removes a standalone declaration for that signal when the declaration is a
  simple `wire`, `logic`, or `reg` declaration

Example:

```systemverilog
logic [5:0] data32;

memory u_mem2 (
    .o_data(data32)
);

memory u_mem3 (
    .i_data(data32)
);
```

After disconnect:

```systemverilog
memory u_mem2 (
    .o_data()
);

memory u_mem3 (
    .i_data()
);
```

## Limitations

Disconnect uses syntax-index instance/connection data to choose the ports, but
its edit construction is intentionally conservative and textual. It removes only
simple standalone declarations and will leave more complex declarations for the
user to clean up manually, for example declarations with multiple names,
assignments, or unsupported net/variable keywords.
