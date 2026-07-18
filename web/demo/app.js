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
const effectSelect = $('effect-select');

const VIDEO_EXTS = /\.(mp4|mov|mkv|webm|avi|m4v|gif)$/i;

let fx = null;
let mediaKind = null;      // 'video' | 'image' | null
let imageBitmap = null;
let imageDirty = false;
let scrubbing = false;
let recorder = null;

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
    const manifest = await (await fetch('effects.json')).json();
    for (const eff of manifest.effects) {
      const opt = document.createElement('option');
      opt.value = eff.path;
      opt.textContent = eff.name;
      effectSelect.appendChild(opt);
    }
  } catch {
    setStatus('effects.json missing — run: node web/tools/build-manifest.mjs');
  }

  setStatus('ready — load a video or image');
  window.fx = fx; // console/debug access
  requestAnimationFrame(tick);
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

async function loadMedia(file) {
  const url = URL.createObjectURL(file);
  const isVideo = file.type.startsWith('video/') || VIDEO_EXTS.test(file.name);
  document.body.classList.add('has-media');

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
    const seekable = Number.isFinite(video.duration) && video.duration > 0;
    scrub.disabled = !seekable;
    scrub.max = seekable ? String(video.duration) : '0';
    playBtn.disabled = false;
    playBtn.textContent = 'Pause';
    await video.play().catch(() => {});
    setStatus(`${file.name} — ${video.videoWidth}×${video.videoHeight}`);
  } else {
    mediaKind = 'image';
    video.pause();
    video.removeAttribute('src');
    imageBitmap = await createImageBitmap(file);
    canvas.width = imageBitmap.width;
    canvas.height = imageBitmap.height;
    await fx.setSourceSize(imageBitmap.width, imageBitmap.height);
    imageDirty = true;
    scrub.disabled = true;
    playBtn.disabled = true;
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

scrub.addEventListener('input', () => {
  scrubbing = true;
  timeLabel.textContent = `${fmtTime(parseFloat(scrub.value))} / ${fmtTime(video.duration)}`;
});
scrub.addEventListener('change', () => {
  video.currentTime = parseFloat(scrub.value);
  scrubbing = false;
});

/* ---- layer stack --------------------------------------------------- */

effectSelect.addEventListener('change', async () => {
  const path = effectSelect.value;
  effectSelect.value = '';
  if (!path || !fx) return;
  if (!fx.inputTexture) { setStatus('load a video or image first'); return; }
  setStatus(`compiling ${path}…`);
  const t0 = performance.now();
  await fx.addLayer(path);
  const info = fx.getLayerInfo();
  const last = info[info.length - 1];
  setStatus(last.error ? `layer failed: ${last.error}` : `added ${last.name} in ${Math.round(performance.now() - t0)} ms`);
  renderLayerPanel();
});

function renderLayerPanel() {
  if (!fx) return;
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
    toggle.onchange = async () => { await fx.toggleLayer(info.index, toggle.checked); renderLayerPanel(); };

    const up = document.createElement('button');
    up.textContent = '▲';
    up.disabled = info.index === 0;
    up.onclick = async () => { await fx.moveLayer(info.index, -1); renderLayerPanel(); };

    const down = document.createElement('button');
    down.textContent = '▼';
    down.disabled = info.index === infos.length - 1;
    down.onclick = async () => { await fx.moveLayer(info.index, +1); renderLayerPanel(); };

    const del = document.createElement('button');
    del.textContent = '✕';
    del.onclick = async () => { await fx.removeLayer(info.index); renderLayerPanel(); };

    const name = document.createElement('span');
    name.className = 'name' + (info.enabled ? '' : ' off');
    name.textContent = `${info.index}. ${info.name}`;

    head.append(toggle, up, down, del, name);
    div.appendChild(head);

    if (info.error) {
      const err = document.createElement('div');
      err.className = 'layer-error';
      err.textContent = info.error;
      div.appendChild(err);
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

        const val = document.createElement('span');
        val.className = 'val';
        val.textContent = (+p.value).toFixed(3).replace(/\.?0+$/, '') || '0';

        slider.oninput = () => {
          const v = parseFloat(slider.value);
          fx.setParam(info.index, p.name, v);
          val.textContent = v.toFixed(3).replace(/\.?0+$/, '') || '0';
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
  recorder = new MediaRecorder(stream, { mimeType: 'video/webm;codecs=vp9', videoBitsPerSecond: 12_000_000 });
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
