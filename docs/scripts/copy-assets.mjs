// Copy the demo media from assets/videos/ into docs/public/videos/ so the site
// serves them without committing a second copy.  The destination is gitignored.
// Runs as `prebuild` and `predev`.
import { cp, rm } from 'node:fs/promises'
import path from 'node:path'

const docs = path.resolve(import.meta.dirname, '..')
const source = path.resolve(docs, '..', 'assets', 'videos')
const dest = path.join(docs, 'public', 'videos')

await rm(dest, { recursive: true, force: true })
await cp(source, dest, { recursive: true })
console.log(`copied ${path.relative(process.cwd(), source)} -> ${path.relative(process.cwd(), dest)}`)
