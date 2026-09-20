// Checks the built site against the choices this repo made.  It does not re-test what
// VitePress already guarantees (dead links fail the build itself).
//
//   node docs/scripts/check-dist.mjs [--dist <dir>] [--docs <dir>] [--only A5,A6]
//
// Defaults resolve from this script's location (docs/), never from the current directory.
// An explicitly passed --dist / --docs is resolved against the current directory.
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import path from 'node:path'
import { pathToFileURL } from 'node:url'

const ORIGIN = 'https://lazyverilog.github.io'
const SPONSOR = 'https://github.com/sponsors/kjoonha'
const ACTIONS = ['/installation', '/usage', '/configuration', SPONSOR]
const MIN_FEATURES = 9
const DEEP_PAGE = 'installation'

const SKIP_SOURCE_DIRS = new Set(['node_modules', '.vitepress', 'public'])

function read(file) {
  return readFileSync(file, 'utf8')
}

function walk(dir, skip = new Set()) {
  const out = []
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (skip.has(entry.name)) continue
    const full = path.join(dir, entry.name)
    if (entry.isDirectory()) out.push(...walk(full, skip))
    else out.push(full)
  }
  return out
}

function tags(html, name) {
  return html.match(new RegExp(`<${name}\\b[^>]*>`, 'g')) ?? []
}

function attrs(tag) {
  const out = {}
  for (const m of tag.matchAll(/\s([:\w.-]+)(?:="([^"]*)")?/g)) out[m[1]] = m[2] ?? ''
  return out
}

// `x.md`/`x.html` -> `x`, `a/index.md` -> `a/`, `index.md` -> ``.
function pageId(rel) {
  return rel.replace(/\\/g, '/').replace(/(^|\/)index\.(md|html)$/, '$1').replace(/\.(md|html)$/, '')
}

function excluder(patterns, fail) {
  const tests = []
  for (const p of patterns) {
    let m
    if ((m = /^([\w.-]+)\/\*\*$/.exec(p))) tests.push((rel) => rel.startsWith(`${m[1]}/`))
    else if ((m = /^\*\*\/([\w.-]+)$/.exec(p))) tests.push((rel) => rel === m[1] || rel.endsWith(`/${m[1]}`))
    else if ((m = /^([\w.-]+)$/.exec(p))) tests.push((rel) => rel === m[1])
    else fail(`A7: unsupported srcExclude pattern shape "${p}" (use <dir>/**, **/<file>, or <file>)`)
  }
  return (rel) => tests.some((t) => t(rel))
}

function sidebarLinks(items, out = []) {
  for (const item of items) {
    if (item.link && !/^https?:/.test(item.link)) out.push(item.link.replace(/^\//, '').replace(/#.*$/, ''))
    if (item.items) sidebarLinks(item.items, out)
  }
  return out
}

function distHas(dist, urlPath) {
  const rel = urlPath.replace(/^\//, '').replace(/[?#].*$/, '')
  return [rel, `${rel}.html`, path.join(rel, 'index.html')].some((c) => existsSync(path.join(dist, c)) && statSync(path.join(dist, c)).isFile())
}

export function runChecks({ dist, docs, only }) {
  const failures = []
  const want = (id) => !only || only.includes(id)
  const fail = (msg) => failures.push(msg)

  const indexFile = path.join(dist, 'index.html')
  const index = existsSync(indexFile) ? read(indexFile) : ''
  if (!index && want('A1')) fail(`A1: ${indexFile} is missing`)

  // A1: hero logo has alt text.
  if (want('A1') && index) {
    const hero = tags(index, 'img').map(attrs).find((a) => (a.class ?? '').split(/\s+/).includes('image-src'))
    if (!hero) fail('A1: hero logo <img class="image-src"> not found in index.html')
    else if (!hero.alt?.trim()) fail('A1: hero logo has empty alt text')
  }

  // A2: feature cards.
  if (want('A2')) {
    const cards = (index.match(/<article class="box"/g) ?? []).length
    if (cards < MIN_FEATURES) fail(`A2: ${cards} feature cards on the landing page, need at least ${MIN_FEATURES}`)
  }

  // A3 / A4: the four buttons.
  const buttons = tags(index, 'a').map(attrs).filter((a) => (a.class ?? '').split(/\s+/).includes('VPButton')).map((a) => a.href)
  if (want('A3')) {
    const same = buttons.length === ACTIONS.length && ACTIONS.every((h) => buttons.includes(h))
    if (!same) fail(`A3: hero actions are [${buttons.join(', ')}], expected exactly [${ACTIONS.join(', ')}]`)
  }
  if (want('A4')) {
    for (const href of buttons.filter((h) => h?.startsWith('/'))) {
      if (!distHas(dist, href)) fail(`A4: hero action ${href} has no built page`)
    }
  }

  // A5: canonical and social URLs, on the landing page and one deep page.
  if (want('A5')) {
    const cases = [
      ['index.html', `${ORIGIN}/`],
      [`${DEEP_PAGE}.html`, `${ORIGIN}/${DEEP_PAGE}`],
    ]
    for (const [file, expected] of cases) {
      const full = path.join(dist, file)
      if (!existsSync(full)) {
        fail(`A5: ${file} is missing`)
        continue
      }
      const html = read(full)
      const canonical = tags(html, 'link').map(attrs).find((a) => a.rel === 'canonical')?.href
      const ogUrl = tags(html, 'meta').map(attrs).find((a) => a.property === 'og:url')?.content
      if (canonical !== expected) fail(`A5: ${file} canonical is ${canonical}, expected ${expected}`)
      if (ogUrl !== expected) fail(`A5: ${file} og:url is ${ogUrl}, expected ${expected}`)
    }
    const ogImage = tags(index, 'meta').map(attrs).find((a) => a.property === 'og:image')?.content
    if (!ogImage?.startsWith(`${ORIGIN}/`)) fail(`A5: og:image is ${ogImage}, expected a URL under ${ORIGIN}/`)
    else if (!distHas(dist, ogImage.slice(ORIGIN.length))) fail(`A5: og:image file ${ogImage} does not exist in dist`)
  }

  // A6: Pages-from-branch would run Jekyll without this.
  if (want('A6') && !existsSync(path.join(dist, '.nojekyll'))) fail('A6: .nojekyll is missing from the tree')

  // A7: source <-> output <-> sidebar parity.
  if (want('A7')) {
    const siteFile = path.join(docs, '.vitepress', 'site.json')
    const site = JSON.parse(read(siteFile))
    const excluded = excluder(site.srcExclude ?? [], fail)
    const sources = new Set(
      walk(docs, SKIP_SOURCE_DIRS)
        .map((f) => path.relative(docs, f).replace(/\\/g, '/'))
        .filter((rel) => rel.endsWith('.md') && !excluded(rel))
        .map(pageId),
    )
    const built = new Set(
      walk(dist)
        .map((f) => path.relative(dist, f).replace(/\\/g, '/'))
        .filter((rel) => rel.endsWith('.html') && rel !== '404.html')
        .map(pageId),
    )
    for (const id of sources) if (!built.has(id)) fail(`A7: source page "${id}" has no built page`)
    for (const id of built) if (!sources.has(id)) fail(`A7: built page "${id}" has no source (or its source is excluded)`)
    const links = new Set(sidebarLinks(site.sidebar ?? []))
    for (const id of built) if (id !== '' && !links.has(id)) fail(`A7: built page "${id}" is not in the sidebar (docs/.vitepress/site.json)`)
    for (const link of links) if (!built.has(link)) fail(`A7: sidebar link "/${link}" has no built page`)
  }

  // A8: every local <img> in every built page exists.
  if (want('A8')) {
    for (const file of walk(dist).filter((f) => f.endsWith('.html'))) {
      const rel = path.relative(dist, file)
      for (const a of tags(read(file), 'img').map(attrs)) {
        const src = a.src
        if (!src || /^(https?:|data:|\/\/)/.test(src)) continue
        const target = src.startsWith('/') ? src : `/${path.posix.join(path.posix.dirname(rel.replace(/\\/g, '/')), src)}`
        if (!distHas(dist, target)) fail(`A8: ${rel} references missing image ${src}`)
      }
    }
  }

  // A9: decision 3.  The hero name must not render in the accent color.
  if (want('A9')) {
    const themeDir = path.join(docs, '.vitepress', 'theme')
    const css = read(path.join(themeDir, 'custom.css')).replace(/\/\*[\s\S]*?\*\//g, '').replace(/\s+/g, ' ').trim()
    const expected = ':root, .dark { --vp-home-hero-name-color: var(--vp-c-text-1) !important; }'
    if (css !== expected) fail(`A9: theme/custom.css must be exactly \`${expected}\`, found \`${css}\``)
    const ts = read(path.join(themeDir, 'index.ts'))
    const catppuccin = ts.indexOf('@catppuccin/vitepress/theme/')
    const custom = ts.indexOf('./custom.css')
    if (catppuccin < 0 || custom < 0 || custom < catppuccin) fail('A9: theme/index.ts must import custom.css after the Catppuccin CSS')
  }

  return failures
}

function parseArgs(argv) {
  const opts = {}
  for (let i = 0; i < argv.length; i++) {
    const flag = argv[i]
    if (flag === '--dist' || flag === '--docs' || flag === '--only') opts[flag.slice(2)] = argv[++i]
    else throw new Error(`unknown argument ${flag}`)
  }
  return opts
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
  const docsDefault = path.resolve(import.meta.dirname, '..')
  const opts = parseArgs(process.argv.slice(2))
  const dist = opts.dist ? path.resolve(opts.dist) : path.join(docsDefault, '.vitepress', 'dist')
  const docs = opts.docs ? path.resolve(opts.docs) : docsDefault
  const only = opts.only ? opts.only.split(',').map((s) => s.trim()) : undefined
  const failures = runChecks({ dist, docs, only })
  for (const f of failures) console.error(f)
  console.log(failures.length ? `check-dist: ${failures.length} failure(s)` : 'check-dist: ok')
  process.exit(failures.length ? 1 : 0)
}
