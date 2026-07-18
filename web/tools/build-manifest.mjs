// Generate demo/effects.json from the repo's shaders/ folder.
import { readdirSync, statSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const shadersDir = path.resolve(here, '..', '..', 'shaders');
const outPath = path.resolve(here, '..', 'demo', 'effects.json');

function* findPresets(dir) {
  for (const name of readdirSync(dir).sort()) {
    const full = path.join(dir, name);
    if (statSync(full).isDirectory()) yield* findPresets(full);
    else if (name.endsWith('.slangp')) yield full;
  }
}

const effects = [];
for (const preset of findPresets(shadersDir)) {
  const rel = path.relative(path.resolve(shadersDir, '..'), preset).replace(/\\/g, '/');
  effects.push({ name: path.basename(preset, '.slangp'), path: rel });
}

writeFileSync(outPath, JSON.stringify({ effects }, null, 2));
console.log(`wrote ${outPath} (${effects.length} effects)`);
