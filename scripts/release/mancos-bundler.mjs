#!/usr/bin/env node
// Mancos bundler launcher: runs the wasm CLI under node, defaulting the Lua
// environment directory to the env/ folder packaged next to this script.
//
//   node mancos-bundler.mjs <factorio-data> <mods-dir|vanilla> <out.yafcbundle>
//   node mancos-bundler.mjs <factorio-data> <mods-dir|vanilla> <env-dir> <out.yafcbundle>
//   node mancos-bundler.mjs --help
//
// Update check (opt-in by prompt): interactive runs ask before downloading a
// newer release over this install. Skip entirely with --no-update-check or
// MANCOS_BUNDLER_NO_UPDATE=1; non-interactive runs (no TTY) never check.
import { spawnSync } from 'node:child_process';
import { createInterface } from 'node:readline/promises';
import { mkdtempSync, writeFileSync, readdirSync, cpSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Stamped with the release tag by the release workflow; dev copies keep the
// placeholder, which disables the update check.
const VERSION = '__MANCOS_BUNDLER_VERSION__';
const RELEASES_API = 'https://api.github.com/repos/hackcasual/mancos/releases/latest';
const RELEASES_PAGE = 'https://github.com/hackcasual/mancos/releases/latest';

const here = dirname(fileURLToPath(import.meta.url));
const rawArgs = process.argv.slice(2);
const skipUpdate = rawArgs.includes('--no-update-check') ||
    process.env.MANCOS_BUNDLER_NO_UPDATE === '1';
const args = rawArgs.filter((a) => a !== '--no-update-check');

// Tags sort as dotted numerics ("v0.1.10" > "v0.1.9"); anything unparsable
// compares equal so we never nag on exotic tags.
function isNewer(remote, local) {
  const parse = (v) => v.replace(/^v/, '').split('.').map((n) => parseInt(n, 10));
  const [a, b] = [parse(remote), parse(local)];
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const [x, y] = [a[i] ?? 0, b[i] ?? 0];
    if (!Number.isFinite(x) || !Number.isFinite(y)) return false;
    if (x !== y) return x > y;
  }
  return false;
}

// Extract a zip with whatever the platform has: bsdtar handles zip on
// Windows 10+/macOS; unzip covers typical Linux. Returns true on success.
function extractZip(zipPath, destDir) {
  for (const [cmd, cmdArgs] of [['tar', ['-xf', zipPath, '-C', destDir]],
                                ['unzip', ['-q', zipPath, '-d', destDir]]]) {
    const result = spawnSync(cmd, cmdArgs, { stdio: 'ignore' });
    if (result.status === 0) return true;
  }
  return false;
}

async function maybeSelfUpdate() {
  if (skipUpdate || VERSION.startsWith('__') || !process.stdout.isTTY ||
      !process.stdin.isTTY || args.some((a) => a === '-h' || a === '--help')) {
    return;
  }
  let release;
  try {
    const response = await fetch(RELEASES_API, {
      signal: AbortSignal.timeout(3000),
      headers: { accept: 'application/vnd.github+json' },
    });
    if (!response.ok) return;
    release = await response.json();
  } catch { return; }  // offline/rate-limited: silently continue
  const tag = release?.tag_name;
  if (!tag || !isNewer(tag, VERSION)) return;

  const asset = (release.assets ?? []).find((a) =>
      a.name === `mancos-bundler-${tag}.zip`);
  console.log(`mancos-bundler ${tag} is available (you have ${VERSION}).`);
  if (!asset) {
    console.log(`  download: ${RELEASES_PAGE}`);
    return;
  }
  const rl = createInterface({ input: process.stdin, output: process.stdout });
  const answer = (await rl.question(`Download and install over ${here}? [y/N] `)).trim();
  rl.close();
  if (answer.toLowerCase() !== 'y') return;

  const staging = mkdtempSync(join(tmpdir(), 'mancos-bundler-update-'));
  try {
    const zipPath = join(staging, asset.name);
    const download = await fetch(asset.browser_download_url,
                                 { signal: AbortSignal.timeout(120000) });
    if (!download.ok) throw new Error(`download failed: HTTP ${download.status}`);
    writeFileSync(zipPath, Buffer.from(await download.arrayBuffer()));
    if (!extractZip(zipPath, staging)) {
      throw new Error('no tar/unzip available to extract the archive');
    }
    // The zip wraps everything in a mancos-bundler-<tag>/ directory.
    const inner = readdirSync(staging).find((d) => d.startsWith('mancos-bundler-') &&
                                                   !d.endsWith('.zip'));
    if (!inner) throw new Error('unexpected archive layout');
    cpSync(join(staging, inner), here, { recursive: true, force: true });
    console.log(`updated to ${tag} — takes effect on the next run.\n`);
  } catch (error) {
    console.log(`update failed (${error.message}) — continuing with ${VERSION}.`);
    console.log(`  manual download: ${RELEASES_PAGE}`);
  } finally {
    rmSync(staging, { recursive: true, force: true });
  }
}

await maybeSelfUpdate();

if (args.length === 3 && !args.some((a) => a === '-h' || a === '--help')) {
  args.splice(2, 0, join(here, 'env'));  // insert the packaged env dir
}
const result = spawnSync(process.execPath,
    [join(here, 'mancos_bundler_node.js'), ...args], { stdio: 'inherit' });
process.exit(result.status ?? 1);
