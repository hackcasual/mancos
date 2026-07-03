#!/usr/bin/env node
// Mancos bundler launcher: runs the wasm CLI under node, defaulting the Lua
// environment directory to the env/ folder packaged next to this script.
//
//   node mancos-bundler.mjs <factorio-data> <mods-dir|vanilla> <out.yafcbundle>
//   node mancos-bundler.mjs <factorio-data> <mods-dir|vanilla> <env-dir> <out.yafcbundle>
//   node mancos-bundler.mjs --help
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.length === 3 && !args.some((a) => a === '-h' || a === '--help')) {
  args.splice(2, 0, join(here, 'env'));  // insert the packaged env dir
}
const result = spawnSync(process.execPath,
    [join(here, 'mancos_bundler_node.js'), ...args], { stdio: 'inherit' });
process.exit(result.status ?? 1);
