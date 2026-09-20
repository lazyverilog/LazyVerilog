import { defineConfig } from 'vitepress'
import site from './site.json'

const ORIGIN = 'https://lazyverilog.github.io'
const DESCRIPTION =
  'LazyVerilog is a fast, practical SystemVerilog LSP written in C++ for Neovim and VS Code: ' +
  'formatting, linting, navigation, and RTL automation.'

// `installation.md` -> `installation`, `autoinst/index.md` -> `autoinst/`, `index.md` -> ``.
function pagePath(relativePath: string): string {
  return relativePath.replace(/(^|\/)index\.md$/, '$1').replace(/\.md$/, '')
}

export default defineConfig({
  title: 'LazyVerilog',
  description: DESCRIPTION,
  lang: 'en-US',
  base: '/',
  cleanUrls: true,
  srcExclude: site.srcExclude,
  lastUpdated: false,

  markdown: {
    image: { lazyLoading: true },
    theme: { light: 'catppuccin-latte', dark: 'catppuccin-mocha' },
    // The docs fence SystemVerilog as `systemverilog`; Shiki names the grammar `system-verilog`.
    languageAlias: { systemverilog: 'system-verilog', sv: 'system-verilog' },
  },

  sitemap: { hostname: `${ORIGIN}/` },

  head: [['link', { rel: 'icon', type: 'image/png', href: '/favicon.png' }]],

  // Per-page canonical and social tags.  A static `head` entry would repeat the
  // landing-page URL on every page.
  transformHead({ pageData }) {
    if (pageData.isNotFound) return []
    const url = `${ORIGIN}/${pagePath(pageData.relativePath)}`
    const title = pageData.title || 'LazyVerilog'
    const description = pageData.description || DESCRIPTION
    return [
      ['link', { rel: 'canonical', href: url }],
      ['meta', { property: 'og:type', content: 'website' }],
      ['meta', { property: 'og:site_name', content: 'LazyVerilog' }],
      ['meta', { property: 'og:title', content: title }],
      ['meta', { property: 'og:description', content: description }],
      ['meta', { property: 'og:url', content: url }],
      ['meta', { property: 'og:image', content: `${ORIGIN}/og.png` }],
      ['meta', { name: 'twitter:card', content: 'summary_large_image' }],
      ['meta', { name: 'twitter:image', content: `${ORIGIN}/og.png` }],
    ]
  },

  themeConfig: {
    logo: '/favicon.png',
    nav: [
      { text: 'Installation', link: '/installation/' },
      { text: 'Usage', link: '/usage/' },
      { text: 'Configuration', link: '/configuration' },
      { text: 'Features', link: '/features' },
      { text: 'Demos', link: '/demos' },
      { text: 'Releases', link: 'https://github.com/lazyverilog/LazyVerilog/releases' },
      { text: 'Sponsor', link: 'https://github.com/sponsors/kjoonha' },
    ],
    sidebar: site.sidebar,
    socialLinks: [{ icon: 'github', link: 'https://github.com/lazyverilog/LazyVerilog' }],
    search: { provider: 'local' },
    editLink: {
      pattern: 'https://github.com/lazyverilog/LazyVerilog/edit/main/docs/:path',
      text: 'Edit this page on GitHub',
    },
    footer: {
      message: 'Released under the MIT License. Themed with Catppuccin.',
    },
  },
})
