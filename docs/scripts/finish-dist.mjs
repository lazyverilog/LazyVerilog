// Runs as `postbuild`.  Pages-from-branch runs Jekyll unless `.nojekyll` is present.
import { writeFile } from 'node:fs/promises'
import path from 'node:path'

const dist = path.resolve(import.meta.dirname, '..', '.vitepress', 'dist')
await writeFile(path.join(dist, '.nojekyll'), '')
console.log('wrote .nojekyll')
