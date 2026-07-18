// Batch test: compile every bundled preset's passes through the wasm chain.
// Usage: node tools/test-compile.mjs [--verbose] [presetPathFilter]
import { createRequire } from 'node:module';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { parsePreset, dirnameOf } from '../src/slangp.js';
import { compileSlang, wgslDeclaredBindings } from '../src/compiler.js';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const shadersDir = path.resolve(here, '..', '..', 'shaders');

const verbose = process.argv.includes('--verbose');
const filter = process.argv.slice(2).find((a) => !a.startsWith('--'));

const glslang = require('@webgpu/glslang/dist/node-devel/glslang.js')();
const twgsl = await require(path.join(here, '..', 'vendor', 'twgsl.js'))(
  path.join(here, '..', 'vendor', 'twgsl.wasm')
);

const readFile = async (p) => readFileSync(p.replace(/\//g, path.sep), 'utf8');

function* findPresets(dir) {
  for (const name of readdirSync(dir)) {
    const full = path.join(dir, name);
    if (statSync(full).isDirectory()) yield* findPresets(full);
    else if (name.endsWith('.slangp')) yield full;
  }
}

let ok = 0;
let fail = 0;
const failures = [];

for (const presetPath of findPresets(shadersDir)) {
  if (filter && !presetPath.includes(filter)) continue;
  const rel = path.relative(shadersDir, presetPath);
  try {
    const preset = parsePreset(readFileSync(presetPath, 'utf8'), dirnameOf(presetPath.replace(/\\/g, '/')));
    const mods = [];
    for (const pass of preset.passes) {
      const src = await readFile(pass.path);
      const mod = await compileSlang(src, { path: pass.path, readFile, glslang, twgsl });
      mods.push(mod);
    }
    ok++;
    if (verbose) {
      console.log(`OK   ${rel} (${preset.passes.length} pass${preset.passes.length > 1 ? 'es' : ''})`);
      for (const mod of mods) {
        const b = wgslDeclaredBindings(mod.fragWgsl);
        console.log(
          `     ${path.basename(mod.path)}: params=[${mod.params.map((p) => p.name).join(',')}]` +
          ` samplers=[${mod.samplers.map((s) => s.name).join(',')}]` +
          ` push=${mod.push ? mod.push.size : 0}B ubo=${mod.ubo ? mod.ubo.size : 0}B bindings=${b.length}`
        );
      }
    } else {
      console.log(`OK   ${rel}`);
    }
  } catch (e) {
    fail++;
    failures.push({ rel, msg: e.message });
    console.log(`FAIL ${rel}`);
    console.log(`     ${String(e.message).split('\n').slice(0, 6).join('\n     ')}`);
  }
}

console.log(`\n${ok} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
