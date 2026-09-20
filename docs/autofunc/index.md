# AutoFunc

**Code action.** Writes a call to a function or task with every argument filled in from its signature.
Put the cursor on the function or task name.

```systemverilog
function automatic logic [7:0] clamp(input logic [7:0] val, input logic [7:0] limit);

// use_named_arguments = true
clamp(.val(val), .limit(limit))

// use_named_arguments = false
clamp(val, limit)
```

```toml
[autofunc]
indent_size = 4
use_named_arguments = true
```

| Section | Option | Default | Description |
|---------|--------|---------|-------------|
| `autofunc` | `indent_size` | `4` | Indent for multiline argument lists. Does not follow `[format].indent_size` (default `2`), so set both if you want them to match |
| `autofunc` | `use_named_arguments` | `true` | Generate `.arg(value)` instead of positional arguments |
