<!-- Parent: ../AGENTS.md -->

# scripts

## Purpose
Repository-maintenance scripts that call out to services. Unlike `tools/`, nothing
here benchmarks or debugs the server, and nothing here is part of the build.

## Subdirectories

| Directory | Purpose |
|-----------|---------|
| `translate-readme/` | Translates `README.md` into `docs/i18n/README.<lang>.md` with the Claude Agent SDK |

## For AI Agents

### translate-readme

```bash
cd scripts/translate-readme && npm install
node index.ts ko fr          # named languages
node index.ts --tier 1       # a tier (CI's default)
node index.ts --dry-run ko   # print the prepared source, call no model
```

- Driven in CI by `.github/workflows/translate-readme.yml`, on every push to `main`
  that touches `README.md`, and on `workflow_dispatch` for a chosen language set.
  Auth is the `CLAUDE_CODE_OAUTH_TOKEN` repository secret, which is what the Agent
  SDK reads; there is no `ANTHROPIC_API_KEY` in this workflow.
- **The English `README.md` is the only source.** Translations are generated output:
  edit the English file and let CI regenerate, never hand-patch a `docs/i18n/` file —
  the next source change overwrites it.
- Translations are cached in `docs/i18n/.translation-cache.json`, keyed on the SHA-256
  of `README.md` plus the model and `PROMPT_VERSION`. An unchanged README costs
  nothing to re-run, which is what makes the push trigger safe. Change the prompt and
  bump `PROMPT_VERSION`, or every cached language stays stale forever.
- **Relative links are rewritten before the model sees them**, not by it
  (`rewriteRelativeLinks()`). `README.md` links its siblings as `docs/features.md` and
  `assets/...`; from `docs/i18n/` those resolve two directories too high, so every
  link in every translation would 404. The model is only asked to preserve what it is
  handed — path arithmetic is not a translation decision. `--dry-run` prints exactly
  what gets sent.
- The run is deliberately toolless and isolated: `tools: []` and `settingSources: []`.
  Without the latter the SDK loads this repository's `CLAUDE.md`, settings, hooks and
  MCP servers into a job that only needs to rewrite prose.
- Adding a language is a row in `LANGUAGES`; add it to the README language bar too, or
  the file is generated and never linked. CI generates **tier 1** by default, which is
  the set that bar lists.
