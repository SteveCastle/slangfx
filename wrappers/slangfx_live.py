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
    tree holding --preset (its grandparent), else the cwd."""
    if args.shaders_dir:
        return args.shaders_dir
    if args.preset:
        return os.path.dirname(os.path.dirname(os.path.abspath(args.preset)))
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
def export_video(args, video, preset, params_str, out_path):
    """Render `video` through `preset` (with `params_str`) to an H.264 file at
    the source's native resolution/fps, copying the original audio. Reuses
    wrappers/slangfx.py for the shaded path; a plain libx264 transcode when no
    preset is loaded. Blocking; returns (ok, message)."""
    here = os.path.dirname(os.path.abspath(__file__))
    if preset:
        cmd = [sys.executable, os.path.join(here, "slangfx.py"),
               "-i", video, "-o", out_path, "--preset", preset,
               "--slangfx", args.slangfx, "--ffmpeg", args.ffmpeg]
        if params_str:
            cmd += ["--params", params_str]
    else:
        # No shader: straight H.264 transcode, copy audio, keep res/fps.
        cmd = [args.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
               "-i", video, "-map", "0:v:0", "-map", "0:a?",
               "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
               "-pix_fmt", "yuv420p", "-c:a", "copy", out_path]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except Exception as e:
        return False, f"export error: {e}"
    if r.returncode == 0 and os.path.isfile(out_path):
        return True, f"exported -> {out_path}"
    tail = (r.stderr or r.stdout or "").strip().splitlines()
    return False, "export failed: " + (tail[-1] if tail else f"rc={r.returncode}")


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
    export_state = {"busy": False, "msg": None, "shown": None}

    def current_params_string():
        return ",".join(f"{p['name']}={dpg.get_value('sld_' + p['name']):g}"
                        for p in state["params"])

    def run_export(out_path):
        if not state["video"]:
            dpg.set_value("status", "load a video before exporting")
            return
        if export_state["busy"]:
            dpg.set_value("status", "an export is already running")
            return
        video, preset = state["video"], state["preset"]
        params_str = current_params_string()

        def worker():
            export_state["busy"] = True
            export_state["msg"] = (f"exporting -> {os.path.basename(out_path)} "
                                   "(full resolution; this can take a while)...")
            ok, msg = export_video(args, video, preset, params_str, out_path)
            export_state["msg"] = msg
            export_state["busy"] = False
            print(msg, flush=True)

        threading.Thread(target=worker, daemon=True).start()

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
            run_export(f)

    # --- switching --------------------------------------------------------
    def start(video=_UNSET, preset=_UNSET):
        """(Re)build the pipeline for the current video/preset. Either arg may
        be a path, None (explicitly clear), or omitted (keep current)."""
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
                    callback=lambda: run_export(
                        default_export_path(state["video"], state["preset"])
                        if state["video"] else ""))
                dpg.add_menu_item(label="Export as...",
                                  callback=open_export_dialog)

        with dpg.group(horizontal=True):
            dpg.add_image("preview_tex", width=w, height=h)
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
        p = state["pipe"]
        if p:
            buf = p.get_latest()
            if buf is not None:
                arr = np.frombuffer(buf, dtype=np.uint8).astype(np.float32) * scale
                dpg.set_value("preview_tex", arr)
            if not p.running and p.frames == 0:
                dpg.set_value("status", "could not open source (check the file)")
        # Surface export progress/result from the worker thread.
        if export_state["msg"] and export_state["msg"] != export_state["shown"]:
            dpg.set_value("status", export_state["msg"])
            export_state["shown"] = export_state["msg"]
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
