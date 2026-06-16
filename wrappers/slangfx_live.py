#!/usr/bin/env python3
"""slangfx_live.py — real-time shader-param playground.

Streams a video through a slang preset and shows the result in a window with
one slider per `#pragma parameter`. Dragging a slider sends a live
`name=value` update to slangfx over UDP (localhost), which applies it on the
next frame with no pipeline rebuild. Tuned a look you like? Hit "Copy params"
to get the `k=v,k=v` string for `slangfx --params` / `beat_cut --shader-params`.

Pipeline:  ffmpeg (decode+loop+scale) -> slangfx (--control-port) -> this app
Controls:  this app -> UDP name=value -> slangfx

Usage:
    pip install dearpygui numpy
    python wrappers/slangfx_live.py -i video.mp4 \
        --preset ../shaders/perlin-flow/perlin-flow.slangp [--width 1280]

Headless self-test (no window, used by CI / smoke checks):
    python wrappers/slangfx_live.py -i still_or_video --preset P --selftest
"""
import argparse
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time


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
    """Resolve preview WxH, preserving source aspect when only width is given."""
    if args.width and args.height:
        return args.width, args.height
    sw, sh = _probe_dims(args.ffmpeg, args.input)
    w = args.width or min(1280, sw)
    h = args.height or max(2, round(w * sh / sw / 2) * 2)
    return int(w), int(h)


class Pipeline:
    """ffmpeg | slangfx subprocess pair feeding RGBA frames, plus a UDP
    control channel. A reader thread keeps only the latest frame so the UI
    never blocks (newest-wins; stale frames are dropped)."""

    def __init__(self, args, w, h, params):
        self.w, self.h, self.fb = w, h, w * h * 4
        self.port = args.control_port
        self.params = params
        self.latest = None
        self.lock = threading.Lock()
        self.paused = False
        self.running = True
        self.frames = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        startup = ",".join(f"{p['name']}={p['default']}" for p in params)
        self.ff = subprocess.Popen(
            [args.ffmpeg, "-loglevel", "error", "-stream_loop", "-1",
             "-i", args.input, "-vf", f"scale={w}:{h},setsar=1",
             "-f", "rawvideo", "-pix_fmt", "rgba", "-r", str(args.fps), "-"],
            stdout=subprocess.PIPE)
        self.sfx = subprocess.Popen(
            [args.slangfx, "--preset", args.preset, "--width", str(w),
             "--height", str(h), "--control-port", str(self.port),
             "--params", startup],
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


# --------------------------------------------------------------------------
# GUI (Dear PyGui)
# --------------------------------------------------------------------------
def run_gui(args, w, h, params):
    import dearpygui.dearpygui as dpg
    import numpy as np

    pipe = Pipeline(args, w, h, params)
    dpg.create_context()

    # Dynamic texture for the live preview (RGBA float, normalized).
    blank = [0.0] * (w * h * 4)
    with dpg.texture_registry():
        tex = dpg.add_raw_texture(
            width=w, height=h, default_value=blank,
            format=dpg.mvFormat_Float_rgba, tag="preview_tex")

    def on_slider(sender, value, name):
        pipe.set_param(name, value)

    def copy_params():
        s = ",".join(f"{p['name']}={dpg.get_value('sld_' + p['name']):g}"
                     for p in params)
        dpg.set_clipboard_text(s)
        print("params:", s, flush=True)
        dpg.set_value("status", f"copied: {s}")

    def reset_params():
        for p in params:
            dpg.set_value("sld_" + p["name"], p["default"])
            pipe.set_param(p["name"], p["default"])
        dpg.set_value("status", "reset to defaults")

    def toggle_pause():
        pipe.paused = not pipe.paused
        dpg.set_value("status", "paused" if pipe.paused else "running")

    with dpg.window(tag="main"):
        with dpg.group(horizontal=True):
            dpg.add_image("preview_tex", width=w, height=h)
            with dpg.child_window(width=320, autosize_y=True):
                dpg.add_text(os.path.basename(args.preset))
                dpg.add_separator()
                for p in params:
                    dpg.add_slider_float(
                        label=p["label"] or p["name"], tag="sld_" + p["name"],
                        default_value=p["default"], min_value=p["min"],
                        max_value=p["max"], callback=on_slider,
                        user_data=p["name"], width=180)
                dpg.add_separator()
                with dpg.group(horizontal=True):
                    dpg.add_button(label="Copy params", callback=copy_params)
                    dpg.add_button(label="Reset", callback=reset_params)
                    dpg.add_button(label="Pause", callback=toggle_pause)
                dpg.add_text("", tag="status")

    dpg.create_viewport(title="slangfx live", width=w + 360, height=h + 60)
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("main", True)

    scale = np.float32(1.0 / 255.0)
    while dpg.is_dearpygui_running():
        buf = pipe.get_latest()
        if buf is not None:
            arr = np.frombuffer(buf, dtype=np.uint8).astype(np.float32) * scale
            dpg.set_value("preview_tex", arr)
        if not pipe.running:
            dpg.set_value("status", "stream ended")
        dpg.render_dearpygui_frame()

    pipe.close()
    dpg.destroy_context()


# --------------------------------------------------------------------------
# Headless self-test: prove frames flow and a live UDP update changes output.
# --------------------------------------------------------------------------
def _l1(a, b):
    """Sampled L1 distance between two RGBA buffers (every ~1KB)."""
    return sum(abs(x - y) for x, y in zip(a[::997], b[::997]))


def _wait_frames(pipe, n, deadline):
    n0 = pipe.frames
    while pipe.frames < n0 + n and time.time() < deadline and pipe.running:
        time.sleep(0.02)


def run_selftest(args, w, h, params):
    """Exercise the full Python path (param discovery, ffmpeg|slangfx spawn,
    frame read, UDP control) and prove a live update changes the render.

    Method: drive amount=0 (both shaders define this as an exact passthrough,
    `mix(cur,col,0)=cur`, which is genuinely static), confirm two later frames
    are byte-identical, then drive amount=1 and confirm the render changes
    sharply. This isolates the live UDP update from the shader's own feedback
    animation — the only thing changing the frame is the param we sent."""
    if not os.path.isfile(args.slangfx):
        print(f"SELFTEST FAIL: slangfx not found at {args.slangfx}"); return 2
    if not any(p["name"] == "amount" for p in params):
        print("SELFTEST SKIP: preset has no 'amount' param"); return 0
    pipe = Pipeline(args, w, h, params)
    deadline = time.time() + 25

    # slangfx binds its UDP control port only after Vulkan init, so wait for
    # frames to flow (= it's up and listening) before sending control packets.
    _wait_frames(pipe, 5, deadline)
    pipe.set_param("speed", 0.0)
    pipe.set_param("amount", 0.0)           # exact passthrough -> static
    _wait_frames(pipe, 30, deadline)
    a = pipe.get_latest()
    _wait_frames(pipe, 8, deadline)
    a2 = pipe.get_latest()

    pipe.set_param("amount", 1.0)           # full effect
    _wait_frames(pipe, 12, deadline)
    b = pipe.get_latest()
    pipe.close()

    if not all(x is not None for x in (a, a2, b)):
        print(f"SELFTEST FAIL: insufficient frames (got {pipe.frames})"); return 2

    d_static = _l1(a, a2)
    d_change = _l1(a, b)
    print(f"params discovered: {len(params)} "
          f"({', '.join(p['name'] for p in params)})")
    print(f"frames flowed: {pipe.frames}, preview {w}x{h}")
    print(f"passthrough drift (amount=0): {d_static}; "
          f"change after amount 0->1: {d_change}")
    if d_static == 0 and d_change > 1000:
        print("SELFTEST PASS: live UDP update changed the render "
              "(static baseline confirmed)")
        return 0
    print("SELFTEST FAIL: baseline not static or update had no clear effect")
    return 1


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
                    help="Run headless; verify frames flow and live update works.")
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
