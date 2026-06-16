#!/usr/bin/env python3
"""Headless check for slangfx --control-port live param updates.

Feeds a STILL image through perlin-flow with speed=0 (so every rendered frame
is identical), then sends a UDP 'warp=...' update partway through. If live
control works, frames after the update differ from frames before it; without
it they'd be byte-identical. Exits non-zero on failure.
"""
import os, socket, subprocess, sys, time

W, H = 480, 270
FB = W * H * 4
PORT = 9777
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))            # beat-cut/
SLANGFX = os.path.join(HERE, "..", "build", "slangfx.exe")
PRESET = os.path.join(ROOT, "shaders", "perlin-flow", "perlin-flow.slangp")
STILL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ.get("TEMP", "/tmp"), "frame_src.png")

ff = subprocess.Popen(
    ["ffmpeg", "-loglevel", "error", "-loop", "1", "-i", STILL,
     "-t", "4", "-r", "30", "-vf", f"scale={W}:{H}",
     "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
    stdout=subprocess.PIPE)
sfx = subprocess.Popen(
    [SLANGFX, "--preset", PRESET, "--width", str(W), "--height", str(H),
     "--control-port", str(PORT),
     "--params", "speed=0.0,warp=0.0,motion_warp=0.0,trip=0.0"],
    stdin=ff.stdout, stdout=subprocess.PIPE)
ff.stdout.close()

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def read_frame():
    buf = b""
    while len(buf) < FB:
        chunk = sfx.stdout.read(FB - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf

before = after = None
sent = False
i = 0
while True:
    f = read_frame()
    if f is None:
        break
    if i == 5:
        before = f
    if i == 20 and not sent:
        sock.sendto(b"warp=0.30,trip=1.5", ("127.0.0.1", PORT))
        sent = True
        time.sleep(0.05)          # let the datagram land before more reads
    if i == 90:
        after = f
        break
    i += 1

sfx.terminate(); ff.terminate()
if before is None or after is None:
    print(f"FAIL: not enough frames (got {i})"); sys.exit(2)

diff = sum(abs(a - b) for a, b in zip(before[::997], after[::997]))  # sampled
changed = before != after
print(f"frames read: {i}, before==after: {not changed}, sampled L1 diff: {diff}")
if changed and diff > 0:
    print("PASS: live UDP param update changed the rendered output")
    sys.exit(0)
print("FAIL: output did not change after UDP update")
sys.exit(1)
