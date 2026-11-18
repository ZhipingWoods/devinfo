#!/usr/bin/env python3
"""Render captured devinfo output as terminal-window PNGs for the site.

The text is the real stdout of the binary, byte for byte -- this script only
paints it. Anything that is not in the capture file does not appear in the
image, so the screenshots cannot drift away from what the tool prints.

Where the captures came from:

  macos-m5.txt      Apple M5, macOS 26.6, arm64, this laptop
  linux-8x4090.txt  2x EPYC 7402 + 8x RTX 4090, Ubuntu 22.04, x86_64
  jetson-orin.txt   Jetson AGX Orin 64GB, L4T R36.4, aarch64
  windows-i7.txt    i7-11700, Windows 10.0.26200, x86_64. This one was
                    recovered from git history: it is the report the binary
                    wrote to devinfo-report.txt when it was double-clicked on
                    that machine, not a transcription.

                    It is the FIRST round of testing on that machine, before
                    the six fixes results.html lists, and it shows two of them:
                    amdhip64.dll is still labelled "rocm" there, and SSE4.1 /
                    SSE4.2 are missing from the ISA line. src/core.cpp now
                    emits "hip". The frame is kept because that box has not
                    been re-tested, and the README says so under the cards.

Every image is the same size, so the README cards line up. Long captures are
cut with a visible "..." row; the cut is drawn, not hidden, and the elided
range is named in JOBS below.

    python3 tools/render_shots.py

Reads  assets/captures/*.txt
Writes site/assets/*.png
"""
import pathlib
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
CAPS = ROOT / "assets" / "captures"
OUT = ROOT / "site" / "assets"

SCALE = 2            # render at 2x for retina, CSS downscales
FSIZE = 13
PAD = 22
TITLEBAR = 34
LINE_H = 19
LINES = 34           # fixed content height, in lines, for every image
COLS = 82            # fixed content width, in characters

BG = (11, 15, 22)
BG_TOP = (15, 20, 29)
FG = (214, 226, 240)
DIM = (110, 125, 148)
ACCENT = (140, 128, 255)
TEAL = (45, 212, 191)
AMBER = (240, 180, 41)
RED = (240, 91, 99)
GREEN = (36, 196, 134)

FONT = "/System/Library/Fonts/Menlo.ttc"
FONT_BOLD = "/System/Library/Fonts/Menlo.ttc"


def load_font(size, bold=False):
    try:
        return ImageFont.truetype(FONT_BOLD if bold else FONT, size, index=1 if bold else 0)
    except Exception:
        return ImageFont.truetype(FONT, size, index=0)


def classify(line):
    """Return (kind, text) where kind drives the base colour of the line."""
    s = line.rstrip("\n")
    if s.strip() == "...":
        return "cut", s
    if not s.strip():
        return "blank", s
    if re.match(r"^devinfo - ", s):
        return "banner", s
    if s.startswith("------"):
        return "rule", s
    if re.match(r"^(CPU|GPU\d|NPU\d|NPU|accelerators|deploy|WARN)\b", s) or s.strip() in (
        "accelerators",
        "deploy (derived from the measurements above, not measured)",
    ):
        return "head", s
    if s.lstrip().startswith("-- not present"):
        return "absent", s
    return "body", s


TOKEN = re.compile(r"(\s+|\S+)")
UNIT_COL = (128, 148, 178)
UNIT = re.compile(
    r"(GHz~?|MHz~?|GiB~?|MiB~?|KiB~?|GB/s~?|MT/s~?|TOPS~?|TFLOPs~?|TFLOPS~?|TF~?|"
    r"(?:gen\d+)|x\d+|%|B~?)"
)


def draw_line(d, x, y, text, kind, f_reg, f_bold):
    base = {
        "banner": TEAL,
        "rule": (45, 58, 78),
        "head": ACCENT,
        "absent": RED,
        "body": FG,
        "blank": FG,
        "cut": DIM,
    }.get(kind, FG)

    if kind in ("head", "banner", "cut"):
        d.text((x, y), text, font=f_bold if kind != "cut" else f_reg, fill=base)
        return

    # Per-token colouring. The tool marks every derived number with a trailing
    # "~", so painting the marker amber and leaving the value white splits one
    # quantity across two colours. Walk backwards: a token ending in "~" drags
    # the number in front of it into amber as well.
    toks = TOKEN.findall(text)
    amber = [False] * len(toks)
    for i, tok in enumerate(toks):
        if "~" in tok:
            amber[i] = True
            j = i - 1
            while j >= 0 and toks[j].isspace():
                j -= 1
            if j >= 0 and re.search(r"\d", toks[j]):
                amber[j] = True

    cx = x
    for i, tok in enumerate(toks):
        if tok.isspace():
            cx += d.textlength(tok, font=f_reg)
            continue
        if amber[i]:
            col = AMBER
        elif re.fullmatch(r"[A-Z][A-Za-z0-9_.+-]*", tok) and kind == "absent":
            col = RED
        elif tok in ("yes", "runtime", "driver", "lib", "true"):
            col = GREEN
        elif UNIT.fullmatch(tok):
            col = UNIT_COL
        else:
            col = base
        d.text((cx, y), tok, font=f_reg, fill=col)
        cx += d.textlength(tok, font=f_reg)


def select(lines, keep):
    """Apply the keep ranges, inserting a visible cut between them.

    keep is a list of (first, last) 1-based inclusive ranges, or None for the
    whole capture. Cutting is an editorial act, so it is drawn as "..." rather
    than silently stitched together.
    """
    if not keep:
        return lines[:LINES]
    out = []
    for n, (a, b) in enumerate(keep):
        if n:
            out.append("...")
        out.extend(lines[a - 1:b])
    return out[:LINES]


def render(src, title, out, chrome="mac", keep=None):
    raw = [ln for ln in src.read_text(encoding="utf-8", errors="replace").splitlines()
           if not ln.startswith("bash: warning:")]
    raw = select(raw, keep)

    f_reg = load_font(FSIZE * SCALE)
    f_bold = load_font(FSIZE * SCALE, bold=True)
    f_title = load_font((FSIZE - 2) * SCALE)

    # Fixed canvas: the widest line of the widest capture, and exactly LINES
    # rows of text, so every card in the README is the same picture size.
    probe = ImageDraw.Draw(Image.new("RGB", (10, 10)))
    char_w = probe.textlength("M" * COLS, font=f_reg)
    win_w = int(char_w) + PAD * 2 * SCALE
    win_h = TITLEBAR * SCALE + PAD * SCALE * 2 + LINES * LINE_H * SCALE

    img = Image.new("RGB", (win_w, win_h), BG)
    d = ImageDraw.Draw(img)

    for yy in range(win_h):
        t = yy / max(1, win_h)
        c = tuple(int(BG_TOP[i] + (BG[i] - BG_TOP[i]) * t) for i in range(3))
        d.line([(0, yy), (win_w, yy)], fill=c)

    d.rectangle([0, 0, win_w - 1, TITLEBAR * SCALE], fill=(19, 25, 36))
    cy = TITLEBAR * SCALE // 2
    if chrome == "mac":
        r = 5 * SCALE // 2
        for i, col in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
            cxx = (16 + i * 15) * SCALE
            d.ellipse([cxx - r, cy - r, cxx + r, cy + r], fill=col)
    else:
        # Windows: minimise, maximise, close, right-aligned.
        r = 5 * SCALE // 2
        for i, col in enumerate([(120, 134, 156), (120, 134, 156), (196, 43, 28)]):
            cxx = win_w - (58 - i * 18) * SCALE // 2
            d.rectangle([cxx - r, cy - r, cxx + r, cy + r], fill=col)
    d.text((80 * SCALE, cy - (FSIZE - 2) * SCALE // 2), title, font=f_title, fill=DIM)
    d.line([(0, TITLEBAR * SCALE), (win_w, TITLEBAR * SCALE)], fill=(30, 40, 55))

    y = TITLEBAR * SCALE + PAD * SCALE
    for ln in raw:
        kind, text = classify(ln)
        draw_line(d, PAD * SCALE, y, text, kind, f_reg, f_bold)
        y += LINE_H * SCALE

    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"{out.relative_to(ROOT)}  {img.width}x{img.height}  {len(raw)} lines  {title}")


def main():
    jobs = [
        ("macos-m5.txt", "devinfo - Apple M5 - macOS", "macos-m5.png", "mac", None),
        ("linux-8x4090.txt", "devinfo - 8x RTX 4090 - Linux", "linux-8x4090.png", "mac",
         [(1, 17), (53, 62)]),
        ("windows-i7.txt", "devinfo - i7-11700 - Windows", "windows-i7.png", "win", None),
        ("jetson-orin.txt", "devinfo - Jetson AGX Orin - Linux arm64", "jetson-orin.png", "mac",
         [(1, 16), (18, 28)]),
    ]
    if not CAPS.exists():
        sys.exit(f"missing {CAPS}")
    for src, title, dst, chrome, keep in jobs:
        p = CAPS / src
        if not p.exists():
            print(f"skip {src}: not captured")
            continue
        render(p, title, OUT / dst, chrome, keep)


if __name__ == "__main__":
    main()
