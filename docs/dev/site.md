# The documentation site

`https://lazyverilog.github.io` is built from this `docs/` directory with
[VitePress](https://vitepress.dev) and the official
[Catppuccin theme](https://github.com/catppuccin/vitepress). The build output is force-pushed by
`.github/workflows/site.yml` to `lazyverilog/lazyverilog.github.io`, which GitHub Pages serves from
its `main` branch.

## Layout

| Path | Role |
|------|------|
| `docs/index.md` | Landing page: hero, the four buttons, feature cards. Frontmatter only. |
| `docs/.vitepress/config.mts` | Site config: nav, per-page canonical/`og:` tags, theme, search. |
| `docs/.vitepress/site.json` | Sidebar and `srcExclude`; read by `config.mts` **and** `check-dist`. |
| `docs/.vitepress/theme/` | Catppuccin CSS import plus the single override in `custom.css`. |
| `docs/public/` | Static files: `logo.webp`, `favicon.png`, `og.png` (derived, committed). |
| `docs/scripts/` | `copy-assets.mjs`, `finish-dist.mjs`, `check-dist.mjs` and its tests, `make-derived.py`. |

Not published: `docs/dev/`, `docs/releases/` (read by `tools/release.sh`), `docs/i18n/`, every
`AGENTS.md`. Put user-facing pages **outside** those directories.

## Preview and build

```bash
npm --prefix docs ci
npm --prefix docs run dev        # live preview
npm --prefix docs run build      # writes docs/.vitepress/dist
node --test "docs/scripts/*.test.mjs"
node docs/scripts/check-dist.mjs
npm --prefix docs run preview    # serve the build; needed for extensionless URLs
```

`prebuild`/`predev` copy `assets/videos/` into `docs/public/videos/` (gitignored). `postbuild`
writes `.nojekyll`.

The build fails on a dead internal link and on Vue template syntax in prose (`{{`, an unbalanced
`<tag>` outside a code span). Fix the page; do not set `ignoreDeadLinks`.

## Adding or moving a page

1. Write the Markdown under `docs/` (not in an excluded directory).
2. Add it to the sidebar in `docs/.vitepress/site.json`.
3. Build. `check-dist` compares the source pages, the built pages and the sidebar, and fails when
   they disagree.

## What `check-dist` checks

Only this repository's own choices; VitePress already guarantees title, `404.html`, sitemap and link
resolution.

| Id | Assertion |
|----|-----------|
| A1 | Hero logo has alt text |
| A2 | At least 9 feature cards |
| A3 | Exactly four hero buttons: `/installation`, `/usage`, `/configuration`, the sponsor page |
| A4 | Each internal button target is a built page |
| A5 | `canonical`/`og:url` are `https://lazyverilog.github.io/…` on the landing page and a deep page; `og:image` exists |
| A6 | `.nojekyll` exists |
| A7 | Source pages = built pages = sidebar entries; only three `srcExclude` pattern shapes allowed |
| A8 | Every local `<img>` in every page exists |
| A9 | The hero name is not accent-colored (see below) |

`docs/scripts/check-dist.test.mjs` applies one mutation per assertion and requires that assertion to
fail. Defaults resolve from the script's location; an explicit `--dist`/`--docs` is relative to the
current directory.

**A9 and colors.** The Catppuccin theme colors the hero name with the accent. The project rule is no
accent color as large text, so `theme/custom.css` sets exactly one variable,
`--vp-home-hero-name-color`, to the theme's text color. Do not add other color overrides.

## Regenerating the derived images

`docs/public/logo.webp`, `og.png` and `favicon.png` are committed copies made from
`assets/lazyverilog_logo.png` and `vscode/assets/lazyverilog_icon.png`, which stay the source of
truth.

```bash
python -m venv .venv && .venv/bin/pip install pillow==12.3.0
.venv/bin/python docs/scripts/make-derived.py
```

Landing-page weight, measured once at first build (recorded for reference, not gated): 18.8 KB
HTML, 120 KB CSS, about 190 KB scripts and font, 61 KB hero image, 15 KB icon; about 450 KB raw and
about 230 KB transferred. The demo GIFs load only on the Demos page.

## One-time setup of the publishing target

Done once by an organization admin, in this order:

```bash
# 1. The site repo needs a branch before Pages can point at it.
gh repo create lazyverilog/lazyverilog.github.io --public --add-readme   # already done

# 2. Pages: deploy from the main branch, root.
gh api -X POST repos/lazyverilog/lazyverilog.github.io/pages \
  -f 'source[branch]=main' -f 'source[path]=/'

# 3. A key that can write only to the site repo.
ssh-keygen -t ed25519 -N '' -C 'lazyverilog-site-deploy' -f site_deploy_key
gh repo deploy-key add site_deploy_key.pub \
  --repo lazyverilog/lazyverilog.github.io --allow-write --title 'site publish'

# 4. The private half lives in an environment that only `main` may use.
gh api -X PUT repos/lazyverilog/LazyVerilog/environments/site-publish \
  --input - <<'JSON'
{"deployment_branch_policy": {"protected_branches": false, "custom_branch_policies": true}}
JSON
gh api -X POST repos/lazyverilog/LazyVerilog/environments/site-publish/deployment-branch-policies \
  -f name=main
gh secret set SITE_DEPLOY_KEY --repo lazyverilog/LazyVerilog --env site-publish < site_deploy_key
shred -u site_deploy_key site_deploy_key.pub 2>/dev/null || rm -f site_deploy_key site_deploy_key.pub
```

A push made with a deploy key triggers the Pages build; a push with `GITHUB_TOKEN` would not, which
is why the workflow uses a key.

**Rotating the key:** delete the deploy key in the site repo, run step 3 and the `gh secret set`
line again.

## Publishing by hand

To check what Pages does before the workflow has ever run on `main`, publish a local build:

```bash
npm --prefix docs run build && node docs/scripts/check-dist.mjs
cd docs/.vitepress/dist
git init -q -b main && git remote add origin git@github.com:lazyverilog/lazyverilog.github.io.git
git add --all && git commit -q -m 'Manual publish'
git push --force origin main
```

Then check on the real host: `/` returns 200, `/installation` (no `.html`) returns 200, a missing
path shows the site's `404.html`, assets load. A manual publish proves Pages and the key; it does
not exercise the workflow's runner steps (the `site-publish` environment admits only `main`).

## Rollback

Revert the docs commit on `main`; `site.yml` rebuilds and republishes. Or re-run the `publish` job
of an earlier green run (limited by the 30-day artifact retention and GitHub's job re-run window).
The site repo keeps no history of build output, by design.

## Required checks

`ci.yml` uses `paths-ignore` for docs and Markdown. That is safe only while no CI status check is a
*required* check on `main`: a workflow skipped by path filters leaves a required check pending
forever, whereas a job skipped by `if:` reports as skipped and passes. If a check becomes required,
replace `paths-ignore` with a first `changes` job (checkout with `fetch-depth: 0`, `git diff
--name-only` of the base against `HEAD`, fail open when the base cannot be resolved) and gate the
matrix on it. `site.yml` has the same property for `site-build`.
