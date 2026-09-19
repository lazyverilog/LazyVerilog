#!/usr/bin/env node
// Translates the root README.md into docs/i18n/README.<lang>.md using the
// Claude Agent SDK.  Authentication comes from the environment the SDK already
// reads (CLAUDE_CODE_OAUTH_TOKEN in CI, or a local `claude` login).
//
//   node index.ts fr ko          # named languages
//   node index.ts --tier 1       # a whole tier
//   node index.ts --all          # every tier
//
// Unchanged sources are served from docs/i18n/.translation-cache.json rather
// than re-translated; --force ignores the cache.

import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { query } from "@anthropic-ai/claude-agent-sdk";

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const SOURCE = path.join(REPO_ROOT, "README.md");
const OUT_DIR = path.join(REPO_ROOT, "docs", "i18n");
const OUT_PATTERN = "README.{lang}.md";
const CACHE_FILE = path.join(OUT_DIR, ".translation-cache.json");

const DEFAULT_MODEL = "claude-opus-5";
const DEFAULT_CONCURRENCY = 10;

// Bumped whenever the prompt below changes in a way that should invalidate
// every cached translation.
const PROMPT_VERSION = 1;

type Language = { name: string; endonym: string; tier: number };

// Tiers follow the source project's grouping; `name` is what the prompt asks
// for, `endonym` is what the README language bar shows.
const LANGUAGES: Record<string, Language> = {
  "zh-CN": { name: "Simplified Chinese", endonym: "简体中文", tier: 1 },
  ja: { name: "Japanese", endonym: "日本語", tier: 1 },
  pt: { name: "Portuguese (Brazil)", endonym: "Português", tier: 1 },
  ko: { name: "Korean", endonym: "한국어", tier: 1 },
  es: { name: "Spanish", endonym: "Español", tier: 1 },
  de: { name: "German", endonym: "Deutsch", tier: 1 },
  fr: { name: "French", endonym: "Français", tier: 1 },

  he: { name: "Hebrew", endonym: "עברית", tier: 2 },
  ar: { name: "Arabic", endonym: "العربية", tier: 2 },
  ru: { name: "Russian", endonym: "Русский", tier: 2 },
  pl: { name: "Polish", endonym: "Polski", tier: 2 },
  cs: { name: "Czech", endonym: "Čeština", tier: 2 },
  nl: { name: "Dutch", endonym: "Nederlands", tier: 2 },
  tr: { name: "Turkish", endonym: "Türkçe", tier: 2 },
  uk: { name: "Ukrainian", endonym: "Українська", tier: 2 },

  vi: { name: "Vietnamese", endonym: "Tiếng Việt", tier: 3 },
  tl: { name: "Tagalog", endonym: "Tagalog", tier: 3 },
  id: { name: "Indonesian", endonym: "Bahasa Indonesia", tier: 3 },
  th: { name: "Thai", endonym: "ไทย", tier: 3 },
  hi: { name: "Hindi", endonym: "हिन्दी", tier: 3 },
  bn: { name: "Bengali", endonym: "বাংলা", tier: 3 },
  ur: { name: "Urdu", endonym: "اردو", tier: 3 },
  ro: { name: "Romanian", endonym: "Română", tier: 3 },
  sv: { name: "Swedish", endonym: "Svenska", tier: 3 },

  it: { name: "Italian", endonym: "Italiano", tier: 4 },
  el: { name: "Greek", endonym: "Ελληνικά", tier: 4 },
  hu: { name: "Hungarian", endonym: "Magyar", tier: 4 },
  fi: { name: "Finnish", endonym: "Suomi", tier: 4 },
  da: { name: "Danish", endonym: "Dansk", tier: 4 },
  no: { name: "Norwegian", endonym: "Norsk", tier: 4 },
};

type Args = {
  langs: string[];
  force: boolean;
  useExisting: boolean;
  dryRun: boolean;
  model: string;
  concurrency: number;
};

function parseArgs(argv: string[]): Args {
  const langs = new Set<string>();
  let force = false;
  let useExisting = false;
  let dryRun = false;
  let model = process.env.TRANSLATE_MODEL || DEFAULT_MODEL;
  let concurrency = DEFAULT_CONCURRENCY;

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--all") {
      for (const code of Object.keys(LANGUAGES)) langs.add(code);
    } else if (arg === "--tier") {
      const tier = Number(argv[++i]);
      const codes = Object.keys(LANGUAGES).filter((c) => LANGUAGES[c].tier === tier);
      if (codes.length === 0) throw new Error(`no such tier: ${argv[i]}`);
      for (const code of codes) langs.add(code);
    } else if (arg === "--force") {
      force = true;
    } else if (arg === "--use-existing") {
      useExisting = true;
    } else if (arg === "--dry-run") {
      dryRun = true;
    } else if (arg === "--model") {
      model = argv[++i];
    } else if (arg === "--concurrency") {
      concurrency = Number(argv[++i]);
      if (!Number.isInteger(concurrency) || concurrency < 1) {
        throw new Error("--concurrency takes a positive integer");
      }
    } else if (arg === "--help" || arg === "-h") {
      printUsage();
      process.exit(0);
    } else if (arg.startsWith("-")) {
      throw new Error(`unknown option: ${arg}`);
    } else {
      if (!(arg in LANGUAGES)) throw new Error(`unknown language code: ${arg}`);
      langs.add(arg);
    }
  }

  if (langs.size === 0) throw new Error("no languages selected (pass codes, --tier N, or --all)");
  return { langs: [...langs], force, useExisting, dryRun, model, concurrency };
}

function printUsage(): void {
  const byTier = new Map<number, string[]>();
  for (const [code, lang] of Object.entries(LANGUAGES)) {
    byTier.set(lang.tier, [...(byTier.get(lang.tier) ?? []), code]);
  }
  console.log("usage: node index.ts [<lang>...] [--tier N] [--all] [options]");
  console.log("");
  console.log("options:");
  console.log("  --force            re-translate even when the cached source hash matches");
  console.log("  --use-existing     show Claude the current translation as a terminology reference");
  console.log("  --dry-run          print the plan and the prepared source, call no model");
  console.log(`  --model <id>       default ${DEFAULT_MODEL} (or $TRANSLATE_MODEL)`);
  console.log(`  --concurrency <n>  default ${DEFAULT_CONCURRENCY}`);
  console.log("");
  for (const tier of [...byTier.keys()].sort()) {
    console.log(`  tier ${tier}: ${byTier.get(tier)!.join(" ")}`);
  }
}

// README.md links its siblings relatively (`docs/features.md`, `assets/...`),
// and the translations live two directories down, so those targets are
// rewritten to point back at the repo root.  Done here rather than in the
// prompt: it is path arithmetic, and the model should only have to preserve
// what it is handed.
function rewriteRelativeLinks(markdown: string, prefix: string): string {
  const absolute = /^(?:[a-z][a-z0-9+.-]*:|\/\/|\/|#)/i;
  const retarget = (target: string): string =>
    absolute.test(target) || target.startsWith("../") ? target : prefix + target;

  let inFence = false;
  return markdown
    .split("\n")
    .map((line) => {
      if (/^\s*(```|~~~)/.test(line)) {
        inFence = !inFence;
        return line;
      }
      if (inFence) return line;
      return line
        .replace(/(\]\()([^)\s]+)/g, (_m, open: string, target: string) => open + retarget(target))
        .replace(
          /\b(src|href)=(["'])([^"']+)\2/gi,
          (_m, attr: string, quote: string, target: string) =>
            `${attr}=${quote}${retarget(target)}${quote}`,
        );
    })
    .join("\n");
}

const SYSTEM_PROMPT = [
  "You are a professional technical translator working on the documentation of an",
  "open-source SystemVerilog language server. You translate Markdown documents and",
  "return nothing but the translated document.",
].join(" ");

function buildPrompt(lang: Language, source: string, existing: string | null): string {
  const parts = [
    `Translate the following README from English into ${lang.name}.`,
    "",
    "Rules:",
    `- Translate the prose into natural, idiomatic ${lang.name} as a software engineer would write it.`,
    "- Preserve the Markdown structure exactly: heading levels, lists, tables, blockquotes, and inline HTML.",
    "- Do NOT translate anything inside code blocks or inline code: shell commands, code samples,",
    "  file paths, configuration keys, option names, identifiers, and CLI flags stay verbatim.",
    "- Keep every URL and link target byte-for-byte as written, including relative paths such as",
    "  `../../docs/features.md`. Translate only the link text.",
    "- Keep the product name LazyVerilog, and keep established industry terms (SystemVerilog, RTL, LSP,",
    "  linter, formatter) in the form engineers in this language actually use, in English where that is the norm.",
    "- Anchor links (`#some-heading`) must keep matching their headings: if you translate a heading,",
    "  update every in-document link that points at it so it still resolves.",
    `- Begin the document with one blockquote line, written in ${lang.name}, stating that this is an`,
    "  automated translation generated by Claude and that the English original is authoritative, and",
    "  containing the Markdown link `[README.md](../../README.md)`.",
    "",
    "Output only the translated Markdown document. No preamble, no commentary, and do not wrap the",
    "whole document in a code fence.",
  ];

  if (existing) {
    parts.push(
      "",
      `An earlier translation into ${lang.name} is included below for terminology and style reference`,
      "only. The English README is authoritative: follow the earlier translation's word choices where",
      "they are good, but translate the current English text, including anything the earlier version",
      "is missing.",
      "",
      "<previous-translation>",
      existing,
      "</previous-translation>",
    );
  }

  parts.push("", "<readme>", source, "</readme>");
  return parts.join("\n");
}

// The model is asked not to fence the whole document, but a stray fence would
// otherwise be written to disk verbatim.
function stripDocumentFence(text: string): string {
  const trimmed = text.trim();
  const match = /^```[a-zA-Z]*\n([\s\S]*)\n```$/.exec(trimmed);
  return match ? match[1] : trimmed;
}

type Translation = { text: string; costUsd: number };

async function translate(lang: Language, prompt: string, model: string): Promise<Translation> {
  for await (const message of query({
    prompt,
    options: {
      model,
      systemPrompt: { type: "custom", prompt: SYSTEM_PROMPT },
      // Pure text transformation: no tools, and no repository CLAUDE.md,
      // settings, hooks or MCP servers pulled into the run.
      tools: [],
      settingSources: [],
      maxTurns: 1,
      persistSession: false,
      cwd: REPO_ROOT,
    },
  })) {
    if (message.type !== "result") continue;
    if (message.subtype !== "success" || message.is_error) {
      throw new Error(`translation into ${lang.name} failed: ${message.subtype}`);
    }
    const text = stripDocumentFence(message.result);
    if (text.length === 0) throw new Error(`translation into ${lang.name} came back empty`);
    return { text, costUsd: message.total_cost_usd ?? 0 };
  }
  throw new Error(`translation into ${lang.name} ended without a result`);
}

type CacheEntry = {
  sourceHash: string;
  model: string;
  promptVersion: number;
  generatedAt: string;
  costUsd: number;
};
type Cache = { version: number; entries: Record<string, CacheEntry> };

async function readCache(): Promise<Cache> {
  try {
    const parsed = JSON.parse(await readFile(CACHE_FILE, "utf8")) as Cache;
    if (parsed?.version === 1 && parsed.entries) return parsed;
  } catch {
    // A missing or unreadable cache just means everything is translated again.
  }
  return { version: 1, entries: {} };
}

function outPath(code: string): string {
  return path.join(OUT_DIR, OUT_PATTERN.replace("{lang}", code));
}

// Bounded fan-out: `limit` workers pulling from one queue of language codes.
async function runPool<T>(items: T[], limit: number, work: (item: T) => Promise<void>): Promise<void> {
  let next = 0;
  const workers = Array.from({ length: Math.min(limit, items.length) }, async () => {
    while (true) {
      const index = next++;
      if (index >= items.length) return;
      await work(items[index]);
    }
  });
  await Promise.all(workers);
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));

  const source = await readFile(SOURCE, "utf8");
  const sourceHash = createHash("sha256").update(source).digest("hex");
  const toRoot = `${path.relative(OUT_DIR, REPO_ROOT).split(path.sep).join("/")}/`;
  const rewritten = rewriteRelativeLinks(source, toRoot);

  await mkdir(OUT_DIR, { recursive: true });
  const cache = await readCache();

  const stale = args.langs.filter((code) => {
    if (args.force) return true;
    const entry = cache.entries[code];
    return !(
      entry &&
      entry.sourceHash === sourceHash &&
      entry.model === args.model &&
      entry.promptVersion === PROMPT_VERSION &&
      existsSync(outPath(code))
    );
  });

  for (const code of args.langs) {
    if (!stale.includes(code)) console.log(`- ${code}: up to date`);
  }
  if (stale.length === 0) {
    console.log("nothing to translate");
    return;
  }

  console.log(`translating ${stale.length} language(s) with ${args.model}: ${stale.join(" ")}`);

  if (args.dryRun) {
    // The prepared source is the same for every language, so printing it once
    // is the whole reviewable input.
    console.log(`--- prepared source (links rewritten against ${toRoot}) ---`);
    console.log(rewritten);
    return;
  }

  const failures: string[] = [];
  let totalCost = 0;

  await runPool(stale, args.concurrency, async (code) => {
    const lang = LANGUAGES[code];
    const target = outPath(code);
    const existing = args.useExisting && existsSync(target) ? await readFile(target, "utf8") : null;
    const started = Date.now();
    try {
      const { text, costUsd } = await translate(
        lang,
        buildPrompt(lang, rewritten, existing),
        args.model,
      );
      await writeFile(target, text.endsWith("\n") ? text : `${text}\n`, "utf8");
      cache.entries[code] = {
        sourceHash,
        model: args.model,
        promptVersion: PROMPT_VERSION,
        generatedAt: new Date().toISOString(),
        costUsd,
      };
      totalCost += costUsd;
      const seconds = ((Date.now() - started) / 1000).toFixed(1);
      console.log(
        `ok ${code}: ${path.relative(REPO_ROOT, target)} (${seconds}s, $${costUsd.toFixed(4)})`,
      );
    } catch (error) {
      failures.push(code);
      console.error(`FAILED ${code}: ${error instanceof Error ? error.message : String(error)}`);
    }
  });

  // Written even on partial failure so a rerun only retries what failed.
  const ordered = Object.fromEntries(
    Object.keys(cache.entries)
      .sort()
      .map((code) => [code, cache.entries[code]]),
  );
  await writeFile(CACHE_FILE, `${JSON.stringify({ version: 1, entries: ordered }, null, 2)}\n`, "utf8");

  console.log(`total estimated cost: $${totalCost.toFixed(4)}`);
  if (failures.length > 0) {
    console.error(`failed: ${failures.join(" ")}`);
    process.exitCode = 1;
  }
}

await main();
