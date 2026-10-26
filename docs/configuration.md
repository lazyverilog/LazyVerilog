# Configuration

Everything is set in `lazyverilog.toml`. The server uses the nearest one above each file you open, so
two projects open side by side each use their own.

## Without a lazyverilog.toml

A file with no `lazyverilog.toml` above it still gets cross-file features. The server indexes the
folder the file belongs to as an *implicit project*:

- your editor's workspace folders, when it sends them: the folder VS Code opened, or the git
  checkout of each file the Neovim plugin opens (never the directory nvim was started in), or
- the nearest version-control checkout above the file (a `.git`, `.hg` or `.svn` directory).

Every `.sv` and `.v` file under that folder is indexed, and every directory that holds a `.svh`,
`.vh` or `.svi` header is an include directory. Directories that hold tool output are skipped:
dot-directories, names starting with `build`, `out`, `work`, `sim_build`, `obj_dir`, `xcelium.d`,
`INCA_libs`, `csrc`, `node_modules`, and names ending in `.runs`, `.cache`, `.sim` or `.gen`.
Directory names and `*.ext` patterns in `.gitignore` are skipped too. A subtree with its own
`lazyverilog.toml` belongs to that project instead.

An implicit project is a guess, so it does less than a filelist:

- No defines. `` `ifdef `` blocks that need one stay inactive.
- No whole-design compilation, so no elaboration diagnostics.
- Two modules with the same name are both indexed. Go to definition ranks the nearer one first.
  Rename refuses the name until a `lazyverilog.toml` says which files belong together.
- Nothing is written into your tree. Index shards go to the user cache:
  `%LOCALAPPDATA%\lazyverilog\i` on Windows, `~/Library/Caches/lazyverilog/i` on macOS, and
  `$XDG_CACHE_HOME/lazyverilog/i` (default `~/.cache/lazyverilog/i`) elsewhere.

The home directory, the folders above it, and a drive or filesystem root are never indexed this way.
A folder is not indexed at all when it holds more than 5000 source files or 300000 entries, or takes
longer than 5 seconds to list. The server then shows one message and works on open files only. Add a
`lazyverilog.toml`, or exclude directories (below).

When you save a new `lazyverilog.toml` in the folder, the folder becomes an ordinary project. The
server drops the implicit project and its cache.

### Turning it off or narrowing it

The client sends these in `initializationOptions`:

```json
{ "lazyverilog": { "implicitProject": "auto", "implicitProjectExclude": ["vendor/**", "sim"] } }
```

| Key | Value | Effect |
|-----|-------|--------|
| `implicitProject` | `"auto"` (default) | Workspace folders, else the version-control checkout |
| | `"workspace"` | Workspace folders only |
| | `"off"` | No implicit projects: a file with no `lazyverilog.toml` is handled on its own |
| `implicitProjectExclude` | string[] | Globs relative to the folder. A matching directory or file is skipped |

The server also takes `--implicit-project=off|workspace|auto` on its command line, for a site-wide
wrapper. `initializationOptions` override the flag. In Neovim, pass the flag with
`cmd_args = { "--implicit-project=off" }` in `setup()`.

After you add or remove many files, run the `lazyverilog.rescan` command to list the folder again.

## Minimal example

```toml
[design]
vcode = "demo/vcode.f"
define = ["RTL_SIM"]

[format]
enable_format_on_save = true
indent_size = 4

[lint.statement]
enable = true
case_missing_default = true
```

Lint rules are all off until you turn them on: `[lint].enable` defaults to `true` and only switches
everything off, while each category (`[lint.statement]`, `[lint.naming]`, ...) needs its own
`enable = true` plus the rules you want. See [Linter options](linter/options.md).

## Complete example

The `lazyverilog.toml` from the repository root, imported here so it cannot drift from the file the
project tests against.

<<< ../lazyverilog.toml

## Index cache

The index of each project file is cached on disk, so a restart does not parse the project again:

- In a project with a `lazyverilog.toml`: `.cache/lazyverilog/index` next to that file. Add `.cache/`
  to your `.gitignore`.
- Files under no project: `lazyverilog/index` in the user cache directory (listed above).
- Implicit projects: `lazyverilog/i` in the user cache directory.

An entry is reused only when the file, its includes, its defines and include directories are
unchanged, compared by content, not by timestamp. You can delete the directory at any time; the next
start indexes the files again.

## CPU use

Indexing uses up to 8 worker threads, never more than the CPUs this process may use: its CPU affinity,
a container CPU quota (cgroup), and the batch scheduler's allocation. The scheduler variables read are
`SLURM_CPUS_PER_TASK` (Slurm), `LSB_DJOB_NUMPROC` (LSF), `NCPUS` (PBS Pro) and `PBS_NUM_PPN`
(Torque); the smallest one set wins. `OMP_NUM_THREADS` is not read.

## Option reference

| Topic | Page |
|-------|------|
| Filelist and defines | [Design & filelist](design/index.md) |
| Formatter | [Options](formatter/options.md), [macro policy](formatter/macros.md) |
| Linter | [Linter options](linter/options.md) |
| Semantic diagnostics | [Background compilation](diagnostics/background-compilation.md) |
| Folding and inlay hints | [LSP features](lsp/index.md) |
| RTL tree | [RTL tree](rtl-tree/index.md) |
| Code generation | [AutoInst](autoinst/index.md), [AutoWire](autowire/index.md), [AutoArg](autoarg/index.md), [AutoFunc](autofunc/index.md), [AutoFF](autoff/index.md) |
