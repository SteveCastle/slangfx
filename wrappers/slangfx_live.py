#!/usr/bin/env python3
"""slangfx_live.py — real-time shader-param playground.

Streams a video — or loops a still image — through a stack of slang presets
(layers) and shows the result in a window with one slider per
`#pragma parameter`, grouped per layer. Dragging a slider sends a live
`name=value` update to that layer over UDP (localhost), applied on the next
frame with no pipeline rebuild. Layers can be added, removed, reordered, and
bypassed from the Layers panel; the whole stack runs inside ONE slangfx
process, chained on the GPU (layer N sees layer N-1's output, no intermediate
readback — fast enough for realtime streaming). The menu bar
switches shader or video on the fly (the chain is restarted and the sliders
rebuilt). Tuned a look you like? "Copy params" gives per-layer `k=v,k=v`
strings for `slangfx --params` / `beat_cut --shader-params`, or "Export"
renders the current stack to an H.264 file at the source's native
resolution/fps with the original audio copied. A still image previews as an
endless loop (time-based shader animation keeps running) and exports as an
--image-duration clip at --fps, no audio.

Pipeline:  ffmpeg (decode+loop+letterbox) -> slangfx per layer -> app
Controls:  app -> UDP name=value -> the layer's control port
Menu:      Shader (replace / Add layer) / Video / Params / Export

Both -i/--input and --preset are optional: start empty and load them from the
menus, or pass a video with no preset to play it raw (no shader) until you pick
one. The Shader/Video menus list files discovered under --shaders-dir/
--videos-dir (defaulting sensibly), plus a Browse... dialog. On Windows you
can also drag media files (or a .slangp) from Explorer onto the window.

Usage (from slangfx's own venv — create with `python -m venv .venv`, then
install deps with `.venv/Scripts/python -m pip install -r wrappers/requirements.txt`):
    python wrappers/slangfx_live.py                       # empty; load from menus
    python wrappers/slangfx_live.py -i my_clip.mp4        # raw video, no shader
    python wrappers/slangfx_live.py -i my_clip.mp4 --preset path/to/effect.slangp

Headless self-test (no window): verifies live control + a shader switch.
    python wrappers/slangfx_live.py -i my_clip.mp4 --preset path/to/effect.slangp --selftest
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

VIDEO_EXTS = (".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".gif")
IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tif", ".tiff")
MEDIA_EXTS = VIDEO_EXTS + IMAGE_EXTS

FROZEN = getattr(sys, "frozen", False)   # running as a PyInstaller bundle

# In the windowed (--noconsole) PyInstaller build every child process would
# otherwise open its own console window — a cmd window flashing per pipeline
# restart. Dev runs keep the shared console (and ffmpeg's stderr with it).
_NOWIN = ({"creationflags": subprocess.CREATE_NO_WINDOW}
          if os.name == "nt" and FROZEN else {})
_sp_popen, _sp_check_output = subprocess.Popen, subprocess.check_output


def _popen(*a, **kw):
    return _sp_popen(*a, **{**_NOWIN, **kw})


def _check_output(*a, **kw):
    return _sp_check_output(*a, **{**_NOWIN, **kw})


def app_dir():
    """Directory holding the running app: the exe's folder when frozen, else
    this script's folder."""
    if FROZEN:
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def is_image(path):
    return bool(path) and path.lower().endswith(IMAGE_EXTS)


# --------------------------------------------------------------------------
# Shader parameter discovery
# --------------------------------------------------------------------------
_PRAGMA = re.compile(
    r'#pragma\s+parameter\s+(\w+)\s+"([^"]*)"\s+'
    r'(-?\d+\.?\d*)\s+(-?\d+\.?\d*)\s+(-?\d+\.?\d*)\s+(-?\d+\.?\d*)')


def _preset_shader_files(preset_path):
    """Return the .slang files referenced by a .slangp, in pass order."""
    base = os.path.dirname(preset_path)
    files = []
    with open(preset_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.match(r'\s*shader\d+\s*=\s*"?([^"\n]+?)"?\s*$', line)
            if m:
                files.append(os.path.normpath(os.path.join(base, m.group(1).strip())))
    return files


def _preset_default_overrides(preset_path):
    """`name = value` overrides declared directly in the .slangp."""
    overrides = {}
    reserved = ("shader", "alias", "scale", "filter", "wrap", "float_framebuffer",
                "mipmap", "frame_count", "parameters", "textures", "shaders")
    with open(preset_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.match(r'\s*(\w+)\s*=\s*([-\d.]+)\s*$', line)
            if m and not any(m.group(1).startswith(r) for r in reserved):
                try:
                    overrides[m.group(1)] = float(m.group(2))
                except ValueError:
                    pass
    return overrides


def discover_params(preset_path):
    """List of {name,label,default,min,max,step} for every shader param,
    de-duplicated by name (first declaration wins), with .slangp default
    overrides applied."""
    params, seen = [], set()
    for sf in _preset_shader_files(preset_path):
        if not os.path.isfile(sf):
            continue
        with open(sf, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = _PRAGMA.search(line)
                if not m or m.group(1) in seen:
                    continue
                seen.add(m.group(1))
                params.append({
                    "name": m.group(1), "label": m.group(2),
                    "default": float(m.group(3)), "min": float(m.group(4)),
                    "max": float(m.group(5)), "step": float(m.group(6)),
                })
    overrides = _preset_default_overrides(preset_path)
    for p in params:
        if p["name"] in overrides:
            p["default"] = overrides[p["name"]]
    return params


# --------------------------------------------------------------------------
# File discovery for the menus
# --------------------------------------------------------------------------
def find_presets(root):
    """All .slangp presets under `root` (recursive), sorted. Empty if root is
    missing/None."""
    found = []
    if root and os.path.isdir(root):
        for dp, _, files in os.walk(root):
            for f in files:
                if f.endswith(".slangp"):
                    found.append(os.path.join(dp, f))
    return sorted(set(found))


def find_videos(root):
    """Videos and images directly in `root`, sorted. Empty if root is
    missing/None."""
    if not root or not os.path.isdir(root):
        return []
    try:
        names = os.listdir(root)
    except OSError:
        return []
    return sorted(os.path.join(root, f) for f in names
                  if f.lower().endswith(MEDIA_EXTS))


def shaders_root_for(args):
    """Directory to scan for presets: explicit --shaders-dir, else the shaders
    tree holding --preset (its grandparent), else the bundled slangfx/shaders
    next to this script, else the cwd."""
    if args.shaders_dir:
        return args.shaders_dir
    if args.preset:
        return os.path.dirname(os.path.dirname(os.path.abspath(args.preset[0])))
    # Frozen release layout: shaders/ sits next to the exe. Dev layout: next
    # to wrappers/ (the repo root).
    for bundled in (os.path.join(app_dir(), "shaders"),
                    os.path.join(app_dir(), "..", "shaders")):
        if os.path.isdir(bundled):
            return os.path.normpath(bundled)
    return os.getcwd()


def videos_root_for(args):
    """Directory to scan for videos: explicit --videos-dir, else the folder of
    --input, else the cwd."""
    if args.videos_dir:
        return args.videos_dir
    if args.input:
        return os.path.dirname(os.path.abspath(args.input))
    return os.getcwd()


# --------------------------------------------------------------------------
# Pipeline orchestration
# --------------------------------------------------------------------------
def _probe_dims(ffmpeg, path):
    """(w, h) of the source via ffprobe; falls back to 1280x720."""
    ffprobe = ffmpeg.replace("ffmpeg", "ffprobe")
    try:
        out = _check_output(
            [ffprobe, "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", path],
            text=True).strip().splitlines()[0]
        w, h = (int(x) for x in out.split("x")[:2])
        return w, h
    except Exception:
        return 1280, 720


def _probe_duration(ffmpeg, path):
    """Duration in seconds via ffprobe; 0.0 if unknown (disables scrubbing)."""
    ffprobe = ffmpeg.replace("ffmpeg", "ffprobe")
    try:
        out = _check_output(
            [ffprobe, "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", path], text=True).strip()
        return max(0.0, float(out))
    except Exception:
        return 0.0


def fmt_time(t):
    t = max(0, int(t))
    return f"{t // 60}:{t % 60:02d}"


def preview_size(args):
    """Resolve the fixed preview WxH. Every source is letterboxed into this
    size, so it's chosen once: from the launch video's aspect when there is
    one, else a 16:9 default (a video loaded later just letterboxes into it)."""
    if args.width and args.height:
        return args.width, args.height
    if args.input:
        sw, sh = _probe_dims(args.ffmpeg, args.input)
    else:
        sw, sh = 1280, 720
    w = args.width or min(1280, sw)
    h = args.height or max(2, round(w * sh / sw / 2) * 2)
    return int(w), int(h)


class Pipeline:
    """Feeds letterboxed RGBA frames from one (video, layer-stack) pair. All
    enabled layers run inside ONE slangfx process (repeated --preset flags)
    chained on the GPU — layer N's input, including its `Original`, is layer
    N-1's output, with no intermediate readback. Live updates go to a single
    UDP control port; 'i:name=value' routes to layer i. With no layers it's
    ffmpeg alone (raw video). A reader thread keeps only the latest frame so
    the UI never blocks (newest-wins). Switching video or restructuring
    layers builds a fresh Pipeline."""

    def __init__(self, slangfx, ffmpeg, video, layers, w, h, fps, port,
                 realtime=True, start_at=0.0):
        self.w, self.h, self.fb = w, h, w * h * 4
        self.layers = [l for l in layers if l.get("enabled", True)]
        self.port = port
        self.procs = []
        self.latest = None
        self.lock = threading.Lock()
        self.paused = False
        self.running = True
        self.frames = 0
        self.fps = fps
        self.start_at = start_at if not is_image(video) else 0.0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Letterbox any source into the fixed WxH so the texture never resizes.
        vf = (f"scale={w}:{h}:force_original_aspect_ratio=decrease,"
              f"pad={w}:{h}:-1:-1:color=black,setsar=1")
        # `-re` paces ffmpeg to the source's native frame rate (real time);
        # without it ffmpeg floods the pipe and the preview plays many times too
        # fast. Off for the self-test, where we want frames as fast as possible.
        ff_cmd = [ffmpeg, "-loglevel", "error"]
        if is_image(video):
            # Still image: the image2 demuxer's `-loop 1` repeats the single
            # frame forever (`-stream_loop` doesn't loop stills), and
            # `-framerate` gives `-re` a native rate to pace against. Time-based
            # shader animation keeps running because frames keep flowing.
            ff_cmd += ["-loop", "1", "-framerate", str(fps)]
        else:
            ff_cmd += ["-stream_loop", "-1"]
        if realtime:
            ff_cmd += ["-re"]
        if self.start_at > 0:
            # Input-side seek: the first play starts here; when -stream_loop
            # wraps, later loops start from 0 (position() models that).
            ff_cmd += ["-ss", f"{self.start_at:.3f}"]
        ff_cmd += ["-i", video, "-vf", vf,
                   "-f", "rawvideo", "-pix_fmt", "rgba", "-r", str(fps), "-"]
        self.ff = _popen(ff_cmd, stdout=subprocess.PIPE)
        self.procs.append(self.ff)

        src = self.ff.stdout
        if self.layers:
            cmd = [slangfx]
            for layer in self.layers:
                cmd += ["--preset", layer["preset"]]
                startup = ",".join(f"{p['name']}={p['default']}"
                                   for p in layer["params"])
                if startup:
                    cmd += ["--params", startup]
            cmd += ["--width", str(w), "--height", str(h),
                    "--control-port", str(port)]
            try:
                proc = _popen(cmd, stdin=src, stdout=subprocess.PIPE)
            except OSError as e:
                self.close()
                raise RuntimeError(
                    f"could not run slangfx at '{slangfx}' ({e}) — "
                    "build it or pass --slangfx") from e
            src.close()                        # child owns the upstream pipe now
            src = proc.stdout
            self.procs.append(proc)
        self.src = src        # shaded output (or raw video if no layers)

        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        rd = self.src
        while self.running:
            if self.paused:
                # Stop draining the pipe: the OS buffer fills, ffmpeg blocks,
                # and playback genuinely freezes (not just the displayed frame).
                time.sleep(0.05)
                continue
            buf = rd.read(self.fb)
            if not buf or len(buf) < self.fb:
                break
            self.frames += 1
            with self.lock:
                self.latest = buf
        self.running = False

    def get_latest(self):
        with self.lock:
            return self.latest

    def position(self, duration):
        """Current media position (s). Output frames arrive at self.fps
        regardless of source rate, so frames/fps is media time elapsed; the
        modulo folds -stream_loop wraps back into [0, duration)."""
        if duration <= 0:
            return 0.0
        return (self.start_at + self.frames / float(self.fps)) % duration

    def set_param(self, layer, name, value):
        """Send a live k=v update routed to `layer` ('i:name=value' protocol,
        where i is the layer's position among the ENABLED layers in the
        chain). With a single layer, send the un-prefixed form instead — it
        means the same thing and also works with pre-layer slangfx binaries
        (which silently drop prefixed names). No-op for layers not in the
        chain (disabled)."""
        idx = next((i for i, l in enumerate(self.layers) if l is layer), None)
        if idx is None:
            return
        msg = (f"{name}={value}" if len(self.layers) == 1
               else f"{idx}:{name}={value}")
        try:
            self.sock.sendto(msg.encode(), ("127.0.0.1", self.port))
        except OSError:
            pass

    def close(self):
        self.running = False
        for p in reversed(self.procs):
            try:
                p.terminate()
            except Exception:
                pass


def make_layer(preset, params, enabled=True):
    return {"preset": os.path.abspath(preset), "params": params,
            "enabled": enabled}


def make_pipeline(args, video, layers, w, h, port, realtime=True,
                  start_at=0.0):
    return Pipeline(args.slangfx, args.ffmpeg, video, layers,
                    w, h, args.fps, port, realtime=realtime,
                    start_at=start_at)


def parse_params_str(s):
    """'a=1,b=2' -> {'a': 1.0, 'b': 2.0}; tolerant of spaces/empties."""
    out = {}
    for tok in (s or "").replace(";", ",").replace("\n", ",").split(","):
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k.strip()] = float(v)
            except ValueError:
                pass
    return out


# --------------------------------------------------------------------------
# Export — full-resolution render to H.264 with the original audio.
# --------------------------------------------------------------------------
def _probe_wh_fr(ffmpeg, video):
    """(w, h, r_frame_rate) of the source; falls back to 1920x1080@30."""
    ffprobe = ffmpeg.replace("ffmpeg", "ffprobe")
    try:
        out = _check_output(
            [ffprobe, "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height,r_frame_rate",
             "-of", "json", video], text=True)
        s = json.loads(out)["streams"][0]
        return int(s["width"]), int(s["height"]), s.get("r_frame_rate", "30/1")
    except Exception:
        return 1920, 1080, "30/1"


def probe_total_frames(ffmpeg, video):
    """Best-effort total frame count: nb_frames, else duration*fps. 0 if
    unknown (caller treats progress as indeterminate)."""
    ffprobe = ffmpeg.replace("ffmpeg", "ffprobe")
    try:
        out = _check_output(
            [ffprobe, "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=nb_frames,r_frame_rate,duration:format=duration",
             "-of", "json", video], text=True)
        d = json.loads(out)
        s = d.get("streams", [{}])[0]
        nb = s.get("nb_frames", "")
        if nb.isdigit() and int(nb) > 0:
            return int(nb)
        dur = s.get("duration") or d.get("format", {}).get("duration")
        num, _, den = s.get("r_frame_rate", "0/1").partition("/")
        fps = float(num) / float(den or 1) if float(den or 1) else 0.0
        if dur and fps:
            return int(round(float(dur) * fps))
    except Exception:
        pass
    return 0


class Exporter:
    """Runs a full-resolution export in a background thread and exposes
    pollable state: `progress` (0..1, or <0 if total unknown), `status`, `logs`
    (list of lines), `done`, `ok`. The shaded path chains every layer between
    decoder and encoder (ffmpeg | slangfx... | ffmpeg) with `-progress` parsed
    from the encoder; with no layers it's a single transcode. An image
    out_path (.png/.jpg/...) renders ONE processed frame — for a video source,
    the frame at media time `at` (the playhead). Video out_paths copy the
    original audio; res/fps match the source.

    `layers` is a list of {"preset": path, "params_str": "k=v,..."} applied
    in order."""

    def __init__(self, args, video, layers, out_path, at=0.0):
        self.args = args
        self.video = video
        self.layers = layers or []
        self.out_path = out_path
        self.at = at
        self.image_out = out_path.lower().endswith(IMAGE_EXTS)
        self.progress = -1.0
        self.status = "preparing..."
        self.logs = []
        self.done = False
        self.ok = False
        self.total = 0
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def join(self, timeout=None):
        self._thread.join(timeout)

    def _log(self, line):
        line = line.rstrip()
        if line:
            self.logs.append(line)
            if len(self.logs) > 500:
                del self.logs[:len(self.logs) - 500]

    def _pump_logs(self, stream):
        for raw in iter(stream.readline, b""):
            self._log(raw.decode("utf-8", "replace"))

    def _pump_progress(self, stream):
        fps = spd = tm = None
        for raw in iter(stream.readline, b""):
            line = raw.decode("utf-8", "replace").strip()
            k, _, v = line.partition("=")
            if k == "frame":
                try:
                    cur = int(v)
                except ValueError:
                    continue
                if self.total > 0:
                    self.progress = min(1.0, cur / self.total)
                self.status = (f"frame {cur}"
                               + (f" / {self.total}" if self.total else "")
                               + (f"   {spd}" if spd else "")
                               + (f"   {tm}" if tm else ""))
            elif k == "fps":
                fps = v
            elif k == "speed":
                spd = v.strip()
            elif k == "out_time":
                tm = v.split(".")[0]
            elif k == "progress" and v == "end" and self.total > 0:
                self.progress = 1.0

    def _run(self):
        try:
            if self.image_out:
                self.total = 1
            elif is_image(self.video):
                self.total = int(round(self.args.fps * self.args.image_duration))
            else:
                self.total = probe_total_frames(self.args.ffmpeg, self.video)
            threads, waits = self._spawn()
            for t in threads:
                t.join()
            rc = 0
            for p in waits:
                rc = p.wait() or rc
            self.ok = (rc == 0 and os.path.isfile(self.out_path))
            self._log("done." if self.ok else f"pipeline returned {rc}")
        except Exception as e:
            self._log(f"error: {e}")
            self.ok = False
        finally:
            if self.ok:
                self.progress = 1.0
                self.status = f"exported -> {self.out_path}"
            else:
                self.status = "export failed (see log)"
            self.done = True

    def _layer_cmds(self, w, h):
        """One slangfx invocation carrying the whole stack — layers chain on
        the GPU inside the process (repeated --preset flags)."""
        if not self.layers:
            return []
        c = [self.args.slangfx]
        for l in self.layers:
            c += ["--preset", l["preset"]]
            if l.get("params_str"):
                c += ["--params", l["params_str"]]
        c += ["--width", str(w), "--height", str(h)]
        return [c]

    def _spawn_chain(self, decode, layer_cmds, encode):
        """decode | layer | layer | ... | encode. `decode` may be None for a
        single-process transcode (no raw pipe). Returns (threads, waits);
        threads pump the encoder's -progress plus every process's stderr."""
        procs, threads = [], []
        src = None
        if decode is not None:
            dec = _popen(decode, stdout=subprocess.PIPE)
            procs.append(dec)
            src = dec.stdout
            for cmd in layer_cmds:
                mid = _popen(cmd, stdin=src,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                src.close()
                src = mid.stdout
                procs.append(mid)
                threads.append(threading.Thread(
                    target=self._pump_logs, args=(mid.stderr,), daemon=True))
        enc = _popen(encode, stdin=src,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if src is not None:
            src.close()
        procs.append(enc)
        threads += [
            threading.Thread(target=self._pump_progress, args=(enc.stdout,), daemon=True),
            threading.Thread(target=self._pump_logs, args=(enc.stderr,), daemon=True),
        ]
        for t in threads:
            t.start()
        return threads, list(reversed(procs))

    def _spawn(self):
        a = self.args
        image = is_image(self.video)
        names = "+".join(os.path.splitext(os.path.basename(l["preset"]))[0]
                         for l in self.layers) or "no shader"
        if self.image_out:
            # Single processed frame at native resolution. For a video source,
            # seek to the playhead first; a still needs no seek. Image formats
            # don't need even dims, so no scaling.
            w, h, _ = _probe_wh_fr(a.ffmpeg, self.video)
            seek = ([] if image or self.at <= 0
                    else ["-ss", f"{self.at:.3f}"])
            self._log(f"export frame {w}x{h} @ {self.at:.2f}s ({names})  ->  "
                      f"{os.path.basename(self.out_path)}")
            if self.layers:
                decode = [a.ffmpeg, "-hide_banner", "-loglevel", "error", *seek,
                          "-i", self.video, "-frames:v", "1",
                          "-f", "rawvideo", "-pix_fmt", "rgba", "-"]
                encode = [a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                          "-progress", "pipe:1", "-nostats",
                          "-f", "rawvideo", "-pix_fmt", "rgba",
                          "-video_size", f"{w}x{h}", "-i", "-",
                          "-frames:v", "1", self.out_path]
                return self._spawn_chain(decode, self._layer_cmds(w, h), encode)
            encode = [a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                      "-progress", "pipe:1", "-nostats", *seek,
                      "-i", self.video, "-frames:v", "1", self.out_path]
            return self._spawn_chain(None, [], encode)
        if self.layers:
            w, h, fr = _probe_wh_fr(a.ffmpeg, self.video)
            if image:
                # A still becomes an --image-duration clip at --fps. yuv420p
                # needs even dims, so odd images are scaled down 1px.
                w, h, fr = w - w % 2, h - h % 2, str(a.fps)
            self._log(f"export {w}x{h} @ {fr} ({names})  ->  "
                      f"{os.path.basename(self.out_path)}")
            decode = [a.ffmpeg, "-hide_banner", "-loglevel", "error"]
            if image:
                decode += ["-loop", "1", "-framerate", fr,
                           "-t", str(a.image_duration)]
            decode += ["-i", self.video, "-vf", f"scale={w}:{h}",
                       "-f", "rawvideo", "-pix_fmt", "rgba", "-"]
            encode = [a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                      "-progress", "pipe:1", "-nostats",
                      "-f", "rawvideo", "-pix_fmt", "rgba",
                      "-video_size", f"{w}x{h}", "-framerate", fr, "-i", "-"]
            if not image:
                encode += ["-i", self.video, "-map", "0:v", "-map", "1:a?",
                           "-c:a", "copy", "-shortest"]
            encode += ["-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
                       "-pix_fmt", "yuv420p", self.out_path]
            return self._spawn_chain(decode, self._layer_cmds(w, h), encode)
        self._log(f"export (no shader)  ->  {os.path.basename(self.out_path)}")
        encode = [a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                  "-progress", "pipe:1", "-nostats"]
        if image:
            encode += ["-loop", "1", "-framerate", str(a.fps),
                       "-t", str(a.image_duration), "-i", self.video,
                       "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2"]
        else:
            encode += ["-i", self.video,
                       "-map", "0:v:0", "-map", "0:a?", "-c:a", "copy"]
        encode += ["-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
                   "-pix_fmt", "yuv420p", self.out_path]
        return self._spawn_chain(None, [], encode)


def export_video(args, video, layers, out_path, at=0.0):
    """Blocking export (used by --export). Returns (ok, message)."""
    ex = Exporter(args, video, layers, out_path, at=at).start()
    ex.join()
    return ex.ok, ex.status


def default_export_path(video, layers, ext=".mp4"):
    base = os.path.splitext(video)[0]
    tag = "+".join(os.path.splitext(os.path.basename(l["preset"]))[0]
                   for l in layers) or "h264"
    return f"{base}_{tag}{ext}"


# --------------------------------------------------------------------------
# Windows Explorer drag & drop
#
# Dear PyGui has no OS-level file-drop support, so on Windows we subclass the
# viewport's win32 window: DragAcceptFiles() opts in, and a chained WndProc
# catches WM_DROPFILES and queues the paths. The hook runs on the UI thread
# (dispatched inside render_dearpygui_frame), so the queue needs no locking.
# --------------------------------------------------------------------------
def install_file_drop(window_title, sink):
    """Accept Explorer file drops on the window titled `window_title`,
    appending dropped paths to `sink` (a list). Returns an object that must be
    kept alive (the ctypes WndProc), or None off-Windows / on failure."""
    if not sys.platform.startswith("win"):
        return None
    import ctypes
    from ctypes import wintypes
    user32, shell32 = ctypes.windll.user32, ctypes.windll.shell32
    hwnd = user32.FindWindowW(None, window_title)
    if not hwnd:
        return None

    WM_DROPFILES, GWLP_WNDPROC = 0x0233, -4
    LRESULT = ctypes.c_longlong
    WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT,
                                 wintypes.WPARAM, wintypes.LPARAM)
    user32.SetWindowLongPtrW.restype = LRESULT
    user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, WNDPROC]
    user32.CallWindowProcW.restype = LRESULT
    user32.CallWindowProcW.argtypes = [LRESULT, wintypes.HWND, wintypes.UINT,
                                       wintypes.WPARAM, wintypes.LPARAM]
    shell32.DragQueryFileW.argtypes = [wintypes.WPARAM, wintypes.UINT,
                                       ctypes.c_wchar_p, wintypes.UINT]
    shell32.DragFinish.argtypes = [wintypes.WPARAM]
    shell32.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]

    def wndproc(h, msg, wp, lp):
        if msg == WM_DROPFILES:
            for i in range(shell32.DragQueryFileW(wp, 0xFFFFFFFF, None, 0)):
                n = shell32.DragQueryFileW(wp, i, None, 0)
                buf = ctypes.create_unicode_buffer(n + 1)
                shell32.DragQueryFileW(wp, i, buf, n + 1)
                sink.append(buf.value)
            shell32.DragFinish(wp)
            return 0
        return user32.CallWindowProcW(old_proc, h, msg, wp, lp)

    proc = WNDPROC(wndproc)                    # must outlive the window
    old_proc = user32.SetWindowLongPtrW(hwnd, GWLP_WNDPROC, proc)
    shell32.DragAcceptFiles(hwnd, True)
    return proc


# --------------------------------------------------------------------------
# GUI (Dear PyGui)
# --------------------------------------------------------------------------
_UNSET = object()   # "argument not provided" sentinel (None is a real value)


def run_gui(args, w, h, layers):
    import dearpygui.dearpygui as dpg
    import numpy as np

    presets = find_presets(shaders_root_for(args))
    videos = find_videos(videos_root_for(args))

    state = {
        "video": os.path.abspath(args.input) if args.input else None,
        "layers": layers,                  # [{preset, params, enabled, port}]
        "port_next": args.control_port,
        "pipe": None,
        # Scrubbing needs a known length; 0 = unseekable (image / no video).
        "duration": (_probe_duration(args.ffmpeg, os.path.abspath(args.input))
                     if args.input and not is_image(args.input) else 0.0),
    }

    dpg.create_context()
    # Dark empty-state fill so the preview pane reads as "waiting", not broken.
    blank = np.full(w * h * 4, 0.10, dtype=np.float32)
    blank[3::4] = 1.0
    with dpg.texture_registry():
        dpg.add_raw_texture(width=w, height=h, default_value=blank,
                            format=dpg.mvFormat_Float_rgba, tag="preview_tex")

    def name_or(path, fallback):
        return os.path.basename(path) if path else fallback

    # --- param controls ---------------------------------------------------
    # Slider tags are namespaced per layer (sld_{i}_{name}) because the same
    # param name (e.g. `amount`) can appear in several layers.
    def on_slider(sender, value, user_data):
        i, name = user_data
        if state["pipe"] and 0 <= i < len(state["layers"]):
            state["pipe"].set_param(state["layers"][i], name, value)

    def layer_params_string(i, layer):
        return ",".join(f"{p['name']}={dpg.get_value(f'sld_{i}_' + p['name']):g}"
                        for p in layer["params"])

    def snapshot_params():
        """Fold current slider values into each layer's defaults so a rebuilt
        pipeline (seek / layer restructure) starts from what you see."""
        for i, l in enumerate(state["layers"]):
            for p in l["params"]:
                tag = f"sld_{i}_" + p["name"]
                if dpg.does_item_exist(tag):
                    p["default"] = dpg.get_value(tag)

    def copy_params():
        if not state["layers"]:
            dpg.set_value("status", "no shader loaded — nothing to copy")
            return
        if len(state["layers"]) == 1:
            s = layer_params_string(0, state["layers"][0])
        else:
            s = "\n".join(
                f"{os.path.basename(l['preset'])}: {layer_params_string(i, l)}"
                for i, l in enumerate(state["layers"]))
        dpg.set_clipboard_text(s)
        print("params:", s, flush=True)
        dpg.set_value("status", f"copied: {s}")

    def reset_params():
        for i, l in enumerate(state["layers"]):
            for p in l["params"]:
                dpg.set_value(f"sld_{i}_" + p["name"], p["default"])
                if state["pipe"]:
                    state["pipe"].set_param(l, p["name"], p["default"])
        if state["layers"]:
            dpg.set_value("status", "reset to defaults")

    def toggle_pause():
        p = state["pipe"]
        if p:
            p.paused = not p.paused
            dpg.configure_item("btn_playpause",
                               label="Play" if p.paused else "Pause")
            dpg.set_value("status", "paused" if p.paused else "running")

    # --- layer stack ------------------------------------------------------
    def restructure(mutate):
        """Snapshot sliders, apply a layer-stack mutation, rebuild (keeps the
        playhead — start() resumes at the current position)."""
        if state.get("export_view"):
            dpg.set_value("status", "finish the export (Continue) before editing layers")
            return
        snapshot_params()
        mutate()
        start()

    def add_layer(path):
        path = os.path.abspath(path)
        new_params = discover_params(path)
        if not new_params:
            dpg.set_value("status", f"no #pragma params in {os.path.basename(path)}")
            return
        restructure(lambda: state["layers"].append(make_layer(path, new_params)))

    def remove_layer(sender, app_data, i):
        restructure(lambda: state["layers"].pop(i))

    def move_layer(sender, app_data, user_data):
        i, d = user_data
        j = i + d
        if 0 <= j < len(state["layers"]):
            def swap():
                ls = state["layers"]
                ls[i], ls[j] = ls[j], ls[i]
            restructure(swap)

    def toggle_layer(sender, enabled, i):
        def flip():
            state["layers"][i]["enabled"] = enabled
        restructure(flip)

    def rebuild_layer_list():
        dpg.delete_item("layers_list", children_only=True)
        n = len(state["layers"])
        if not n:
            dpg.add_text("No layers — use the Shader menu.",
                         parent="layers_list", color=(170, 170, 170))
            return
        for i, l in enumerate(state["layers"]):
            with dpg.group(horizontal=True, parent="layers_list"):
                dpg.add_checkbox(default_value=l["enabled"],
                                 callback=toggle_layer, user_data=i)
                dpg.add_button(label="^", small=True, enabled=i > 0,
                               callback=move_layer, user_data=(i, -1))
                dpg.add_button(label="v", small=True, enabled=i < n - 1,
                               callback=move_layer, user_data=(i, +1))
                dpg.add_button(label="x", small=True,
                               callback=remove_layer, user_data=i)
                dpg.add_text(f"{i + 1}. {os.path.basename(l['preset'])}"
                             + ("" if l["enabled"] else "  (off)"))

    def rebuild_sliders():
        rebuild_layer_list()
        dpg.delete_item("sliders", children_only=True)
        if not state["layers"]:
            # Empty state for the shader controls.
            msg = ("No shader loaded — playing raw video.\n"
                   "Use the Shader menu to add an effect."
                   if state["video"] else
                   "No shader loaded.\nUse the Shader menu to pick one.")
            dpg.add_text(msg, parent="sliders", color=(170, 170, 170), wrap=300)
            return
        for i, l in enumerate(state["layers"]):
            hdr = dpg.add_collapsing_header(
                parent="sliders", default_open=l["enabled"],
                label=f"{i + 1} · {os.path.basename(l['preset'])}"
                      + ("" if l["enabled"] else "  (off)"))
            for p in l["params"]:
                dpg.add_slider_float(
                    parent=hdr, label=p["label"] or p["name"],
                    tag=f"sld_{i}_" + p["name"], default_value=p["default"],
                    min_value=p["min"], max_value=p["max"], callback=on_slider,
                    user_data=(i, p["name"]), width=170)

    def set_blank():
        dpg.set_value("preview_tex", blank)

    # --- export -----------------------------------------------------------
    def export_layers():
        """Enabled layers with their current slider values, exporter-shaped."""
        return [{"preset": l["preset"], "params_str": layer_params_string(i, l)}
                for i, l in enumerate(state["layers"]) if l["enabled"]]

    def start_export(out_path, duration_ok=False):
        if not state["video"]:
            dpg.set_value("status", "load a video or image before exporting")
            return
        if state.get("export_view") or not out_path:
            return
        # Still image -> video clip: ask how long the clip should run (the
        # shader animates over the whole length). Frame exports skip this.
        if (is_image(state["video"]) and not duration_ok
                and not out_path.lower().endswith(IMAGE_EXTS)):
            state["pending_export"] = out_path
            dpg.set_value("img_dur", float(args.image_duration))
            dpg.show_item("dlg_duration")
            return
        exp_layers = export_layers()
        # Frame exports render the frame under the playhead.
        at = (state["pipe"].position(state["duration"])
              if state["pipe"] and state["duration"] > 0 else 0.0)
        # Stop live playback so the export render gets the whole GPU.
        if state["pipe"]:
            state["pipe"].close()
            state["pipe"] = None
        set_blank()
        # Take over the preview area with the export panel.
        state["export_view"] = True
        state["export_log_shown"] = 0
        state["export_done_shown"] = False
        state["exporter"] = Exporter(args, state["video"], exp_layers,
                                     out_path, at=at)
        dpg.set_value("export_title", "Exporting  (live playback paused)")
        dpg.set_value("export_bar", 0.0)
        dpg.configure_item("export_bar", overlay="0%")
        dpg.set_value("export_stat", "starting...")
        dpg.delete_item("export_log", children_only=True)
        dpg.hide_item("export_continue")
        dpg.hide_item("preview_img")
        dpg.hide_item("transport")
        dpg.show_item("export_panel")
        dpg.set_value("status", f"exporting -> {os.path.basename(out_path)}")
        state["exporter"].start()

    def on_continue():
        state["export_view"] = False
        state["exporter"] = None
        dpg.hide_item("export_panel")
        dpg.show_item("preview_img")
        dpg.show_item("transport")
        start()                        # resume live playback (current video/preset)

    def open_export_dialog():
        if not state["video"]:
            dpg.set_value("status", "load a video or image before exporting")
            return
        out = default_export_path(state["video"], state["layers"])
        dpg.configure_item("dlg_export", default_path=os.path.dirname(out),
                           default_filename=os.path.basename(out))
        dpg.show_item("dlg_export")

    def on_export_pick(sender, app_data):
        f = app_data.get("file_path_name")
        if f:
            start_export(f)

    def on_duration_ok():
        try:
            args.image_duration = max(0.5, float(dpg.get_value("img_dur")))
        except (TypeError, ValueError):
            pass
        dpg.hide_item("dlg_duration")
        out = state.pop("pending_export", None)
        if out:
            start_export(out, duration_ok=True)

    def on_duration_cancel():
        state.pop("pending_export", None)
        dpg.hide_item("dlg_duration")
        dpg.set_value("status", "export cancelled")

    # --- switching --------------------------------------------------------
    def start(video=_UNSET, preset=_UNSET, at=None):
        """(Re)build the pipeline for the current video/preset. Either arg may
        be a path, None (explicitly clear), or omitted (keep current). `at`
        starts playback at that media time; a shader-only switch keeps the
        current playhead, a video switch starts from 0."""
        if state.get("export_view") and (video is not _UNSET or preset is not _UNSET):
            dpg.set_value("status", "finish the export (Continue) before switching")
            return
        if at is not None:
            resume = at
        elif video is _UNSET and state["pipe"] and state["duration"] > 0:
            resume = state["pipe"].position(state["duration"])
        else:
            resume = 0.0
        if video is not _UNSET:
            state["video"] = os.path.abspath(video) if video else None
            state["duration"] = (
                _probe_duration(args.ffmpeg, state["video"])
                if state["video"] and not is_image(state["video"]) else 0.0)
        if preset is not _UNSET:
            # `preset` replaces the whole stack (single-shader semantics);
            # add_layer/remove_layer/move_layer edit the stack in place.
            if preset:
                new_params = discover_params(os.path.abspath(preset))
                if not new_params:
                    dpg.set_value("status",
                                  f"no #pragma params in {os.path.basename(preset)}")
                    return
                state["layers"] = [make_layer(preset, new_params)]
            else:
                state["layers"] = []

        if state["pipe"]:
            state["pipe"].close()
            state["pipe"] = None

        seekable = state["duration"] > 0
        dpg.configure_item("scrub", max_value=max(state["duration"], 0.001),
                           enabled=seekable)
        dpg.set_value("scrub", resume if seekable else 0.0)
        dpg.set_value("time_label",
                      f"{fmt_time(resume)} / {fmt_time(state['duration'])}"
                      if seekable else "--:-- / --:--")
        dpg.configure_item("btn_playpause", label="Pause")

        if not state["video"]:
            # Empty state: nothing to play yet.
            set_blank()
            rebuild_sliders()
            dpg.set_value("status", "Load a video to begin (Video menu)")
            return

        # A fresh port every rebuild avoids any bind race with the old proc.
        port = state["port_next"]
        state["port_next"] += 1
        state["pipe_built_at"] = time.time()
        try:
            state["pipe"] = make_pipeline(args, state["video"], state["layers"],
                                          w, h, port, start_at=resume)
        except Exception as e:
            state["pipe"] = None
            set_blank()
            rebuild_sliders()
            dpg.set_value("status", str(e))
            return
        rebuild_sliders()
        active = [l for l in state["layers"] if l["enabled"]]
        shader = (" + ".join(os.path.basename(l["preset"]) for l in active)
                  or "raw video (no shader)")
        dpg.set_value("status", f"{shader}  |  {name_or(state['video'], '')}")

    def seek(t):
        """Restart the stream at media time t, keeping the current slider
        values (they're snapshotted into the params' defaults so the rebuilt
        pipeline starts from them)."""
        if not state["video"] or state.get("export_view") or state["duration"] <= 0:
            return
        snapshot_params()
        start(at=max(0.0, min(t, state["duration"] - 0.05)))

    def on_pick_shader(sender, app_data):
        f = app_data.get("file_path_name")
        if f and os.path.isfile(f):
            if state.pop("browse_add", False):
                add_layer(f)
            else:
                start(preset=f)

    def open_shader_dialog(add=False):
        state["browse_add"] = add
        dpg.show_item("dlg_shader")

    def on_pick_video(sender, app_data):
        f = app_data.get("file_path_name")
        if f and os.path.isfile(f):
            start(video=f)

    with dpg.file_dialog(directory_selector=False, show=False, modal=True,
                         callback=on_pick_shader, tag="dlg_shader",
                         width=720, height=440):
        dpg.add_file_extension(".slangp", color=(120, 200, 255))
        dpg.add_file_extension(".*")
    with dpg.file_dialog(directory_selector=False, show=False, modal=True,
                         callback=on_pick_video, tag="dlg_video",
                         width=720, height=440):
        dpg.add_file_extension("Video files (*.mp4 *.mov *.mkv *.webm){"
                               ".mp4,.mov,.mkv,.webm,.avi,.m4v,.gif}",
                               color=(120, 255, 160))
        dpg.add_file_extension("Image files (*.png *.jpg *.webp){"
                               ".png,.jpg,.jpeg,.bmp,.webp,.tif,.tiff}",
                               color=(255, 220, 120))
        dpg.add_file_extension(".*")
    with dpg.window(label="Still image -> video clip", tag="dlg_duration",
                    modal=True, show=False, no_resize=True, no_collapse=True,
                    width=360, pos=(160, 140)):
        dpg.add_text("The source is a single image. Choose the clip length —\n"
                     "animated shader effects run across the whole clip.",
                     color=(200, 200, 200))
        dpg.add_input_float(tag="img_dur", label="clip length (s)",
                            default_value=10.0, min_value=0.5,
                            max_value=3600.0, min_clamped=True,
                            max_clamped=True, step=1.0, width=140)
        dpg.add_spacer(height=4)
        with dpg.group(horizontal=True):
            dpg.add_button(label="Export", width=100, callback=on_duration_ok)
            dpg.add_button(label="Cancel", width=100, callback=on_duration_cancel)

    with dpg.file_dialog(directory_selector=False, show=False, modal=True,
                         callback=on_export_pick, tag="dlg_export",
                         width=720, height=440):
        dpg.add_file_extension(".mp4", color=(255, 210, 120))
        dpg.add_file_extension("Image (single frame) (*.png *.jpg *.webp){"
                               ".png,.jpg,.jpeg,.bmp,.webp,.tif,.tiff}",
                               color=(255, 220, 120))
        dpg.add_file_extension(".*")

    # --- window + menu ----------------------------------------------------
    with dpg.window(tag="main"):
        with dpg.menu_bar():
            with dpg.menu(label="Shader"):
                dpg.add_menu_item(label="(none — raw video)",
                                  callback=lambda: start(preset=None))
                dpg.add_separator()
                # Click = replace the stack with this one shader; the "Add
                # layer" submenu appends instead.
                for pth in presets:
                    dpg.add_menu_item(
                        label=os.path.basename(pth),
                        callback=lambda s, a, u: start(preset=u), user_data=pth)
                if presets:
                    dpg.add_separator()
                with dpg.menu(label="Add layer"):
                    for pth in presets:
                        dpg.add_menu_item(
                            label=os.path.basename(pth),
                            callback=lambda s, a, u: add_layer(u), user_data=pth)
                    if presets:
                        dpg.add_separator()
                    dpg.add_menu_item(
                        label="Browse .slangp...",
                        callback=lambda: open_shader_dialog(add=True))
                dpg.add_menu_item(label="Browse .slangp...",
                                  callback=lambda: open_shader_dialog(add=False))
            with dpg.menu(label="Video"):
                for pth in videos:
                    dpg.add_menu_item(
                        label=os.path.basename(pth),
                        callback=lambda s, a, u: start(video=u), user_data=pth)
                if videos:
                    dpg.add_separator()
                dpg.add_menu_item(label="Browse video/image...",
                                  callback=lambda: dpg.show_item("dlg_video"))
            with dpg.menu(label="Params"):
                dpg.add_menu_item(label="Copy params", callback=copy_params)
                dpg.add_menu_item(label="Reset to defaults", callback=reset_params)
                dpg.add_menu_item(label="Pause / resume", callback=toggle_pause)
            with dpg.menu(label="Export"):
                dpg.add_menu_item(
                    label="Export H.264 (next to source)",
                    callback=lambda: start_export(
                        default_export_path(state["video"], state["layers"])
                        if state["video"] else ""))
                dpg.add_menu_item(
                    label="Export frame as PNG (at playhead)",
                    callback=lambda: start_export(
                        default_export_path(state["video"], state["layers"], ".png")
                        if state["video"] else ""))
                dpg.add_menu_item(label="Export as...",
                                  callback=open_export_dialog)

        with dpg.group(horizontal=True):
            with dpg.group():            # left column: preview OR export takeover
                dpg.add_image("preview_tex", tag="preview_img", width=w, height=h)
                with dpg.group(horizontal=True, tag="transport"):
                    dpg.add_button(label="Pause", tag="btn_playpause",
                                   width=52, callback=toggle_pause)
                    dpg.add_text("--:-- / --:--", tag="time_label")
                    dpg.add_slider_float(tag="scrub", width=w - 180,
                                         min_value=0.0, max_value=0.001,
                                         format="", enabled=False)
                with dpg.child_window(tag="export_panel", width=w, height=h,
                                      show=False):
                    dpg.add_spacer(height=6)
                    dpg.add_text("Exporting", tag="export_title")
                    dpg.add_progress_bar(tag="export_bar", width=w - 30,
                                         default_value=0.0, overlay="0%")
                    dpg.add_text("", tag="export_stat", color=(200, 200, 200))
                    dpg.add_separator()
                    dpg.add_text("log")
                    dpg.add_child_window(tag="export_log", width=w - 30,
                                         height=h - 200)
                    dpg.add_spacer(height=6)
                    dpg.add_button(label="Continue (back to live play)",
                                   tag="export_continue", show=False,
                                   callback=on_continue)
            with dpg.child_window(width=340, autosize_y=True):
                dpg.add_text("Layers")
                dpg.add_child_window(tag="layers_list", autosize_x=True,
                                     height=108)
                dpg.add_separator()
                dpg.add_child_window(tag="sliders", autosize_x=True, height=-64)
                dpg.add_separator()
                with dpg.group(horizontal=True):
                    dpg.add_button(label="Copy params", callback=copy_params)
                    dpg.add_button(label="Reset", callback=reset_params)
                    dpg.add_button(label="Pause", callback=toggle_pause)
                dpg.add_text("", tag="status")

    # Seek only when the drag is released — every slider move during the drag
    # would otherwise restart the ffmpeg|slangfx pair dozens of times.
    with dpg.item_handler_registry(tag="scrub_handlers"):
        dpg.add_item_deactivated_after_edit_handler(
            callback=lambda: seek(dpg.get_value("scrub")))
    dpg.bind_item_handler_registry("scrub", "scrub_handlers")

    dpg.create_viewport(title="slangfx live", width=w + 380, height=h + 150)
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("main", True)

    # Explorer drag & drop: media files switch the source, .slangp the shader.
    dropped = []
    drop_hook = install_file_drop("slangfx live", dropped)  # noqa: F841 (keep alive)
    if drop_hook is None and sys.platform.startswith("win"):
        print("warning: Explorer drag & drop hook failed to install", flush=True)

    def handle_drops():
        batch, dropped[:] = list(dropped), []
        kw = {}
        for path in batch:
            if not os.path.isfile(path):
                continue
            ext = os.path.splitext(path)[1].lower()
            if ext == ".slangp":
                kw["preset"] = path
            elif ext in MEDIA_EXTS:
                kw["video"] = path
            else:
                dpg.set_value("status",
                              f"can't open dropped file: {os.path.basename(path)}")
        if kw:
            start(**kw)

    start()   # build initial state (plays if a video was given, else empty)

    # A stale engine silently drops routed slider updates and runs only one
    # layer of a stack — the failure looks like "sliders stopped working",
    # so call it out loudly up front.
    if not engine_supports_layers(args.slangfx):
        msg = (f"WARNING: outdated slangfx binary ({args.slangfx}) — "
               "update it; layer stacks and live sliders need a current build")
        print(msg, flush=True)
        dpg.set_value("status", msg)

    def fit_preview(vw, vh):
        """Scale the preview widget (and the widgets sized off it) to fit the
        window, preserving aspect. The texture itself stays w x h — only the
        displayed size changes."""
        avail_w = max(vw - 375, 160)          # right panel + paddings
        avail_h = max(vh - 95, 120)           # menu bar + transport + paddings
        s = min(avail_w / w, avail_h / h)
        dw, dh = max(int(w * s), 32), max(int(h * s), 32)
        dpg.configure_item("preview_img", width=dw, height=dh)
        dpg.configure_item("scrub", width=max(dw - 180, 60))
        dpg.configure_item("export_panel", width=dw, height=dh)
        dpg.configure_item("export_bar", width=dw - 30)
        dpg.configure_item("export_log", width=dw - 30, height=max(dh - 200, 60))

    scale = np.float32(1.0 / 255.0)
    last_client = (0, 0)
    while dpg.is_dearpygui_running():
        client = (dpg.get_viewport_client_width(),
                  dpg.get_viewport_client_height())
        if client != last_client:
            last_client = client
            fit_preview(*client)
        if dropped:
            handle_drops()
        ex = state.get("exporter")
        if ex is not None:
            # Export takeover: drive the progress bar, status, and log panel.
            dpg.set_value("export_bar", ex.progress if ex.progress >= 0 else 0.0)
            dpg.configure_item("export_bar",
                               overlay=(f"{ex.progress * 100:.0f}%"
                                        if ex.progress >= 0 else "working..."))
            dpg.set_value("export_stat", ex.status)
            shown = state.get("export_log_shown", 0)
            if len(ex.logs) > shown:
                for line in ex.logs[shown:]:
                    dpg.add_text(line, parent="export_log", wrap=w - 50)
                state["export_log_shown"] = len(ex.logs)
                dpg.set_y_scroll("export_log", -1.0)
            if ex.done and not state.get("export_done_shown"):
                state["export_done_shown"] = True
                dpg.set_value("export_title",
                              "Export complete" if ex.ok else "Export failed")
                if ex.ok:
                    dpg.set_value("export_bar", 1.0)
                    dpg.configure_item("export_bar", overlay="100%")
                dpg.show_item("export_continue")
                dpg.set_value("status", ex.status)
        else:
            p = state["pipe"]
            if p:
                buf = p.get_latest()
                if buf is not None:
                    arr = np.frombuffer(buf, dtype=np.uint8).astype(np.float32) * scale
                    dpg.set_value("preview_tex", arr)
                if not p.running and p.frames == 0:
                    dpg.set_value("status", "could not open source (check the file)")
                elif not p.running:
                    # The decoder stopped mid-stream — a container whose
                    # demuxer can't rewind for -stream_loop, or a decode
                    # error. Playback must always loop, so restart from the
                    # top (keeping slider values); the 1s guard prevents a
                    # rebuild storm if the file dies instantly every time.
                    if time.time() - state.get("pipe_built_at", 0) > 1.0:
                        snapshot_params()
                        start(at=0.0)
                if state["duration"] > 0:
                    if dpg.is_item_active("scrub"):
                        # Mid-drag: preview the target time, don't fight the drag.
                        t = dpg.get_value("scrub")
                    else:
                        t = p.position(state["duration"])
                        dpg.set_value("scrub", t)
                    dpg.set_value("time_label",
                                  f"{fmt_time(t)} / {fmt_time(state['duration'])}")
        dpg.render_dearpygui_frame()

    if state["pipe"]:
        state["pipe"].close()
    dpg.destroy_context()


# --------------------------------------------------------------------------
# Headless self-test: live control + a shader switch, no window.
# --------------------------------------------------------------------------
def _l1(a, b):
    """Sampled L1 distance between two RGBA buffers (every ~1KB)."""
    return sum(abs(x - y) for x, y in zip(a[::997], b[::997]))


def _wait_frames(pipe, n, deadline):
    n0 = pipe.frames
    while pipe.frames < n0 + n and time.time() < deadline and pipe.running:
        time.sleep(0.02)


def _verify_live_control(pipe, deadline):
    """amount=0 is an exact passthrough (static); amount=1 is the full effect.
    Returns (d_static, d_change): static baseline drift and the change the live
    update produced. Isolates the UDP update from the shader's own animation."""
    layer0 = pipe.layers[0]
    _wait_frames(pipe, 5, deadline)          # wait until slangfx is listening
    pipe.set_param(layer0, "speed", 0.0)
    pipe.set_param(layer0, "amount", 0.0)
    _wait_frames(pipe, 30, deadline)
    a = pipe.get_latest()
    _wait_frames(pipe, 8, deadline)
    a2 = pipe.get_latest()
    pipe.set_param(layer0, "amount", 1.0)
    _wait_frames(pipe, 12, deadline)
    b = pipe.get_latest()
    if not all(x is not None for x in (a, a2, b)):
        return None, None
    return _l1(a, a2), _l1(a, b)


def run_selftest(args, w, h, layers):
    if not os.path.isfile(args.slangfx):
        print(f"SELFTEST FAIL: slangfx not found at {args.slangfx}"); return 2

    # 1) Live control on the first launch preset. (realtime=False: run unpaced
    # so the test finishes fast — the GUI uses realtime pacing.)
    params = layers[0]["params"]
    pipe = make_pipeline(args, args.input, [dict(layers[0])],
                         w, h, args.control_port, realtime=False)
    d_static, d_change = (None, None)
    if any(p["name"] == "amount" for p in params):
        d_static, d_change = _verify_live_control(pipe, time.time() + 25)
    n1 = pipe.frames
    pipe.close()
    print(f"[1] params: {len(params)} ({', '.join(p['name'] for p in params)}); "
          f"frames {n1}, preview {w}x{h}")
    ctrl_ok = d_static == 0 and (d_change or 0) > 1000
    if d_static is not None:
        print(f"    live control: drift(amount=0)={d_static}, "
              f"change(0->1)={d_change} -> {'OK' if ctrl_ok else 'FAIL'}")
    else:
        ctrl_ok = True
        print("    live control: skipped (no 'amount' param)")

    # 2) Switch to a different preset and confirm frames flow + params change.
    launch = os.path.abspath(layers[0]["preset"])
    others = [p for p in find_presets(shaders_root_for(args))
              if os.path.abspath(p) != launch]
    switch_ok = True
    if others:
        p2 = others[0]
        params2 = discover_params(p2)
        pipe2 = make_pipeline(args, args.input, [make_layer(p2, params2)],
                              w, h, args.control_port + 10, realtime=False)
        _wait_frames(pipe2, 12, time.time() + 20)
        switch_ok = pipe2.frames >= 10 and bool(params2)
        print(f"[2] switched -> {os.path.basename(p2)}: frames {pipe2.frames}, "
              f"params {len(params2)} -> {'OK' if switch_ok else 'FAIL'}")
        pipe2.close()
    else:
        print("[2] switch: skipped (no other presets discovered)")

    # 3) Two-layer chain: launch preset + another, frames must flow and layer
    # 2's live control must act on the CHAINED output.
    chain_ok = True
    if others:
        p2 = others[0]
        stack = [dict(layers[0]), make_layer(p2, discover_params(p2))]
        pipe3 = make_pipeline(args, args.input, stack, w, h,
                              args.control_port + 20, realtime=False)
        _wait_frames(pipe3, 12, time.time() + 25)
        chain_ok = pipe3.frames >= 10
        print(f"[3] 2-layer chain ({os.path.basename(launch)} -> "
              f"{os.path.basename(p2)}): frames {pipe3.frames} -> "
              f"{'OK' if chain_ok else 'FAIL'}")
        pipe3.close()
    else:
        print("[3] chain: skipped (no other presets discovered)")

    ok = ctrl_ok and switch_ok and chain_ok
    print("SELFTEST PASS" if ok else "SELFTEST FAIL")
    return 0 if ok else 1


def engine_supports_layers(slangfx):
    """True when the slangfx binary understands multi-preset stacks and
    'i:name=value' routed live control (both shipped together — probed via
    the --help text). Assume modern when the probe itself fails; the goal is
    a loud warning for stale binaries, not a hard gate."""
    try:
        r = subprocess.run([slangfx, "--help"], capture_output=True,
                           text=True, timeout=5, **_NOWIN)
        return "Repeatable" in (r.stdout or "") + (r.stderr or "")
    except Exception:
        return True


def default_slangfx(here):
    """Prefer the binary shipped next to the app (frozen release layout), then
    the repo's built binary. PATH lookup is the last resort and must be
    vetted: on Windows shutil.which() searches the cwd and honors PATHEXT, so
    which("slangfx") run from wrappers/ happily returns slangfx.py — which then
    dies at spawn time with WinError 193 (not a valid Win32 application)."""
    for cand in (os.path.join(app_dir(), "slangfx.exe"),
                 os.path.join(app_dir(), "slangfx"),
                 os.path.join(here, "..", "build", "slangfx.exe"),
                 os.path.join(here, "..", "build", "slangfx")):
        if os.path.isfile(cand):
            return os.path.normpath(cand)
    found = shutil.which("slangfx")
    if found and not found.lower().endswith((".py", ".pyc")):
        return found
    return "slangfx"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", default=None,
                    help="Video/image to stream. Optional — load one from the "
                         "Video menu in the UI.")
    ap.add_argument("--preset", action="append", default=None,
                    help="Path to a .slangp preset. Repeatable — each one is a "
                         "layer, applied in order. Optional: load from the "
                         "Shader menu in the UI; without one the video plays raw.")
    ap.add_argument("--shaders-dir", default=None,
                    help="Directory scanned to populate the Shader menu "
                         "(default: the --preset's shaders tree, else cwd).")
    ap.add_argument("--videos-dir", default=None,
                    help="Directory scanned to populate the Video menu "
                         "(default: the --input's folder, else cwd).")
    ap.add_argument("--params", action="append", default=None,
                    help="Initial 'k=v,k=v' param overrides (seed the sliders; "
                         "also used by --export). Repeatable: the Nth --params "
                         "applies to the Nth --preset.")
    ap.add_argument("--export", default=None, metavar="OUT",
                    help="Headless: render --input through the --preset layer "
                         "stack (with --params) and exit. A video OUT gets "
                         "H.264 with the original audio at source res/fps; an "
                         "image OUT (.png/.jpg/...) gets the single frame at "
                         "--at.")
    ap.add_argument("--at", type=float, default=0.0,
                    help="Media time (s) of the frame for image exports "
                         "(default 0).")
    ap.add_argument("--image-duration", type=float, default=10.0,
                    help="Clip length (seconds) when exporting a still image "
                         "(default 10). The preview loops a still forever.")
    ap.add_argument("--width", type=int, default=0, help="Preview width (px).")
    ap.add_argument("--height", type=int, default=0, help="Preview height (px).")
    ap.add_argument("--fps", type=int, default=30, help="Preview frame rate.")
    ap.add_argument("--control-port", type=int, default=9000)
    ap.add_argument("--slangfx", default=default_slangfx(here))
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    ap.add_argument("--selftest", action="store_true",
                    help="Run headless; verify live control and a shader switch. "
                         "Requires --input and --preset.")
    args = ap.parse_args()

    if args.input and not os.path.isfile(args.input):
        sys.exit(f"input not found: {args.input}")

    # Build the launch layer stack: one layer per --preset, in order, with the
    # matching --params entry (if any) folded into that layer's defaults.
    params_cli = args.params or []
    layers = []
    for i, pth in enumerate(args.preset or []):
        if not os.path.isfile(pth):
            sys.exit(f"preset not found: {pth}")
        params = discover_params(pth)
        if not params:
            sys.exit(f"no #pragma parameters found in {pth}")
        ov = parse_params_str(params_cli[i]) if i < len(params_cli) else {}
        for p in params:
            if p["name"] in ov:
                p["default"] = ov[p["name"]]
        layers.append(make_layer(pth, params))

    if args.export:
        if not args.input:
            sys.exit("--export requires --input")
        exp_layers = [
            {"preset": l["preset"],
             "params_str": ",".join(f"{p['name']}={p['default']:g}"
                                    for p in l["params"])}
            for l in layers]
        ok, msg = export_video(args, os.path.abspath(args.input), exp_layers,
                               os.path.abspath(args.export), at=args.at)
        print(msg)
        sys.exit(0 if ok else 1)

    if args.selftest:
        if not args.input or not layers:
            sys.exit("--selftest requires both --input and --preset")
        w, h = preview_size(args)
        sys.exit(run_selftest(args, w, h, layers))

    w, h = preview_size(args)
    run_gui(args, w, h, layers)


if __name__ == "__main__":
    main()
