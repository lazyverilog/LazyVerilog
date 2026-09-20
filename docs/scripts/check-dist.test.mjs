// Fixtures for check-dist.mjs: each mutation is applied to a temp copy of the real
// build output / docs tree and must produce that assertion's own failure message.
// Needs a built site (`npm run build`); every case skips with a message when it is absent.
import { spawnSync } from 'node:child_process'
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { after, before, describe, it } from 'node:test'
import { runChecks } from './check-dist.mjs'

const docsDir = path.resolve(import.meta.dirname, '..')
const repoRoot = path.resolve(docsDir, '..')
const realDist = path.join(docsDir, '.vitepress', 'dist')
const script = path.join(docsDir, 'scripts', 'check-dist.mjs')
const built = existsSync(path.join(realDist, 'index.html'))
const skip = built ? false : 'no build output; run `npm run build` first'

let work
let n = 0

// A fresh copy of dist + docs sources.  The demo media are replaced by empty files of the
// same names: existence is all the checks read, and it keeps each copy small.
function fixture() {
  const root = path.join(work, `case${n++}`)
  const dist = path.join(root, 'dist')
  const docs = path.join(root, 'docs')
  cpSync(realDist, dist, { recursive: true, filter: (src) => path.basename(src) !== 'videos' })
  mkdirSync(path.join(dist, 'videos'), { recursive: true })
  for (const f of readdirSync(path.join(realDist, 'videos'))) writeFileSync(path.join(dist, 'videos', f), '')
  cpSync(docsDir, docs, {
    recursive: true,
    filter: (src) => !['node_modules', 'dist', 'cache', 'videos'].includes(path.basename(src)),
  })
  return { dist, docs }
}

function edit(file, fn) {
  writeFileSync(file, fn(readFileSync(file, 'utf8')))
}

function expectFailure(id, mutate, only) {
  const fx = fixture()
  mutate(fx)
  const failures = runChecks({ dist: fx.dist, docs: fx.docs, only })
  const own = failures.filter((f) => f.startsWith(`${id}:`))
  if (own.length === 0) throw new Error(`${id} did not fail; got: ${JSON.stringify(failures)}`)
}

before(() => {
  work = mkdtempSync(path.join(tmpdir(), 'check-dist-'))
})
after(() => rmSync(work, { recursive: true, force: true }))

describe('check-dist', { skip }, () => {
  it('passes on the real tree', () => {
    const failures = runChecks({ dist: realDist, docs: docsDir })
    if (failures.length) throw new Error(failures.join('\n'))
  })

  it('A1 fails without hero alt text', () =>
    expectFailure('A1', ({ dist }) =>
      edit(path.join(dist, 'index.html'), (s) => s.replace(/alt="LazyVerilog logo[^"]*"/, 'alt=""')),
    ))

  it('A2 fails with too few feature cards', () =>
    expectFailure('A2', ({ dist }) => edit(path.join(dist, 'features.html'), (s) => s.replaceAll('<article class="box"', '<div class="box"'))))

  it('A10 fails without the installation guide on the landing page', () =>
    expectFailure('A10', ({ dist }) => edit(path.join(dist, 'index.html'), (s) => s.replaceAll('href="/installation/neovim"', 'href="/elsewhere"'))))

  it('A3 fails when a hero action is dropped', () =>
    expectFailure('A3', ({ dist }) =>
      edit(path.join(dist, 'index.html'), (s) => s.replace(/<a[^>]*VPButton[^>]*href="\/usage\/"[^>]*>/, (m) => m.replace('VPButton', 'Other'))),
    ))

  it('A4 fails when an action target page is missing', () =>
    expectFailure('A4', ({ dist }) => rmSync(path.join(dist, 'installation', 'index.html'))))

  it('A5 fails on a canonical URL at another origin', () =>
    expectFailure('A5', ({ dist }) =>
      edit(path.join(dist, 'installation', 'neovim.html'), (s) => s.replace('<link rel="canonical" href="https://lazyverilog.github.io/', '<link rel="canonical" href="https://example.com/')),
    ))

  it('A5 fails on an og:url with an extension', () =>
    expectFailure('A5', ({ dist }) =>
      edit(path.join(dist, 'installation', 'neovim.html'), (s) => s.replace('og:url" content="https://lazyverilog.github.io/installation/neovim"', 'og:url" content="https://lazyverilog.github.io/installation/neovim.html"')),
    ))

  it('A5 fails when the og:image file is missing', () => expectFailure('A5', ({ dist }) => rmSync(path.join(dist, 'og.png'))))

  it('A6 fails without .nojekyll', () => expectFailure('A6', ({ dist }) => rmSync(path.join(dist, '.nojekyll'))))

  it('A7 fails for a source page with no sidebar entry', () =>
    expectFailure('A7', ({ docs }) => writeFileSync(path.join(docs, 'x.md'), '# X\n')))

  it('A7 fails for a built page whose source exists but is not built', () =>
    expectFailure('A7', ({ dist }) => rmSync(path.join(dist, 'usage', 'index.html'))))

  it('A7 fails for a sidebar link without a page', () =>
    expectFailure('A7', ({ docs }) =>
      edit(path.join(docs, '.vitepress', 'site.json'), (s) => s.replace('"link": "/usage/"', '"link": "/nowhere"')),
    ))

  it('A7 fails for an emitted page under an excluded directory', () =>
    expectFailure('A7', ({ dist }) => {
      mkdirSync(path.join(dist, 'dev'), { recursive: true })
      writeFileSync(path.join(dist, 'dev', 'x.html'), '<html></html>')
    }))

  it('A7 fails for an unsupported srcExclude pattern', () =>
    expectFailure('A7', ({ docs }) =>
      edit(path.join(docs, '.vitepress', 'site.json'), (s) => s.replace('"dev/**"', '"dev/*.md"')),
    ))

  it('A8 fails for a missing image', () =>
    expectFailure('A8', ({ dist }) => edit(path.join(dist, 'demos.html'), (s) => s.replace('/videos/Format.gif', '/videos/nope.gif'))))

  it('A9 fails when the hero color value changes', () =>
    expectFailure('A9', ({ docs }) =>
      edit(path.join(docs, '.vitepress', 'theme', 'custom.css'), (s) => s.replace('var(--vp-c-text-1)', 'var(--vp-c-brand-1)')),
    ))

  it('A9 fails when the import order is reversed', () =>
    expectFailure('A9', ({ docs }) =>
      edit(path.join(docs, '.vitepress', 'theme', 'index.ts'), (s) => {
        const lines = s.split('\n')
        const a = lines.findIndex((l) => l.includes('@catppuccin'))
        const b = lines.findIndex((l) => l.includes('./custom.css'))
        ;[lines[a], lines[b]] = [lines[b], lines[a]]
        return lines.join('\n')
      }),
    ))

  it('A9 fails with a second declaration', () =>
    expectFailure('A9', ({ docs }) =>
      edit(path.join(docs, '.vitepress', 'theme', 'custom.css'), (s) => s.replace('}', '  color: red;\n}')),
    ))

  it('--only limits the assertions that run', () => {
    const fx = fixture()
    rmSync(path.join(fx.dist, 'og.png'))
    const failures = runChecks({ dist: fx.dist, docs: fx.docs, only: ['A6'] })
    if (failures.length) throw new Error(`A6-only run reported ${failures.join('; ')}`)
  })

  it('defaults resolve from the script location, from any working directory', () => {
    for (const cwd of [repoRoot, docsDir, tmpdir()]) {
      const r = spawnSync(process.execPath, [script], { cwd, encoding: 'utf8' })
      if (r.status !== 0) throw new Error(`cwd ${cwd}: exit ${r.status}\n${r.stderr}`)
    }
  })

  it('explicit --dist and --docs resolve against the current directory', () => {
    const fx = fixture()
    const r = spawnSync(process.execPath, [script, '--dist', 'dist', '--docs', 'docs'], { cwd: path.dirname(fx.dist), encoding: 'utf8' })
    if (r.status !== 0) throw new Error(`exit ${r.status}\n${r.stderr}`)
  })
})
