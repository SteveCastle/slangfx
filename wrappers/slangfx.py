#!/usr/bin/env python3
"""
slangfx.py - convenience wrapper around the slangfx binary.

Pipes:
    ffmpeg (decode -> rawvideo) -> slangfx (apply slang shaders) -> ffmpeg (encode)

This script handles dimension probing, pixel-format conversion, audio
passthrough, and writes a normal output container. Audio is taken from the
input file unchanged.

Usage:
    python slangfx.py -i in.mp4 --preset crt/newpixie.slangp -o out.mp4

If you'd rather run the pipes manually:
    ffmpeg -i in.mp4 -f rawvideo -pix_fmt rgba - \
      | slangfx --preset crt/newpixie.slangp --width W --height H \
      | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -framerate FR -i - \
              -i in.mp4 -map 0:v -map 1:a -c:v libx264 -c:a copy out.mp4
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


def probe(path: Path) -> tuple[int, int, str]:
    r = subprocess.run(
        [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=width,height,r_frame_rate",
            "-of", "json", str(path),
        ],
        capture_output=True, text=True, check=True,
    )
    data = json.loads(r.stdout)
    s = data["streams"][0]
    return int(s["width"]), int(s["height"]), s["r_frame_rate"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-i", "--input", required=True, type=Path)
    ap.add_argument("-o", "--output", required=True, type=Path)
    ap.add_argument("--preset", required=True, type=Path,
                    help="Path to .slangp preset file.")
    ap.add_argument("--params", default=None,
                    help="Comma-separated #pragma parameter overrides (k=v,k=v).")
    ap.add_argument("--slangfx", default="slangfx",
                    help="Path to the slangfx binary. Defaults to PATH lookup.")
    ap.add_argument("--ffmpeg", default="ffmpeg",
                    help="Path to ffmpeg binary. Defaults to PATH lookup.")
    ap.add_argument("--vcodec", default="libx264",
                    help="Output video codec. Default libx264.")
    ap.add_argument("--crf", default="20", help="x264 CRF (0-51). Default 20.")
    ap.add_argument("--preset-x264", default="veryfast",
                    help="x264 preset. Default veryfast.")
    args = ap.parse_args()

    if shutil.which(args.ffmpeg) is None:
        sys.exit(f"ffmpeg not found at '{args.ffmpeg}'")
    if shutil.which(args.slangfx) is None and not Path(args.slangfx).exists():
        sys.exit(f"slangfx not found at '{args.slangfx}' (build with `meson compile -C build`)")

    w, h, fr = probe(args.input)
    print(f"input: {w}x{h} @ {fr}", file=sys.stderr)

    decode = [
        args.ffmpeg, "-hide_banner", "-loglevel", "error",
        "-i", str(args.input),
        "-f", "rawvideo", "-pix_fmt", "rgba", "-",
    ]

    process = [args.slangfx, "--preset", str(args.preset),
               "--width", str(w), "--height", str(h)]
    if args.params:
        process += ["--params", args.params]

    encode = [
        args.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgba",
        "-video_size", f"{w}x{h}", "-framerate", fr,
        "-i", "-",
        "-i", str(args.input),
        "-map", "0:v", "-map", "1:a?",
        "-c:v", args.vcodec, "-preset", args.preset_x264, "-crf", args.crf,
        "-pix_fmt", "yuv420p",
        "-c:a", "copy", "-shortest",
        str(args.output),
    ]

    print("running pipeline...", file=sys.stderr)
    p1 = subprocess.Popen(decode, stdout=subprocess.PIPE)
    p2 = subprocess.Popen(process, stdin=p1.stdout, stdout=subprocess.PIPE)
    if p1.stdout:
        p1.stdout.close()
    p3 = subprocess.Popen(encode, stdin=p2.stdout)
    if p2.stdout:
        p2.stdout.close()
    rc = p3.wait()
    p2.wait()
    p1.wait()
    if rc != 0:
        sys.exit(f"encode pipeline returned {rc}")
    print(f"wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
