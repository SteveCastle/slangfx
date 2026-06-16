#!/usr/bin/env python3
"""slangfx_live.py — real-time shader-param playground.

Streams a video through a slang preset and shows the result in a window with
one slider per `#pragma parameter`. Dragging a slider sends a live
`name=value` update to slangfx over UDP (localhost), applied on the next frame
with no pipeline rebuild. The menu bar switches shader or video on the fly
(the ffmpeg|slangfx pair is restarted and the sliders rebuilt). Tuned a look
you like? "Copy params" gives the `k=v,k=v` string for `slangfx --params` /
`beat_cut --shader-params`, or "Export" renders the current look to an H.264
file at the source's native resolution/fps with the original audio copied.

Pipeline:  ffmpeg (decode+loop+letterbox) -> slangfx (--control-port) -> app
Controls:  app -> UDP name=value -> slangfx
Menu:      Shader / Video (quick-pick discovered files + Browse...), Params

Both -i/--input and --preset are optional: start empty and load them from the
menus, or pass a video with no preset to play it raw (no shader) until you pick
one. The Shader/Video menus list files discovered under --shaders-dir/
--videos-dir (defaulting sensibly), plus a Browse... dialog.

Usage:
    python -m pip install -r wrappers/requirements.txt
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
    """Videos directly in `root`, sorted. Empty if root is missing/None."""
    if not root or not os.path.isdir(root):
        return []
    try:
        names = os.listdir(root)
    except OSError:
        return []
    return sorted(os.path.join(root, f) for f in names
                  if f.lower().endswith(VIDEO_EXTS))


def shaders_root_for(args):
    """Directory to scan for presets: explicit --shaders-dir, else the shaders
    tree holding --preset (its grandparent), else the bundled slangfx/shaders
    next to this script, else the cwd."""
    if args.shaders_dir:
        return args.shaders_dir
    if args.preset:
        return os.path.dirname(os.path.dirname(os.path.abspath(args.preset)))
    bundled = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "shaders")
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
        out = subprocess.check_output(
            [ffprobe, "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", path],
            text=True).strip().splitlines()[0]
        w, h = (int(x) for x in out.split("x")[:2])
        return w, h
    except Exception:
        return 1280, 720


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
    """Feeds letterboxed RGBA frames from one (video, preset) pair. With a
    preset it's ffmpeg | slangfx (+ a UDP control channel); with preset=None it
    is ffmpeg alone (raw video, no shader). A reader thread keeps only the
    latest frame so the UI never blocks (newest-wins; stale frames dropped).
    Switching video/preset builds a fresh Pipeline."""

    def __init__(self, slangfx, ffmpeg, video, preset, w, h, fps, port, params,
                 realtime=True):
        self.w, self.h, self.fb = w, h, w * h * 4
        self.port = port
        self.params = params
        self.latest = None
        self.lock = threading.Lock()
        self.paused = False
        self.running = True
        self.frames = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Letterbox any source into the fixed WxH so the texture never resizes.
        vf = (f"scale={w}:{h}:force_original_aspect_ratio=decrease,"
              f"pad={w}:{h}:-1:-1:color=black,setsar=1")
        # `-re` paces ffmpeg to the source's native frame rate (real time);
        # without it ffmpeg floods the pipe and the preview plays many times too
        # fast. Off for the self-test, where we want frames as fast as possible.
        ff_cmd = [ffmpeg, "-loglevel", "error", "-stream_loop", "-1"]
        if realtime:
            ff_cmd += ["-re"]
        ff_cmd += ["-i", video, "-vf", vf,
                   "-f", "rawvideo", "-pix_fmt", "rgba", "-r", str(fps), "-"]
        self.ff = subprocess.Popen(ff_cmd, stdout=subprocess.PIPE)

        if preset:
            startup = ",".join(f"{p['name']}={p['default']}" for p in params)
            self.sfx = subprocess.Popen(
                [slangfx, "--preset", preset, "--width", str(w), "--height", str(h),
                 "--control-port", str(port), "--params", startup],
                stdin=self.ff.stdout, stdout=subprocess.PIPE)
            self.ff.stdout.close()
            self.src = self.sfx.stdout         # shaded frames
        else:
            self.sfx = None
            self.src = self.ff.stdout          # raw (letterboxed) video frames

        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        rd = self.src
        while self.running:
            buf = rd.read(self.fb)
            if not buf or len(buf) < self.fb:
                break
            self.frames += 1
            if not self.paused:
                with self.lock:
                    self.latest = buf
        self.running = False

    def get_latest(self):
        with self.lock:
            return self.latest

    def set_param(self, name, value):
        if self.sfx is None:               # no shader -> nothing to control
            return
        try:
            self.sock.sendto(f"{name}={value}".encode(), ("127.0.0.1", self.port))
        except OSError:
            pass

    def close(self):
        self.running = False
        procs = [self.ff] if self.sfx is None else [self.sfx, self.ff]
        for p in procs:
            try:
                p.terminate()
            except Exception:
                pass


def make_pipeline(args, video, preset, w, h, port, params, realtime=True):
    return Pipeline(args.slangfx, args.ffmpeg, video, preset,
                    w, h, args.fps, port, params, realtime=realtime)


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
        out = subprocess.check_output(
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
        out = subprocess.check_output(
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
    """Runs a full-resolution H.264 export in a background thread and exposes
    pollable state: `progress` (0..1, or <0 if total unknown), `status`, `logs`
    (list of lines), `done`, `ok`. The shaded path is ffmpeg | slangfx | ffmpeg
    with `-progress` parsed from the encoder; without a preset it's a single
    libx264 transcode. Original audio is copied; res/fps match the source."""

    def __init__(self, args, video, preset, params_str, out_path):
        self.args = args
        self.video = video
        self.preset = preset
        self.params_str = params_str
        self.out_path = out_path
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

    def _spawn(self):
        a = self.args
        if self.preset:
            w, h, fr = _probe_wh_fr(a.ffmpeg, self.video)
            self._log(f"export {w}x{h} @ {fr}  ->  {os.path.basename(self.out_path)}")
            decode = [a.ffmpeg, "-hide_banner", "-loglevel", "error",
                      "-i", self.video, "-f", "rawvideo", "-pix_fmt", "rgba", "-"]
            proc = [a.slangfx, "--preset", self.preset,
                    "--width", str(w), "--height", str(h)]
            if self.params_str:
                proc += ["--params", self.params_str]
            encode = [a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                      "-progress", "pipe:1", "-nostats",
                      "-f", "rawvideo", "-pix_fmt", "rgba",
                      "-video_size", f"{w}x{h}", "-framerate", fr, "-i", "-",
                      "-i", self.video, "-map", "0:v", "-map", "1:a?",
                      "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
                      "-pix_fmt", "yuv420p", "-c:a", "copy", "-shortest",
                      self.out_path]
            p1 = subprocess.Popen(decode, stdout=subprocess.PIPE)
            p2 = subprocess.Popen(proc, stdin=p1.stdout,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            p1.stdout.close()
            p3 = subprocess.Popen(encode, stdin=p2.stdout,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            p2.stdout.close()
            threads = [
                threading.Thread(target=self._pump_progress, args=(p3.stdout,), daemon=True),
                threading.Thread(target=self._pump_logs, args=(p3.stderr,), daemon=True),
                threading.Thread(target=self._pump_logs, args=(p2.stderr,), daemon=True),
            ]
        else:
            self._log(f"export (no shader)  ->  {os.path.basename(self.out_path)}")
            encode = [a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                      "-progress", "pipe:1", "-nostats", "-i", self.video,
                      "-map", "0:v:0", "-map", "0:a?",
                      "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
                      "-pix_fmt", "yuv420p", "-c:a", "copy", self.out_path]
            p1 = subprocess.Popen(encode, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            p2 = p3 = None
            threads = [
                threading.Thread(target=self._pump_progress, args=(p1.stdout,), daemon=True),
                threading.Thread(target=self._pump_logs, args=(p1.stderr,), daemon=True),
            ]
        for t in threads:
            t.start()
        waits = [p for p in (p3, p2, p1) if p is not None]
        return threads, waits


def export_video(args, video, preset, params_str, out_path):
    """Blocking export (used by --export). Returns (ok, message)."""
    ex = Exporter(args, video, preset, params_str, out_path).start()
    ex.join()
    return ex.ok, ex.status


def default_export_path(video, preset):
    base = os.path.splitext(video)[0]
    tag = os.path.splitext(os.path.basename(preset))[0] if preset else "h264"
    return f"{base}_{tag}.mp4"


# --------------------------------------------------------------------------
# GUI (Dear PyGui)
# --------------------------------------------------------------------------
_UNSET = object()   # "argument not provided" sentinel (None is a real value)


def run_gui(args, w, h, params):
    import dearpygui.dearpygui as dpg
    import numpy as np

    presets = find_presets(shaders_root_for(args))
    videos = find_videos(videos_root_for(args))

    state = {
        "video": os.path.abspath(args.input) if args.input else None,
        "preset": os.path.abspath(args.preset) if args.preset else None,
        "params": params,
        "port": args.control_port,
        "pipe": None,
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
    def on_slider(sender, value, name):
        if state["pipe"]:
            state["pipe"].set_param(name, value)

    def copy_params():
        if not state["params"]:
            dpg.set_value("status", "no shader loaded — nothing to copy")
            return
        s = ",".join(f"{p['name']}={dpg.get_value('sld_' + p['name']):g}"
                     for p in state["params"])
        dpg.set_clipboard_text(s)
        print("params:", s, flush=True)
        dpg.set_value("status", f"copied: {s}")

    def reset_params():
        for p in state["params"]:
            dpg.set_value("sld_" + p["name"], p["default"])
            if state["pipe"]:
                state["pipe"].set_param(p["name"], p["default"])
        if state["params"]:
            dpg.set_value("status", "reset to defaults")

    def toggle_pause():
        p = state["pipe"]
        if p:
            p.paused = not p.paused
            dpg.set_value("status", "paused" if p.paused else "running")

    def rebuild_sliders():
        dpg.delete_item("sliders", children_only=True)
        dpg.set_value("preset_label", name_or(state["preset"], "(no shader)"))
        if not state["params"]:
            # Empty state for the shader controls.
            msg = ("No shader loaded — playing raw video.\n"
                   "Use the Shader menu to add an effect."
                   if state["video"] else
                   "No shader loaded.\nUse the Shader menu to pick one.")
            dpg.add_text(msg, parent="sliders", color=(170, 170, 170), wrap=300)
            return
        for p in state["params"]:
            dpg.add_slider_float(
                parent="sliders", label=p["label"] or p["name"],
                tag="sld_" + p["name"], default_value=p["default"],
                min_value=p["min"], max_value=p["max"], callback=on_slider,
                user_data=p["name"], width=180)

    def set_blank():
        dpg.set_value("preview_tex", blank)

    # --- export -----------------------------------------------------------
    def current_params_string():
        return ",".join(f"{p['name']}={dpg.get_value('sld_' + p['name']):g}"
                        for p in state["params"])

    def start_export(out_path):
        if not state["video"]:
            dpg.set_value("status", "load a video before exporting")
            return
        if state.get("export_view") or not out_path:
            return
        params_str = current_params_string()
        # Stop live playback so the export render gets the whole GPU.
        if state["pipe"]:
            state["pipe"].close()
            state["pipe"] = None
        set_blank()
        # Take over the preview area with the export panel.
        state["export_view"] = True
        state["export_log_shown"] = 0
        state["export_done_shown"] = False
        state["exporter"] = Exporter(args, state["video"], state["preset"],
                                     params_str, out_path)
        dpg.set_value("export_title", "Exporting  (live playback paused)")
        dpg.set_value("export_bar", 0.0)
        dpg.configure_item("export_bar", overlay="0%")
        dpg.set_value("export_stat", "starting...")
        dpg.delete_item("export_log", children_only=True)
        dpg.hide_item("export_continue")
        dpg.hide_item("preview_img")
        dpg.show_item("export_panel")
        dpg.set_value("status", f"exporting -> {os.path.basename(out_path)}")
        state["exporter"].start()

    def on_continue():
        state["export_view"] = False
        state["exporter"] = None
        dpg.hide_item("export_panel")
        dpg.show_item("preview_img")
        start()                        # resume live playback (current video/preset)

    def open_export_dialog():
        if not state["video"]:
            dpg.set_value("status", "load a video before exporting")
            return
        out = default_export_path(state["video"], state["preset"])
        dpg.configure_item("dlg_export", default_path=os.path.dirname(out),
                           default_filename=os.path.basename(out))
        dpg.show_item("dlg_export")

    def on_export_pick(sender, app_data):
        f = app_data.get("file_path_name")
        if f:
            start_export(f)

    # --- switching --------------------------------------------------------
    def start(video=_UNSET, preset=_UNSET):
        """(Re)build the pipeline for the current video/preset. Either arg may
        be a path, None (explicitly clear), or omitted (keep current)."""
        if state.get("export_view") and (video is not _UNSET or preset is not _UNSET):
            dpg.set_value("status", "finish the export (Continue) before switching")
            return
        if video is not _UNSET:
            state["video"] = os.path.abspath(video) if video else None
        if preset is not _UNSET:
            if preset:
                new_params = discover_params(os.path.abspath(preset))
                if not new_params:
                    dpg.set_value("status",
                                  f"no #pragma params in {os.path.basename(preset)}")
                    return
                state["preset"] = os.path.abspath(preset)
                state["params"] = new_params
            else:
                state["preset"] = None
                state["params"] = []

        if state["pipe"]:
            state["pipe"].close()
            state["pipe"] = None

        if not state["video"]:
            # Empty state: nothing to play yet.
            set_blank()
            rebuild_sliders()
            dpg.set_value("status", "Load a video to begin (Video menu)")
            return

        port = state["port"]
        state["port"] += 1                     # fresh port avoids any bind race
        state["pipe"] = make_pipeline(args, state["video"], state["preset"],
                                      w, h, port, state["params"])
        rebuild_sliders()
        shader = name_or(state["preset"], "raw video (no shader)")
        dpg.set_value("status", f"{shader}  |  {name_or(state['video'], '')}")

    def on_pick_shader(sender, app_data):
        f = app_data.get("file_path_name")
        if f and os.path.isfile(f):
            start(preset=f)

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
        dpg.add_file_extension(".*")
    with dpg.file_dialog(directory_selector=False, show=False, modal=True,
                         callback=on_export_pick, tag="dlg_export",
                         width=720, height=440):
        dpg.add_file_extension(".mp4", color=(255, 210, 120))
        dpg.add_file_extension(".*")

    # --- window + menu ----------------------------------------------------
    with dpg.window(tag="main"):
        with dpg.menu_bar():
            with dpg.menu(label="Shader"):
                dpg.add_menu_item(label="(none — raw video)",
                                  callback=lambda: start(preset=None))
                dpg.add_separator()
                for pth in presets:
                    dpg.add_menu_item(
                        label=os.path.basename(pth),
                        callback=lambda s, a, u: start(preset=u), user_data=pth)
                if presets:
                    dpg.add_separator()
                dpg.add_menu_item(label="Browse .slangp...",
                                  callback=lambda: dpg.show_item("dlg_shader"))
            with dpg.menu(label="Video"):
                for pth in videos:
                    dpg.add_menu_item(
                        label=os.path.basename(pth),
                        callback=lambda s, a, u: start(video=u), user_data=pth)
                if videos:
                    dpg.add_separator()
                dpg.add_menu_item(label="Browse video...",
                                  callback=lambda: dpg.show_item("dlg_video"))
            with dpg.menu(label="Params"):
                dpg.add_menu_item(label="Copy params", callback=copy_params)
                dpg.add_menu_item(label="Reset to defaults", callback=reset_params)
                dpg.add_menu_item(label="Pause / resume", callback=toggle_pause)
            with dpg.menu(label="Export"):
                dpg.add_menu_item(
                    label="Export H.264 (next to source)",
                    callback=lambda: start_export(
                        default_export_path(state["video"], state["preset"])
                        if state["video"] else ""))
                dpg.add_menu_item(label="Export as...",
                                  callback=open_export_dialog)

        with dpg.group(horizontal=True):
            with dpg.group():            # left column: preview OR export takeover
                dpg.add_image("preview_tex", tag="preview_img", width=w, height=h)
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
                dpg.add_text("(no shader)", tag="preset_label")
                dpg.add_separator()
                dpg.add_child_window(tag="sliders", autosize_x=True, height=-64)
                dpg.add_separator()
                with dpg.group(horizontal=True):
                    dpg.add_button(label="Copy params", callback=copy_params)
                    dpg.add_button(label="Reset", callback=reset_params)
                    dpg.add_button(label="Pause", callback=toggle_pause)
                dpg.add_text("", tag="status")

    dpg.create_viewport(title="slangfx live", width=w + 380, height=h + 110)
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("main", True)

    start()   # build initial state (plays if a video was given, else empty)

    scale = np.float32(1.0 / 255.0)
    while dpg.is_dearpygui_running():
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
    _wait_frames(pipe, 5, deadline)          # wait until slangfx is listening
    pipe.set_param("speed", 0.0)
    pipe.set_param("amount", 0.0)
    _wait_frames(pipe, 30, deadline)
    a = pipe.get_latest()
    _wait_frames(pipe, 8, deadline)
    a2 = pipe.get_latest()
    pipe.set_param("amount", 1.0)
    _wait_frames(pipe, 12, deadline)
    b = pipe.get_latest()
    if not all(x is not None for x in (a, a2, b)):
        return None, None
    return _l1(a, a2), _l1(a, b)


def run_selftest(args, w, h, params):
    if not os.path.isfile(args.slangfx):
        print(f"SELFTEST FAIL: slangfx not found at {args.slangfx}"); return 2

    # 1) Live control on the launch preset. (realtime=False: run unpaced so the
    # test finishes fast — the GUI uses realtime pacing.)
    pipe = make_pipeline(args, args.input, args.preset, w, h, args.control_port,
                         params, realtime=False)
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
    others = [p for p in find_presets(shaders_root_for(args))
              if os.path.abspath(p) != os.path.abspath(args.preset)]
    switch_ok = True
    if others:
        p2 = others[0]
        params2 = discover_params(p2)
        pipe2 = make_pipeline(args, args.input, p2, w, h, args.control_port + 1,
                              params2, realtime=False)
        _wait_frames(pipe2, 12, time.time() + 20)
        switch_ok = pipe2.frames >= 10 and bool(params2)
        print(f"[2] switched -> {os.path.basename(p2)}: frames {pipe2.frames}, "
              f"params {len(params2)} -> {'OK' if switch_ok else 'FAIL'}")
        pipe2.close()
    else:
        print("[2] switch: skipped (no other presets discovered)")

    ok = ctrl_ok and switch_ok
    print("SELFTEST PASS" if ok else "SELFTEST FAIL")
    return 0 if ok else 1


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", default=None,
                    help="Video/image to stream. Optional — load one from the "
                         "Video menu in the UI.")
    ap.add_argument("--preset", default=None,
                    help="Path to a .slangp preset. Optional — load one from the "
                         "Shader menu in the UI; without one the video plays raw.")
    ap.add_argument("--shaders-dir", default=None,
                    help="Directory scanned to populate the Shader menu "
                         "(default: the --preset's shaders tree, else cwd).")
    ap.add_argument("--videos-dir", default=None,
                    help="Directory scanned to populate the Video menu "
                         "(default: the --input's folder, else cwd).")
    ap.add_argument("--params", default=None,
                    help="Initial 'k=v,k=v' param overrides (seed the sliders; "
                         "also used by --export).")
    ap.add_argument("--export", default=None, metavar="OUT",
                    help="Headless: render --input through --preset (with "
                         "--params) to an H.264 file (original audio + res/fps) "
                         "and exit.")
    ap.add_argument("--width", type=int, default=0, help="Preview width (px).")
    ap.add_argument("--height", type=int, default=0, help="Preview height (px).")
    ap.add_argument("--fps", type=int, default=30, help="Preview frame rate.")
    ap.add_argument("--control-port", type=int, default=9000)
    ap.add_argument("--slangfx", default=shutil.which("slangfx") or
                    os.path.join(here, "..", "build", "slangfx.exe"))
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    ap.add_argument("--selftest", action="store_true",
                    help="Run headless; verify live control and a shader switch. "
                         "Requires --input and --preset.")
    args = ap.parse_args()

    if args.preset and not os.path.isfile(args.preset):
        sys.exit(f"preset not found: {args.preset}")
    if args.input and not os.path.isfile(args.input):
        sys.exit(f"input not found: {args.input}")
    params = discover_params(args.preset) if args.preset else []
    if args.preset and not params:
        sys.exit(f"no #pragma parameters found in {args.preset}")

    # Seed slider defaults from --params so launch overrides are reflected.
    if args.params and params:
        ov = parse_params_str(args.params)
        for p in params:
            if p["name"] in ov:
                p["default"] = ov[p["name"]]

    if args.export:
        if not args.input:
            sys.exit("--export requires --input")
        ok, msg = export_video(
            args, os.path.abspath(args.input),
            os.path.abspath(args.preset) if args.preset else None,
            args.params, os.path.abspath(args.export))
        print(msg)
        sys.exit(0 if ok else 1)

    if args.selftest:
        if not args.input or not args.preset:
            sys.exit("--selftest requires both --input and --preset")
        w, h = preview_size(args)
        sys.exit(run_selftest(args, w, h, params))

    w, h = preview_size(args)
    run_gui(args, w, h, params)


if __name__ == "__main__":
    main()
