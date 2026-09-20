# AutoFunc

**Code action.** Fills in the missing arguments of a function or task call from its signature. Put the
cursor on the function or task name of a call that has fewer arguments than the signature. A complete
call gets no action.

```systemverilog
task add_number(input int a, input int b, output int result);
    result = a + b;
endtask

module top;
    int x, y, sum;

    initial begin
        add_number(x, y);
    end
endmodule
```

With the cursor on `add_number`, the call becomes:

```systemverilog
        add_number(.a(x), .b(y), .result(result));
```

Arguments you already wrote are kept, and each missing one is filled with the parameter's own name.
An empty call such as `add_number(` becomes `add_number(.a(a), .b(b), .result(result));`. With
`use_named_arguments = false` the same call becomes `add_number(x, y, result);`.

The result is laid out by the [formatter](../formatter/options.md#format-function-call), so a long call wraps as your
function-call settings say.

```toml
[autofunc]
indent_size = 4
use_named_arguments = true
```

| Section | Option | Default | Description |
|---------|--------|---------|-------------|
| `autofunc` | `indent_size` | `4` | Indent for multiline argument lists. Does not follow `[format].indent_size` (default `2`), so set both if you want them to match |
| `autofunc` | `use_named_arguments` | `true` | Generate `.arg(value)` instead of positional arguments |
