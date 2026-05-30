# AutoFunc

**Code action**

Generates a function or task call with all arguments filled in from the subroutine's signature. Triggered as a code action when the cursor is on a function or task name.

```systemverilog
// function signature
function automatic logic [7:0] clamp(
    input logic [7:0] val,
    input logic [7:0] limit
);

// AutoFunc generates (with use_named_arguments = true):
clamp(.val(val), .limit(limit))

// AutoFunc generates (with use_named_arguments = false):
clamp(val, limit)
```

```toml
[autofunc]
use_named_arguments = true
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `use_named_arguments` | bool | `true` | Generate `.arg(value)` named style instead of positional |
