/*
 * slangfx live — web demo UI.
 *
 * Proof-of-concept port of wrappers/slangfx_live.py on top of the
 * slangfx-web engine: media loading (video or still), transport with native
 * scrubbing, a layer stack with per-parameter live sliders, PNG frame
 * export, and WebM recording of the processed output.
 */

import { SlangFx, loadToolchain } from '../src/index.js';

const $ = (id) => document.getElementById(id);
const statusEl = $('status');
const canvas = $('preview');
const video = $('video');
const scrub = $('scrub');
const timeLabel = $('time-label');
const playBtn = $('btn-play');
const layersEl = $('layers');
const addLayerSearch = $('add-layer-search');
const addLayerList = $('add-layer-list');

const VIDEO_EXTS = /\.(mp4|mov|mkv|webm|avi|m4v|gif)$/i;

/* ---- custom (hand-written) shader layers --------------------------- */

/* Custom layers are backed by in-memory "virtual files" served to the
 * engine through readFile, under a reserved path prefix. Each custom layer
 * gets its own directory so several can coexist. */
const CUSTOM_PREFIX = 'custom/';
const virtualFiles = new Map(); // path -> source text
let customCounter = 0;

const CUSTOM_PRESET = `shaders = 1
shader0        = custom.slang
filter_linear0 = true
scale_type0    = viewport
scale0         = 1.0
wrap_mode0     = clamp_to_edge
`;

const CUSTOM_BOILERPLATE = `#version 450

// Hand-written slang shader — edit and hit Compile.
//
// Declare a tunable with one line and it appears as a slider:
//   //@param name "Label" default min max step
// Reference it by bare name in the code below.
//
// Inputs the engine fills in every frame:
//   Source            previous layer's output (or the video itself)
//   vTexCoord         0..1 UV, (0,0) = top-left
//   params.SourceSize (w, h, 1/w, 1/h) of Source
//   params.OutputSize (w, h, 1/w, 1/h) of this pass
//   params.FrameCount frame counter (uint)
//   params.Time       seconds (video time)

layout(push_constant) uniform Push
{
    vec4  SourceSize;
    vec4  OutputSize;
    uint  FrameCount;
    float Time;
} params;

//@param amount "Mix" 1.0 0.0 1.0 0.01
//@param wobble "Wobble (px)" 6.0 0.0 64.0 0.5
//@param speed  "Speed" 1.5 0.0 8.0 0.05

layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
    vec2 uv = vTexCoord;
    uv.x += sin(uv.y * 24.0 + params.Time * speed * 6.2832)
            * wobble * params.SourceSize.z;

    vec3 c = texture(Source, uv).rgb;
    c *= vec3(1.05, 0.95, 1.10);              // playground: make it yours

    vec3 base = texture(Source, vTexCoord).rgb;
    FragColor = vec4(mix(base, c, amount), 1.0);
}
`;

/* ---- saved shaders (localStorage) ---------------------------------- */

const SAVED_KEY = 'slangfx-web.saved-shaders';

function loadSaved() {
  try { return JSON.parse(localStorage.getItem(SAVED_KEY)) ?? {}; }
  catch { return {}; }
}

function storeSaved(saves) {
  localStorage.setItem(SAVED_KEY, JSON.stringify(saves));
  populateSavedOptions();
}

const savedNames = new Map(); // custom layer dir -> saved-shader name

function newCustomLayerFiles(source = CUSTOM_BOILERPLATE) {
  const dir = `${CUSTOM_PREFIX}${customCounter++}/`;
  virtualFiles.set(dir + 'custom.slangp', CUSTOM_PRESET);
  virtualFiles.set(dir + 'custom.slang', source);
  return dir;
}

let fx = null;
let manifest = { categories: [], effects: [] };
let mediaKind = null;      // 'video' | 'image' | null
let imageBitmap = null;
let imageDirty = false;
let scrubbing = false;
let recorder = null;
let elementCapture = null;   // lazy HTMLMediaElement.captureStream for audio

function setStatus(msg) { statusEl.textContent = msg; }

function fmtTime(t) {
  if (!Number.isFinite(t)) return '--:--';
  const m = Math.floor(t / 60);
  const s = Math.floor(t % 60);
  return `${m}:${String(s).padStart(2, '0')}`;
}

/* ---- boot ---------------------------------------------------------- */

async function boot() {
  if (!navigator.gpu) {
    setStatus('WebGPU is not available. Use Chrome/Edge 113+ (or enable WebGPU).');
    return;
  }
  // Shader paths are repo-root-relative ("shaders/x/y.slangp"). The demo
  // lives at <root>/web/demo/, so resolve against ../../ — this works both
  // on localhost and when hosted under a subpath (GitHub Pages).
  const ROOT = new URL('../../', location.href);
  const rootUrl = (p) => new URL(p.replace(/^\/+/, ''), ROOT);
  try {
    const toolchain = await loadToolchain();
    fx = await SlangFx.create({
      canvas,
      toolchain,
      readFile: async (p) => {
        const clean = p.replace(/^\/+/, '');
        if (virtualFiles.has(clean)) return virtualFiles.get(clean);
        const res = await fetch(rootUrl(p));
        if (!res.ok) throw new Error(`HTTP ${res.status} for ${p}`);
        return res.text();
      },
      readImage: async (p) => {
        const res = await fetch(rootUrl(p));
        if (!res.ok) throw new Error(`HTTP ${res.status} for ${p}`);
        return createImageBitmap(await res.blob());
      },
    });
  } catch (e) {
    setStatus(`init failed: ${e.message}`);
    throw e;
  }

  try {
    manifest = await (await fetch('effects.json')).json();
  } catch {
    manifest = { categories: [], effects: [] };
    setStatus('effects.json missing — run: node web/tools/build-manifest.mjs');
  }

  setStatus('ready — load a video or image');
  window.fx = fx;                            // console/debug access
  window.renderLayerPanel = renderLayerPanel;
  requestAnimationFrame(tick);
  restoreSession();
}

/* ---- render loop --------------------------------------------------- */

function tick() {
  if (fx && mediaKind === 'video' && video.readyState >= 2) {
    fx.render(video, video.currentTime);
    if (!scrubbing && Number.isFinite(video.duration)) {
      scrub.value = String(video.currentTime);
      timeLabel.textContent = `${fmtTime(video.currentTime)} / ${fmtTime(video.duration)}`;
    }
  } else if (fx && mediaKind === 'image') {
    fx.render(imageDirty ? imageBitmap : null);
    imageDirty = false;
  }
  requestAnimationFrame(tick);
}

/* ---- media --------------------------------------------------------- */

async function loadMedia(file, { persist = true } = {}) {
  const url = URL.createObjectURL(file);
  const isVideo = file.type.startsWith('video/') || VIDEO_EXTS.test(file.name);
  document.body.classList.add('has-media');
  stopMaskEdit();
  if (persist) idbSet('current', file).catch(() => {});

  if (isVideo) {
    mediaKind = 'video';
    imageBitmap = null;
    video.src = url;
    await new Promise((res, rej) => {
      video.onloadedmetadata = res;
      video.onerror = () => rej(new Error('could not open video'));
    });
    canvas.width = video.videoWidth;
    canvas.height = video.videoHeight;
    await fx.setSourceSize(video.videoWidth, video.videoHeight);
    rescaleMasks();
    const seekable = Number.isFinite(video.duration) && video.duration > 0;
    scrub.disabled = !seekable;
    scrub.max = seekable ? String(video.duration) : '0';
    playBtn.disabled = false;
    playBtn.textContent = 'Pause';
    muteBtn.disabled = false;
    volumeSlider.disabled = false;
    applyAudioPrefs();
    await playWithAudioPolicy();
    setStatus(`${file.name} — ${video.videoWidth}×${video.videoHeight}`);
  } else {
    mediaKind = 'image';
    video.pause();
    video.removeAttribute('src');
    imageBitmap = await createImageBitmap(file);
    canvas.width = imageBitmap.width;
    canvas.height = imageBitmap.height;
    await fx.setSourceSize(imageBitmap.width, imageBitmap.height);
    rescaleMasks();
    imageDirty = true;
    scrub.disabled = true;
    playBtn.disabled = true;
    muteBtn.disabled = true;
    volumeSlider.disabled = true;
    timeLabel.textContent = '--:-- / --:--';
    setStatus(`${file.name} — ${imageBitmap.width}×${imageBitmap.height}`);
  }
  renderLayerPanel();
}

$('file-input').addEventListener('change', (e) => {
  if (e.target.files[0]) loadMedia(e.target.files[0]);
});

document.body.addEventListener('dragover', (e) => e.preventDefault());
document.body.addEventListener('drop', async (e) => {
  e.preventDefault();
  for (const file of e.dataTransfer.files) {
    if (file.name.endsWith('.slangp')) continue; // catalog covers shaders
    await loadMedia(file);
    break;
  }
});

/* ---- transport ----------------------------------------------------- */

playBtn.addEventListener('click', () => {
  if (video.paused) { video.play(); playBtn.textContent = 'Pause'; }
  else { video.pause(); playBtn.textContent = 'Play'; }
});

/* ---- audio --------------------------------------------------------- */

const AUDIO_KEY = 'slangfx-web.audio';
const muteBtn = $('btn-mute');
const volumeSlider = $('volume');

function audioPrefs() {
  try { return JSON.parse(localStorage.getItem(AUDIO_KEY)) ?? {}; }
  catch { return {}; }
}

function saveAudioPrefs() {
  try { localStorage.setItem(AUDIO_KEY, JSON.stringify({ muted: video.muted, volume: video.volume })); } catch {}
}

function updateAudioUI() {
  muteBtn.textContent = video.muted || video.volume === 0 ? '🔇' : '🔊';
  volumeSlider.value = String(video.volume);
}

/* Apply stored prefs; if the browser blocks unmuted autoplay, fall back
 * to muted so playback still starts, and let the user unmute by hand. */
function applyAudioPrefs() {
  const p = audioPrefs();
  video.volume = p.volume ?? 1;
  video.muted = p.muted ?? false;
  updateAudioUI();
}

async function playWithAudioPolicy() {
  try {
    await video.play();
  } catch {
    video.muted = true;
    updateAudioUI();
    setStatus('autoplay was blocked with sound — click 🔇 to unmute');
    await video.play().catch(() => {});
  }
}

muteBtn.addEventListener('click', () => {
  video.muted = !video.muted;
  if (!video.muted && video.volume === 0) video.volume = 0.5;
  updateAudioUI();
  saveAudioPrefs();
});

volumeSlider.addEventListener('input', () => {
  video.volume = parseFloat(volumeSlider.value);
  if (video.volume > 0) video.muted = false;   // moving the slider implies sound
  updateAudioUI();
  saveAudioPrefs();
});

scrub.addEventListener('input', () => {
  scrubbing = true;
  timeLabel.textContent = `${fmtTime(parseFloat(scrub.value))} / ${fmtTime(video.duration)}`;
});
scrub.addEventListener('change', () => {
  video.currentTime = parseFloat(scrub.value);
  scrubbing = false;
});

/* ---- viewer sizing modes (fit / cover / 1:1) ------------------------ */

const viewer = $('viewer');
const VIEWMODE_KEY = 'slangfx-web.viewmode';

/* Fit-mode sizing: cap the canvas to the pane in device pixels. Inline
 * styles (not CSS %) because percentage max-heights can't resolve through
 * the fit-content wrappers that keep the mask overlay glued to the canvas. */
function applyViewSizing() {
  if (viewer.classList.contains('size-fit') && !document.fullscreenElement) {
    canvas.style.maxWidth = `${viewer.clientWidth}px`;
    canvas.style.maxHeight = `${viewer.clientHeight}px`;
  } else {
    canvas.style.maxWidth = '';
    canvas.style.maxHeight = '';
  }
}

new ResizeObserver(applyViewSizing).observe(viewer);
document.addEventListener('fullscreenchange', applyViewSizing);

function setViewMode(mode) {
  viewer.className = `size-${mode}`;
  for (const b of document.querySelectorAll('#view-controls .btn'))
    b.classList.toggle('active', b.dataset.mode === mode);
  applyViewSizing();
  try { localStorage.setItem(VIEWMODE_KEY, mode); } catch {}
}

for (const b of document.querySelectorAll('#view-controls .btn'))
  b.addEventListener('click', () => setViewMode(b.dataset.mode));
setViewMode(localStorage.getItem(VIEWMODE_KEY) ?? 'fit');

/* Drag-to-pan in 1:1 mode — the pane is a scroll container in both axes. */
let panState = null;
viewer.addEventListener('pointerdown', (e) => {
  if (!viewer.classList.contains('size-actual') || maskEdit) return;
  if (e.target.closest('.btn')) return;
  panState = { x: e.clientX, y: e.clientY, sl: viewer.scrollLeft, st: viewer.scrollTop };
  viewer.classList.add('panning');
  try { viewer.setPointerCapture(e.pointerId); } catch {}
});
viewer.addEventListener('pointermove', (e) => {
  if (!panState) return;
  viewer.scrollLeft = panState.sl - (e.clientX - panState.x);
  viewer.scrollTop = panState.st - (e.clientY - panState.y);
});
viewer.addEventListener('pointerup', () => { panState = null; viewer.classList.remove('panning'); });
viewer.addEventListener('pointercancel', () => { panState = null; viewer.classList.remove('panning'); });

/* ---- fullscreen ----------------------------------------------------- */

const canvasStack = $('canvas-stack');

function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else canvasStack.requestFullscreen().catch(() => {});
}

$('btn-fullscreen').addEventListener('click', toggleFullscreen);
canvasStack.addEventListener('dblclick', () => { if (!maskEdit) toggleFullscreen(); });

/* ---- session persistence -------------------------------------------- */

/* The media file lives in IndexedDB (blobs can't go in localStorage); the
 * layer stack — paths, params, custom-shader sources, masks as data URLs —
 * lives in localStorage and is restored on boot. */

const SESSION_KEY = 'slangfx-web.session';

function idbOpen() {
  return new Promise((resolve, reject) => {
    const r = indexedDB.open('slangfx-web', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('media');
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

async function idbSet(key, val) {
  const db = await idbOpen();
  return new Promise((resolve, reject) => {
    const tx = db.transaction('media', 'readwrite');
    tx.objectStore('media').put(val, key);
    tx.oncomplete = resolve;
    tx.onerror = () => reject(tx.error);
  });
}

async function idbGet(key) {
  const db = await idbOpen();
  return new Promise((resolve, reject) => {
    const rq = db.transaction('media', 'readonly').objectStore('media').get(key);
    rq.onsuccess = () => resolve(rq.result);
    rq.onerror = () => reject(rq.error);
  });
}

let saveTimer = null;
function scheduleSave() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveSession, 600);
}

function saveSession() {
  if (!fx) return;
  const layers = fx.layers.map((layer) => {
    const isCustom = layer.path.startsWith(CUSTOM_PREFIX);
    const dir = isCustom ? layer.path.slice(0, -'custom.slangp'.length) : null;
    const params = {};
    const src = layer.runtime?.paramValues ?? layer.savedParams;
    if (src) for (const [k, v] of src) params[k] = v;
    return {
      kind: isCustom ? 'custom' : 'preset',
      path: isCustom ? null : layer.path,
      source: isCustom ? virtualFiles.get(dir + 'custom.slang') : null,
      label: layer.label ?? null,
      savedName: isCustom ? savedNames.get(dir) ?? null : null,
      enabled: layer.enabled,
      params,
      mask: layer.maskState ? {
        dataURL: layer.maskState.source.toDataURL('image/png'),
        opacity: layer.maskState.opacity ?? 1,
        invert: !!layer.maskState.invert,
      } : null,
    };
  });
  try { localStorage.setItem(SESSION_KEY, JSON.stringify({ layers })); } catch {}
}

async function restoreSession() {
  try {
    const file = await idbGet('current');
    if (file) await loadMedia(file, { persist: false });
  } catch {}
  if (!fx.inputTexture) return;

  let sess = null;
  try { sess = JSON.parse(localStorage.getItem(SESSION_KEY)); } catch {}
  if (!sess?.layers?.length) return;

  setStatus('restoring session…');
  for (const l of sess.layers) {
    let idx;
    if (l.kind === 'custom') {
      const dir = newCustomLayerFiles(l.source ?? CUSTOM_BOILERPLATE);
      if (l.savedName) savedNames.set(dir, l.savedName);
      idx = await fx.addLayer(dir + 'custom.slangp',
        { label: l.label ?? `custom shader ${dir.split('/')[1]}` });
    } else {
      idx = await fx.addLayer(l.path, l.label ? { label: l.label } : {});
    }
    for (const [k, v] of Object.entries(l.params ?? {})) fx.setParam(idx, k, v);
    if (l.mask?.dataURL) {
      const img = new Image();
      await new Promise((resolve) => { img.onload = resolve; img.onerror = resolve; img.src = l.mask.dataURL; });
      const c = document.createElement('canvas');
      c.width = fx.inputW;
      c.height = fx.inputH;
      c.getContext('2d').drawImage(img, 0, 0, c.width, c.height);
      await fx.setLayerMask(idx, c, { opacity: l.mask.opacity, invert: l.mask.invert });
    }
    if (l.enabled === false) await fx.toggleLayer(idx, false);
  }
  renderLayerPanel();
  setStatus('session restored');
}

/* ---- mask painting ------------------------------------------------- */

/* Each masked layer owns a white/black mask canvas (the engine samples its
 * red channel: white = effect visible). While editing, the visible overlay
 * canvas shows a red "rubylith" wherever the effect is hidden; strokes are
 * stamped into both in parallel and streamed to the GPU. */

const maskOverlay = $('mask-overlay');
const brush = { size: 60, soft: 0.5, mode: 'hide', tool: 'brush' }; // media-space px
let maskEdit = null;   // { layerIdx } | null
let gradState = null;  // { from: [x, y] } while dragging a gradient

function makeMaskCanvas() {
  const c = document.createElement('canvas');
  c.width = fx.inputW;
  c.height = fx.inputH;
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, c.width, c.height);
  return c;
}

/* Rebuild the rubylith overlay from a mask canvas (edit-start / clear). */
function rebuildRuby(maskCanvas) {
  maskOverlay.width = maskCanvas.width;
  maskOverlay.height = maskCanvas.height;
  const rctx = maskOverlay.getContext('2d');
  const mctx = maskCanvas.getContext('2d');
  const img = mctx.getImageData(0, 0, maskCanvas.width, maskCanvas.height);
  const out = rctx.createImageData(img.width, img.height);
  for (let i = 0; i < img.data.length; i += 4) {
    out.data[i] = 255;                       // red
    out.data[i + 3] = 255 - img.data[i];     // alpha = hidden amount
  }
  rctx.putImageData(out, 0, 0);
}

function stampBrush(ctx, x, y, erase) {
  const r = Math.max(brush.size / 2, 1);
  const g = ctx.createRadialGradient(x, y, r * (1 - brush.soft), x, y, r);
  ctx.save();
  if (erase) {
    ctx.globalCompositeOperation = 'destination-out';
    g.addColorStop(0, 'rgba(255,255,255,1)');
    g.addColorStop(1, 'rgba(255,255,255,0)');
  } else {
    g.addColorStop(0, ctx._stampColor + '1)');
    g.addColorStop(1, ctx._stampColor + '0)');
  }
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(x, y, r, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

function maskStroke(x, y) {
  const layer = fx.layers[maskEdit.layerIdx];
  if (!layer?.maskState) return;
  const mctx = layer.maskState.source.getContext('2d');
  const rctx = maskOverlay.getContext('2d');
  if (brush.mode === 'hide') {
    mctx._stampColor = 'rgba(0,0,0,';       // hide effect → mask black
    stampBrush(mctx, x, y, false);
    rctx._stampColor = 'rgba(255,0,0,';     // rubylith red
    stampBrush(rctx, x, y, false);
  } else {
    mctx._stampColor = 'rgba(255,255,255,'; // reveal effect → mask white
    stampBrush(mctx, x, y, false);
    stampBrush(rctx, x, y, true);           // erase rubylith
  }
  fx.updateLayerMask(maskEdit.layerIdx);
}

/* Photoshop-style gradient fill: replaces the whole mask with a ramp from
 * the drag start to the drag end (linear) or outward from the start
 * (radial). Hide mode ramps hidden → visible; Show mode the reverse. */
function applyGradient(from, to) {
  const layer = fx.layers[maskEdit.layerIdx];
  if (!layer?.maskState) return;
  if (Math.hypot(to[0] - from[0], to[1] - from[1]) < 2) return;
  const src = layer.maskState.source;
  const hide = brush.mode === 'hide';

  const ramp = (ctx, c0, c1) => {
    let g;
    if (brush.tool === 'radial') {
      const r = Math.max(Math.hypot(to[0] - from[0], to[1] - from[1]), 2);
      g = ctx.createRadialGradient(from[0], from[1], 0, from[0], from[1], r);
    } else {
      g = ctx.createLinearGradient(from[0], from[1], to[0], to[1]);
    }
    g.addColorStop(0, c0);
    g.addColorStop(1, c1);
    return g;
  };

  const mctx = src.getContext('2d');
  mctx.save();
  mctx.globalCompositeOperation = 'source-over';
  mctx.fillStyle = ramp(mctx, hide ? '#000' : '#fff', hide ? '#fff' : '#000');
  mctx.fillRect(0, 0, src.width, src.height);
  mctx.restore();

  const rctx = maskOverlay.getContext('2d');
  rctx.save();
  rctx.globalCompositeOperation = 'source-over';
  rctx.clearRect(0, 0, maskOverlay.width, maskOverlay.height);
  rctx.fillStyle = ramp(rctx,
    hide ? 'rgba(255,0,0,1)' : 'rgba(255,0,0,0)',
    hide ? 'rgba(255,0,0,0)' : 'rgba(255,0,0,1)');
  rctx.fillRect(0, 0, maskOverlay.width, maskOverlay.height);
  rctx.restore();

  fx.updateLayerMask(maskEdit.layerIdx);
}

function overlayToMedia(e) {
  const rect = maskOverlay.getBoundingClientRect();
  return [
    (e.clientX - rect.left) / rect.width * fx.inputW,
    (e.clientY - rect.top) / rect.height * fx.inputH,
  ];
}

let painting = false;
let lastPt = null;

maskOverlay.addEventListener('pointerdown', (e) => {
  if (!maskEdit) return;
  try { maskOverlay.setPointerCapture(e.pointerId); } catch {}
  if (brush.tool === 'brush') {
    painting = true;
    lastPt = overlayToMedia(e);
    maskStroke(...lastPt);
  } else {
    gradState = { from: overlayToMedia(e) };
  }
});
maskOverlay.addEventListener('pointermove', (e) => {
  if (!maskEdit) return;
  if (gradState) {
    applyGradient(gradState.from, overlayToMedia(e));   // live preview
    return;
  }
  if (!painting) return;
  const pt = overlayToMedia(e);
  // Stamp along the segment so fast strokes stay solid.
  const step = Math.max(brush.size / 4, 2);
  const d = Math.hypot(pt[0] - lastPt[0], pt[1] - lastPt[1]);
  const n = Math.max(1, Math.ceil(d / step));
  for (let i = 1; i <= n; i++)
    maskStroke(lastPt[0] + (pt[0] - lastPt[0]) * i / n, lastPt[1] + (pt[1] - lastPt[1]) * i / n);
  lastPt = pt;
});
maskOverlay.addEventListener('pointerup', (e) => {
  if (gradState && maskEdit) applyGradient(gradState.from, overlayToMedia(e));
  painting = false;
  lastPt = null;
  gradState = null;
  scheduleSave();
});

async function startMaskEdit(layerIdx) {
  const layer = fx.layers[layerIdx];
  if (!layer) return;
  // Cover crops the canvas box, which would skew brush coordinates.
  if (viewer.classList.contains('size-cover')) setViewMode('fit');
  if (!layer.maskState) {
    await fx.setLayerMask(layerIdx, makeMaskCanvas());
    renderLayerPanel();
  }
  maskEdit = { layerIdx };
  rebuildRuby(layer.maskState.source);
  document.body.classList.add('mask-editing');
  setStatus('painting mask — red = effect hidden');
  renderLayerPanel();
}

function stopMaskEdit() {
  maskEdit = null;
  painting = false;
  document.body.classList.remove('mask-editing');
  renderLayerPanel();
}

/* Rescale mask canvases when the media (and so the chain dims) changes. */
function rescaleMasks() {
  fx.layers.forEach((layer, i) => {
    const src = layer.maskState?.source;
    if (!src || (src.width === fx.inputW && src.height === fx.inputH)) return;
    const scaled = document.createElement('canvas');
    scaled.width = fx.inputW;
    scaled.height = fx.inputH;
    scaled.getContext('2d').drawImage(src, 0, 0, scaled.width, scaled.height);
    layer.maskState.source = scaled;
    fx.updateLayerMask(i);
  });
}

/* ---- layer stack --------------------------------------------------- */

/* ---- add-layer dropdown: folders + type-ahead search ---------------- */

const openFolders = new Set();   // category ids expanded this session

function closeAddMenu() {
  addLayerList.classList.remove('open');
  addLayerSearch.value = '';
}

function openAddMenu() {
  addLayerList.classList.add('open');
  rebuildAddMenu();
}

addLayerSearch.addEventListener('focus', openAddMenu);

document.addEventListener('pointerdown', (e) => {
  if (addLayerList.classList.contains('open') && !$('add-section').contains(e.target)) closeAddMenu();
});

// "/" jumps to the search from anywhere (unless already typing somewhere).
document.addEventListener('keydown', (e) => {
  if (e.key !== '/' || e.target.matches?.('input, textarea')) return;
  e.preventDefault();
  addLayerSearch.focus();
});

addLayerSearch.addEventListener('input', () => {
  addLayerList.classList.add('open');
  rebuildAddMenu();
});
addLayerSearch.addEventListener('keydown', (e) => {
  e.stopPropagation();
  if (e.key === 'Escape') { closeAddMenu(); addLayerSearch.blur(); }
  else if (e.key === 'Enter') addLayerList.querySelector('.menu-item')?.click();
});

function categoryLabel(id) {
  return (manifest.categories ?? []).find((c) => c.id === id)?.label ?? id;
}

function rebuildAddMenu() {
  const q = addLayerSearch.value.trim().toLowerCase();
  addLayerList.replaceChildren();
  const savedList = Object.keys(loadSaved()).sort();

  const addItem = (label, onPick, note = null) => {
    const it = document.createElement('div');
    it.className = 'menu-item';
    const span = document.createElement('span');
    span.textContent = label;
    it.appendChild(span);
    if (note) {
      const n = document.createElement('span');
      n.className = 'note';
      n.textContent = note;
      it.appendChild(n);
    }
    it.addEventListener('click', () => { closeAddMenu(); onPick(); });
    addLayerList.appendChild(it);
  };

  if (q) {
    // Searching: collapse everything to one flat, filtered list.
    if ('custom shader write your own'.includes(q))
      addItem('✎ custom shader', () => addChoice('__custom__'));
    for (const name of savedList)
      if (name.toLowerCase().includes(q))
        addItem(`🗎 ${name}`, () => addChoice(`__saved__:${name}`), 'saved');
    for (const eff of manifest.effects) {
      const cat = categoryLabel(eff.category);
      if (eff.name.toLowerCase().includes(q) || cat.toLowerCase().includes(q))
        addItem(eff.name, () => addChoice(eff.path), cat);
    }
    if (!addLayerList.children.length) {
      const none = document.createElement('div');
      none.className = 'menu-empty';
      none.textContent = 'no matches';
      addLayerList.appendChild(none);
    }
    return;
  }

  // Browsing: pinned custom entry, then collapsible folders.
  addItem('✎ custom shader (write your own)', () => addChoice('__custom__'));

  const folder = (id, label, children) => {
    if (!children.length) return;
    const open = openFolders.has(id);
    const head = document.createElement('div');
    head.className = 'menu-folder';
    const title = document.createElement('span');
    title.textContent = `${open ? '▾' : '▸'} ${label}`;
    const count = document.createElement('span');
    count.className = 'note';
    count.textContent = String(children.length);
    head.append(title, count);
    head.addEventListener('click', () => {
      if (open) openFolders.delete(id);
      else openFolders.add(id);
      rebuildAddMenu();
    });
    addLayerList.appendChild(head);
    if (open) for (const c of children) addItem(c.label, c.onPick, c.note);
  };

  folder('saved', 'Saved shaders',
    savedList.map((name) => ({ label: `🗎 ${name}`, onPick: () => addChoice(`__saved__:${name}`) })));
  for (const cat of manifest.categories ?? [])
    folder(cat.id, cat.label,
      manifest.effects
        .filter((e) => e.category === cat.id)
        .map((e) => ({ label: e.name, onPick: () => addChoice(e.path) })));
}

function populateSavedOptions() {
  if (addLayerList.classList.contains('open')) rebuildAddMenu();
}

async function addChoice(choice) {
  if (!choice || !fx) return;
  if (!fx.inputTexture) { setStatus('load a video or image first'); return; }
  let path = choice;
  let opts = {};
  if (choice === '__custom__') {
    const dir = newCustomLayerFiles();
    path = dir + 'custom.slangp';
    opts = { label: `custom shader ${dir.split('/')[1]}` };
  } else if (choice.startsWith('__saved__:')) {
    const name = choice.slice('__saved__:'.length);
    const saves = loadSaved();
    if (!saves[name]) return;
    const dir = newCustomLayerFiles(saves[name].source);
    savedNames.set(dir, name);
    path = dir + 'custom.slangp';
    opts = { label: name };
  }
  setStatus(`compiling ${opts.label ?? path}…`);
  const t0 = performance.now();
  await fx.addLayer(path, opts);
  const info = fx.getLayerInfo();
  const last = info[info.length - 1];
  setStatus(last.error ? `layer failed: ${last.error}` : `added ${last.name} in ${Math.round(performance.now() - t0)} ms`);
  renderLayerPanel();
}

/* Unsaved editor text per custom layer (committed on Compile, so rebuilds
 * triggered by other actions keep running the last compiled version). */
const editorDrafts = new Map(); // slangPath -> draft text

/** Recompile a custom layer from its editor text. */
async function compileCustomLayer(layerIndex, slangPath, source) {
  virtualFiles.set(slangPath, source);
  fx.invalidateModules(slangPath);
  setStatus('compiling custom shader…');
  const t0 = performance.now();
  await fx.rebuild();
  const info = fx.getLayerInfo()[layerIndex];
  setStatus(info.error ? 'custom shader failed to compile — see layer'
                       : `custom shader compiled in ${Math.round(performance.now() - t0)} ms`);
  renderLayerPanel();
}

function renderLayerPanel() {
  if (!fx) return;
  scheduleSave();
  const infos = fx.getLayerInfo();
  layersEl.replaceChildren();
  infos.forEach((info) => {
    const div = document.createElement('div');
    div.className = 'layer' + (info.error ? ' error' : '');

    const head = document.createElement('div');
    head.className = 'layer-head';

    const toggle = document.createElement('input');
    toggle.type = 'checkbox';
    toggle.checked = info.enabled;
    toggle.title = 'bypass';
    toggle.onchange = async () => { stopMaskEdit(); await fx.toggleLayer(info.index, toggle.checked); renderLayerPanel(); };

    const up = document.createElement('button');
    up.textContent = '▲';
    up.disabled = info.index === 0;
    up.onclick = async () => { stopMaskEdit(); await fx.moveLayer(info.index, -1); renderLayerPanel(); };

    const down = document.createElement('button');
    down.textContent = '▼';
    down.disabled = info.index === infos.length - 1;
    down.onclick = async () => { stopMaskEdit(); await fx.moveLayer(info.index, +1); renderLayerPanel(); };

    const del = document.createElement('button');
    del.textContent = '✕';
    del.onclick = async () => { stopMaskEdit(); await fx.removeLayer(info.index); renderLayerPanel(); };

    const maskBtn = document.createElement('button');
    maskBtn.textContent = '▦';
    maskBtn.title = info.mask ? 'edit mask' : 'add a mask (paint where the effect should NOT apply)';
    maskBtn.style.color = maskEdit?.layerIdx === info.index ? 'var(--accent)'
      : info.mask ? 'var(--fg)' : '';
    maskBtn.onclick = () => {
      if (maskEdit?.layerIdx === info.index) stopMaskEdit();
      else startMaskEdit(info.index);
    };

    const name = document.createElement('span');
    name.className = 'name' + (info.enabled ? '' : ' off');
    name.textContent = `${info.index}. ${info.name}` + (info.mask ? ' ▦' : '');

    head.append(toggle, up, down, del, maskBtn, name);
    div.appendChild(head);

    if (maskEdit?.layerIdx === info.index && info.mask) {
      const mc = document.createElement('div');
      mc.className = 'mask-controls';

      const toolBtn = (tool, icon, title) => {
        const b = document.createElement('button');
        b.className = 'btn' + (brush.tool === tool ? ' active' : '');
        b.textContent = icon;
        b.title = title;
        b.onclick = () => { brush.tool = tool; renderLayerPanel(); };
        return b;
      };
      const tools = [
        toolBtn('brush', '🖌', 'brush'),
        toolBtn('linear', '▤', 'linear gradient — drag across the preview'),
        toolBtn('radial', '◎', 'radial gradient — drag outward from the center point'),
      ];

      const modeHide = document.createElement('button');
      modeHide.className = 'btn' + (brush.mode === 'hide' ? ' active' : '');
      modeHide.textContent = 'Hide fx';
      modeHide.onclick = () => { brush.mode = 'hide'; renderLayerPanel(); };
      const modeShow = document.createElement('button');
      modeShow.className = 'btn' + (brush.mode === 'show' ? ' active' : '');
      modeShow.textContent = 'Show fx';
      modeShow.onclick = () => { brush.mode = 'show'; renderLayerPanel(); };

      const isBrush = brush.tool === 'brush';
      const sizeLabel = document.createElement('label');
      sizeLabel.textContent = 'size';
      const size = document.createElement('input');
      size.type = 'range';
      size.min = '8'; size.max = '300'; size.value = String(brush.size);
      size.disabled = !isBrush;
      size.oninput = () => { brush.size = parseFloat(size.value); };
      sizeLabel.appendChild(size);

      const softLabel = document.createElement('label');
      softLabel.textContent = 'soft';
      const soft = document.createElement('input');
      soft.type = 'range';
      soft.min = '0'; soft.max = '0.9'; soft.step = '0.05'; soft.value = String(brush.soft);
      soft.disabled = !isBrush;
      soft.oninput = () => { brush.soft = parseFloat(soft.value); };
      softLabel.appendChild(soft);

      const invLabel = document.createElement('label');
      const inv = document.createElement('input');
      inv.type = 'checkbox';
      inv.checked = info.mask.invert;
      inv.onchange = () => { fx.setLayerMaskOptions(info.index, { invert: inv.checked }); scheduleSave(); };
      invLabel.append(inv, 'invert');

      const opLabel = document.createElement('label');
      opLabel.textContent = 'opacity';
      const op = document.createElement('input');
      op.type = 'range';
      op.min = '0'; op.max = '1'; op.step = '0.01'; op.value = String(info.mask.opacity);
      op.oninput = () => { fx.setLayerMaskOptions(info.index, { opacity: parseFloat(op.value) }); scheduleSave(); };
      opLabel.appendChild(op);

      const clear = document.createElement('button');
      clear.className = 'btn';
      clear.textContent = 'Clear';
      clear.onclick = () => {
        const src = fx.layers[info.index].maskState.source;
        const ctx2 = src.getContext('2d');
        ctx2.globalCompositeOperation = 'source-over';
        ctx2.fillStyle = '#fff';
        ctx2.fillRect(0, 0, src.width, src.height);
        maskOverlay.getContext('2d').clearRect(0, 0, maskOverlay.width, maskOverlay.height);
        fx.updateLayerMask(info.index);
      };

      const remove = document.createElement('button');
      remove.className = 'btn';
      remove.textContent = 'Remove';
      remove.onclick = async () => { stopMaskEdit(); await fx.clearLayerMask(info.index); renderLayerPanel(); };

      const done = document.createElement('button');
      done.className = 'btn';
      done.textContent = 'Done';
      done.onclick = () => stopMaskEdit();

      mc.append(...tools, modeHide, modeShow, sizeLabel, softLabel, invLabel, opLabel, clear, remove, done);
      div.appendChild(mc);
    }

    if (info.error) {
      const err = document.createElement('div');
      err.className = 'layer-error';
      err.textContent = info.error;
      div.appendChild(err);
    }

    if (info.path.startsWith(CUSTOM_PREFIX)) {
      const slangPath = info.path.replace(/custom\.slangp$/, 'custom.slang');
      const editor = document.createElement('div');
      editor.className = 'layer-editor';

      const ta = document.createElement('textarea');
      ta.spellcheck = false;
      ta.value = editorDrafts.get(slangPath) ?? virtualFiles.get(slangPath) ?? '';
      ta.addEventListener('input', () => editorDrafts.set(slangPath, ta.value));
      // Keep keystrokes (incl. Tab-as-indent) inside the editor.
      ta.addEventListener('keydown', (e) => {
        e.stopPropagation();
        if (e.key === 'Tab') {
          e.preventDefault();
          const { selectionStart: s, selectionEnd: t } = ta;
          ta.value = ta.value.slice(0, s) + '    ' + ta.value.slice(t);
          ta.selectionStart = ta.selectionEnd = s + 4;
          editorDrafts.set(slangPath, ta.value);
        }
      });

      const dir = info.path.slice(0, -'custom.slangp'.length);

      const row = document.createElement('div');
      row.className = 'editor-actions';
      const compile = document.createElement('button');
      compile.className = 'btn';
      compile.textContent = 'Compile';
      compile.onclick = () => compileCustomLayer(info.index, slangPath, ta.value);
      const revert = document.createElement('button');
      revert.className = 'btn';
      revert.textContent = 'Revert';
      revert.title = 'discard edits since last compile';
      revert.onclick = () => {
        editorDrafts.delete(slangPath);
        ta.value = virtualFiles.get(slangPath) ?? '';
      };

      const nameInput = document.createElement('input');
      nameInput.type = 'text';
      nameInput.className = 'save-name';
      nameInput.placeholder = 'name…';
      nameInput.value = savedNames.get(dir) ?? '';
      nameInput.addEventListener('keydown', (e) => e.stopPropagation());

      const save = document.createElement('button');
      save.className = 'btn';
      save.textContent = 'Save';
      save.title = 'save this shader to the browser (localStorage); it appears under "saved shaders" in the Add layer menu';
      save.onclick = () => {
        const name = nameInput.value.trim();
        if (!name) { setStatus('give the shader a name to save it'); nameInput.focus(); return; }
        const saves = loadSaved();
        saves[name] = { source: ta.value, savedAt: new Date().toISOString() };
        storeSaved(saves);
        savedNames.set(dir, name);
        setStatus(`saved '${name}' to this browser`);
      };

      const forget = document.createElement('button');
      forget.className = 'btn';
      forget.textContent = 'Forget';
      forget.title = 'delete this saved shader from localStorage (the layer keeps running)';
      forget.hidden = !savedNames.get(dir);
      forget.onclick = () => {
        const name = savedNames.get(dir);
        if (!name) return;
        const saves = loadSaved();
        delete saves[name];
        storeSaved(saves);
        savedNames.delete(dir);
        setStatus(`forgot saved shader '${name}'`);
        renderLayerPanel();
      };

      row.append(compile, revert, nameInput, save, forget);
      editor.append(ta, row);
      div.appendChild(editor);
    }

    if (info.enabled && info.params.length) {
      const params = document.createElement('div');
      params.className = 'layer-params';
      for (const p of info.params) {
        const row = document.createElement('div');
        row.className = 'param-row';

        const label = document.createElement('label');
        label.textContent = p.desc || p.name;
        label.title = p.name;

        const slider = document.createElement('input');
        slider.type = 'range';
        slider.min = String(p.min);
        slider.max = String(p.max);
        slider.step = String(p.step || 0.001);
        slider.value = String(p.value);

        const fmt = (v) => (+v).toFixed(3).replace(/\.?0+$/, '') || '0';
        const val = document.createElement('input');
        val.type = 'number';
        val.className = 'val';
        val.min = String(p.min);
        val.max = String(p.max);
        val.step = String(p.step || 0.001);
        val.value = fmt(p.value);
        val.addEventListener('keydown', (e) => e.stopPropagation());
        val.addEventListener('input', () => {
          const v = parseFloat(val.value);
          if (Number.isNaN(v)) return;
          fx.setParam(info.index, p.name, v);
          slider.value = String(v);
          scheduleSave();
        });
        val.addEventListener('change', () => {
          // Normalize display to the engine's clamped value on commit.
          const cur = fx.getLayerInfo()[info.index]?.params.find((q) => q.name === p.name);
          if (cur) { val.value = fmt(cur.value); slider.value = String(cur.value); }
        });

        slider.oninput = () => {
          const v = parseFloat(slider.value);
          fx.setParam(info.index, p.name, v);
          val.value = fmt(v);
          scheduleSave();
        };

        row.append(label, slider, val);
        params.appendChild(row);
      }
      div.appendChild(params);
    }
    layersEl.appendChild(div);
  });
}

$('btn-reset').addEventListener('click', () => {
  if (!fx) return;
  fx.layers.forEach((_, i) => fx.resetParams(i));
  renderLayerPanel();
});

$('btn-copy-params').addEventListener('click', () => {
  if (!fx) return;
  const infos = fx.getLayerInfo().filter((l) => l.enabled && !l.error);
  const lines = infos.map((l) => {
    const kv = l.params.map((p) => `${p.name}=${+(+p.value).toPrecision(6)}`).join(',');
    return infos.length > 1 ? `${l.name}: ${kv}` : kv;
  });
  navigator.clipboard.writeText(lines.join('\n'));
  setStatus('params copied to clipboard');
});

/* ---- export -------------------------------------------------------- */

$('btn-export-png').addEventListener('click', async () => {
  if (!fx || !fx.inputTexture) return;
  const blob = await fx.exportPNG();
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'slangfx-frame.png';
  a.click();
  setStatus('frame exported');
});

$('btn-export-webm').addEventListener('click', () => {
  const btn = $('btn-export-webm');
  if (recorder) {
    recorder.stop();
    return;
  }
  if (!fx || !fx.inputTexture) return;
  const stream = canvas.captureStream(30);

  // Mix the source video's audio into the recording (spec: element capture
  // audio is pre-volume/mute, so recordings get sound even while previewing
  // muted — matching the native exporter, which copies the audio track).
  if (mediaKind === 'video' && video.readyState >= 2) {
    try {
      elementCapture ??= video.captureStream();
      for (const t of elementCapture.getAudioTracks()) stream.addTrack(t);
    } catch (e) {
      console.warn('slangfx: could not capture video audio for recording:', e);
    }
  }

  const withAudio = stream.getAudioTracks().length > 0;
  const mime = withAudio && MediaRecorder.isTypeSupported('video/webm;codecs=vp9,opus')
    ? 'video/webm;codecs=vp9,opus'
    : 'video/webm;codecs=vp9';
  recorder = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: 12_000_000 });
  const chunks = [];
  recorder.ondataavailable = (e) => { if (e.data.size) chunks.push(e.data); };
  recorder.onstop = () => {
    const blob = new Blob(chunks, { type: 'video/webm' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'slangfx-capture.webm';
    a.click();
    recorder = null;
    btn.textContent = 'Record WebM';
    btn.classList.remove('recording');
    setStatus('recording saved');
  };
  recorder.start();
  btn.textContent = '■ Stop';
  btn.classList.add('recording');
  setStatus('recording…');
});

boot();
