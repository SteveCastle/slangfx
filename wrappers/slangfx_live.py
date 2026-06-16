#!/usr/bin/env python3
"""slangfx_live.py — real-time shader-param playground.

Streams a video through a slang preset and shows the result in a window with
one slider per `#pragma parameter`. Dragging a slider sends a live
`name=value` update to slangfx over UDP (localhost), applied on the next frame
with no pipeline rebuild. The menu bar switches shader or video on the fly
(the ffmpeg|slangfx pair is restarted and the sliders rebuilt). Tuned a look
you like? "Copy params" gives the `k=v,k=v` string for `slangfx --params` /
`beat_cut --shader-params`.

Pipeline:  ffmpeg (decode+loop+letterbox) -> slangfx (--control-port) -> app
Controls:  app -> UDP name=value -> slangfx
Menu:      Shader / Video (quick-pick discovered files + Browse...), Params

Usage:
    pip install dearpygui numpy
    python wrappers/slangfx_live.py -i video.mp4 \
        --preset ../shaders/perlin-flow/perlin-flow.slangp [--width 1280]

Headless self-test (no window): verifies live control + a shader switch.
    python wrappers/slangfx_live.py -i still_or_video --preset P --selftest
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
def find_presets(start_preset):
    """All .slangp presets under the shaders tree that holds `start_preset`
    (its grandparent dir, e.g. shaders/), sorted."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(start_preset)))
    found = []
    for dp, _, files in os.walk(root):
        for f in files:
            if f.endswith(".slangp"):
                found.append(os.path.join(dp, f))
    return sorted(set(found))


def find_videos(start_video):
    """Sibling videos in the folder of `start_video`, sorted."""
    root = os.path.dirname(os.path.abspath(start_video))
    try:
        names = os.listdir(root)
    except OSError:
        return []
    return sorted(os.path.join(root, f) for f in names
                  if f.lower().endswith(VIDEO_EXTS))


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
    """Resolve preview WxH, preserving source aspect when only width is given.
    Fixed for the session: every source is letterboxed into this size, so
    switching to a different-aspect video needs no texture recreation."""
    if args.width and args.height:
        return args.width, args.height
    sw, sh = _probe_dims(args.ffmpeg, args.input)
    w = args.width or min(1280, sw)
    h = args.height or max(2, round(w * sh / sw / 2) * 2)
    return int(w), int(h)


class Pipeline:
    """ffmpeg | slangfx subprocess pair feeding RGBA frames, plus a UDP
    control channel. A reader thread keeps only the latest frame so the UI
    never blocks (newest-wins; stale frames are dropped). One Pipeline streams
    one (video, preset) pair; switching builds a fresh Pipeline."""

    def __init__(self, slangfx, ffmpeg, video, preset, w, h, fps, port, params):
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
        startup = ",".join(f"{p['name']}={p['default']}" for p in params)
        self.ff = subprocess.Popen(
            [ffmpeg, "-loglevel", "error", "-stream_loop", "-1",
             "-i", video, "-vf", vf,
             "-f", "rawvideo", "-pix_fmt", "rgba", "-r", str(fps), "-"],
            stdout=subprocess.PIPE)
        self.sfx = subprocess.Popen(
            [slangfx, "--preset", preset, "--width", str(w), "--height", str(h),
             "--control-port", str(port), "--params", startup],
            stdin=self.ff.stdout, stdout=subprocess.PIPE)
        self.ff.stdout.close()
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        rd = self.sfx.stdout
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
        try:
            self.sock.sendto(f"{name}={value}".encode(), ("127.0.0.1", self.port))
        except OSError:
            pass

    def close(self):
        self.running = False
        for p in (self.sfx, self.ff):
            try:
                p.terminate()
            except Exception:
                pass


def make_pipeline(args, video, preset, w, h, port, params):
    return Pipeline(args.slangfx, args.ffmpeg, video, preset,
                    w, h, args.fps, port, params)


# --------------------------------------------------------------------------
# GUI (Dear PyGui)
# --------------------------------------------------------------------------
def run_gui(args, w, h, params):
    import dearpygui.dearpygui as dpg
    import numpy as np

    presets = find_presets(args.preset)
    videos = find_videos(args.input)

    state = {
        "video": os.path.abspath(args.input),
        "preset": os.path.abspath(args.preset),
        "params": params,
        "port": args.control_port,
        "pipe": None,
    }

    dpg.create_context()
    blank = [0.0] * (w * h * 4)
    with dpg.texture_registry():
        dpg.add_raw_texture(width=w, height=h, default_value=blank,
                            format=dpg.mvFormat_Float_rgba, tag="preview_tex")

    # --- param controls ---------------------------------------------------
    def on_slider(sender, value, name):
        if state["pipe"]:
            state["pipe"].set_param(name, value)

    def copy_params():
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
        dpg.set_value("status", "reset to defaults")

    def toggle_pause():
        p = state["pipe"]
        if p:
            p.paused = not p.paused
            dpg.set_value("status", "paused" if p.paused else "running")

    def rebuild_sliders():
        dpg.delete_item("sliders", children_only=True)
        dpg.set_value("preset_label", os.path.basename(state["preset"]))
        for p in state["params"]:
            dpg.add_slider_float(
                parent="sliders", label=p["label"] or p["name"],
                tag="sld_" + p["name"], default_value=p["default"],
                min_value=p["min"], max_value=p["max"], callback=on_slider,
                user_data=p["name"], width=180)

    # --- switching --------------------------------------------------------
    def start(video=None, preset=None):
        if video:
            state["video"] = os.path.abspath(video)
        if preset:
            new_params = discover_params(os.path.abspath(preset))
            if not new_params:
                dpg.set_value("status",
                              f"no #pragma params in {os.path.basename(preset)}")
                return
            state["preset"] = os.path.abspath(preset)
            state["params"] = new_params
        if state["pipe"]:
            state["pipe"].close()
        port = state["port"]
        state["port"] += 1                     # fresh port avoids any bind race
        state["pipe"] = make_pipeline(args, state["video"], state["preset"],
                                      w, h, port, state["params"])
        rebuild_sliders()
        dpg.set_value("status", f"{os.path.basename(state['preset'])}  |  "
                                f"{os.path.basename(state['video'])}")

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

    # --- window + menu ----------------------------------------------------
    with dpg.window(tag="main"):
        with dpg.menu_bar():
            with dpg.menu(label="Shader"):
                for pth in presets:
                    dpg.add_menu_item(
                        label=os.path.basename(pth),
                        callback=lambda s, a, u: start(preset=u), user_data=pth)
                dpg.add_separator()
                dpg.add_menu_item(label="Browse .slangp...",
                                  callback=lambda: dpg.show_item("dlg_shader"))
            with dpg.menu(label="Video"):
                for pth in videos:
                    dpg.add_menu_item(
                        label=os.path.basename(pth),
                        callback=lambda s, a, u: start(video=u), user_data=pth)
                dpg.add_separator()
                dpg.add_menu_item(label="Browse video...",
                                  callback=lambda: dpg.show_item("dlg_video"))
            with dpg.menu(label="Params"):
                dpg.add_menu_item(label="Copy params", callback=copy_params)
                dpg.add_menu_item(label="Reset to defaults", callback=reset_params)
                dpg.add_menu_item(label="Pause / resume", callback=toggle_pause)

        with dpg.group(horizontal=True):
            dpg.add_image("preview_tex", width=w, height=h)
            with dpg.child_window(width=340, autosize_y=True):
                dpg.add_text(os.path.basename(state["preset"]), tag="preset_label")
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

    start()   # launch the initial pipeline + build its sliders

    scale = np.float32(1.0 / 255.0)
    while dpg.is_dearpygui_running():
        p = state["pipe"]
        if p:
            buf = p.get_latest()
            if buf is not None:
                arr = np.frombuffer(buf, dtype=np.uint8).astype(np.float32) * scale
                dpg.set_value("preview_tex", arr)
            if not p.running:
                dpg.set_value("status", "stream ended (switch shader/video to restart)")
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

    # 1) Live control on the launch preset.
    pipe = make_pipeline(args, args.input, args.preset, w, h, args.control_port, params)
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
    others = [p for p in find_presets(args.preset)
              if os.path.abspath(p) != os.path.abspath(args.preset)]
    switch_ok = True
    if others:
        p2 = others[0]
        params2 = discover_params(p2)
        pipe2 = make_pipeline(args, args.input, p2, w, h, args.control_port + 1, params2)
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
    ap.add_argument("-i", "--input", required=True, help="Video/image to stream.")
    ap.add_argument("--preset", required=True, help="Path to a .slangp preset.")
    ap.add_argument("--width", type=int, default=0, help="Preview width (px).")
    ap.add_argument("--height", type=int, default=0, help="Preview height (px).")
    ap.add_argument("--fps", type=int, default=30, help="Preview frame rate.")
    ap.add_argument("--control-port", type=int, default=9000)
    ap.add_argument("--slangfx", default=shutil.which("slangfx") or
                    os.path.join(here, "..", "build", "slangfx.exe"))
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    ap.add_argument("--selftest", action="store_true",
                    help="Run headless; verify live control and a shader switch.")
    args = ap.parse_args()

    if not os.path.isfile(args.preset):
        sys.exit(f"preset not found: {args.preset}")
    params = discover_params(args.preset)
    if not params:
        sys.exit(f"no #pragma parameters found in {args.preset}")
    w, h = preview_size(args)

    if args.selftest:
        sys.exit(run_selftest(args, w, h, params))
    run_gui(args, w, h, params)


if __name__ == "__main__":
    main()
