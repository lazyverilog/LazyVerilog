<!-- Parent: ../AGENTS.md -->
<!-- Generated: 2026-05-28 | Updated: 2026-05-28 -->

# docs

## Purpose
User-facing and agent-facing documentation for lazyverilog. Covers formatter configuration options and background compilation diagnostics.

## Key Files

| File | Description |
|------|-------------|
| `index.md` | Landing page of the site (hero, four buttons, feature cards) |
| `features.md` | Feature index |
| `.vitepress/site.json` | Sidebar and the list of directories excluded from the site |
| `scripts/check-dist.mjs` | Checks the built site; fixtures in `scripts/check-dist.test.mjs` |
| `AGENTS.md` | This file |

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `formatter/` | Formatter-specific docs (see `formatter/AGENTS.md`) |
| `diagnostics/` | Diagnostics and background compilation docs (see `diagnostics/AGENTS.md`) |
| `linter/` | Linter-specific docs (see `linter/AGENTS.md`) |

## For AI Agents

### Working In This Directory
- When adding a new `lazyverilog.toml` config option, update `formatter/options.md`
- Every user-facing page is published to https://lazyverilog.github.io: add it to the sidebar in `.vitepress/site.json` or the site build check fails
- User-facing docs live **outside** `dev/`, `releases/` and `i18n/`; those three directories are not published (`releases/` is read by `tools/release.sh`)
- `README.md` at the repository root is only a title and a link to the site; put no content there
- Preview locally: `npm --prefix docs ci && npm --prefix docs run dev` (see `dev/site.md`)

<!-- MANUAL: -->
