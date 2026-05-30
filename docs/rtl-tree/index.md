# RTL Tree

**Commands:** `lazyverilog.rtlTree`, `lazyverilog.rtlTreeReverse`

Builds a JSON instantiation hierarchy tree for the current module.

- `rtlTree` — forward hierarchy: the module and all its instantiated children (recursively).
- `rtlTreeReverse` — reverse hierarchy: where the current module is instantiated.

```toml
[rtltree]
show_instance_name = true
show_file = false
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `show_instance_name` | bool | `true` | Show `module (u_instance)` instead of just `module` |
| `show_file` | bool | `false` | Append `[path/to/file.sv]` after the module name |
