# RTL Tree

**Commands:** `lazyverilog.rtlTree`, `lazyverilog.rtlTreeReverse`

Builds a JSON instantiation hierarchy tree for the current module.

- `rtlTree` — forward hierarchy: the module and all its instantiated children (recursively).
- `rtlTreeReverse` — reverse hierarchy: where the current module is instantiated.

![Neovim's RtlTree window listing soc_top, its instances and their children beside the source file](/screenshots/rtl-tree-neovim.webp)

```toml
[rtltree]
show_instance_name = true
show_file = false
```

| Section | Option | Type | Default | Description |
|---------|--------|------|---------|-------------|
| `rtltree` | `show_instance_name` | bool | `true` | Show `module (u_instance)` instead of just `module` |
| `rtltree` | `show_file` | bool | `false` | Append `[path/to/file.sv]` after the module name |
