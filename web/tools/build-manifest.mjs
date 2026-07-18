// Generate demo/effects.json from the repo's shaders/ folder.
// Shaders live in category directories: shaders/<category>/<effect>/<effect>.slangp
// (uncategorized presets directly under shaders/<effect>/ still work).
import { readdirSync, statSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const shadersDir = path.resolve(here, '..', '..', 'shaders');
const outPath = path.resolve(here, '..', 'demo', 'effects.json');

const CATEGORY_LABELS = {
  adjust: 'Adjust',
  'blur-bloom': 'Blur & bloom',
  stylize: 'Stylize',
  dither: 'Dithering',
  crt: 'CRT & retro displays',
  motion: 'Motion-reactive',
  beat: 'Tempo-synced',
  glitch: 'Glitch & analog',
};

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
  const segs = rel.split('/'); // shaders / <category?> / <effect> / file.slangp
  const category = segs.length >= 4 ? segs[1] : 'other';
  effects.push({ name: path.basename(preset, '.slangp'), path: rel, category });
}

const categories = [...new Set(effects.map((e) => e.category))].map((id) => ({
  id,
  label: CATEGORY_LABELS[id] ?? id,
}));
// Keep the label-map order for known categories, unknown ones last.
categories.sort((a, b) => {
  const ka = Object.keys(CATEGORY_LABELS).indexOf(a.id);
  const kb = Object.keys(CATEGORY_LABELS).indexOf(b.id);
  return (ka < 0 ? 99 : ka) - (kb < 0 ? 99 : kb);
});

writeFileSync(outPath, JSON.stringify({ categories, effects }, null, 2));
console.log(`wrote ${outPath} (${effects.length} effects, ${categories.length} categories)`);
