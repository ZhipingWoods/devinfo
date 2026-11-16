#!/usr/bin/env python3
"""Render captured devinfo output as terminal-window PNGs for the site.

The text is the real stdout of the binary, byte for byte -- this script only
paints it. Anything that is not in the capture file does not appear in the
image, so the screenshots cannot drift away from what the tool prints.

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
    if re.match(r"^\s+\w", s) or s.startswith("     "):
        return "body", s
    return "body", s


TOKEN = re.compile(r"(\s+|\S+)")
UNIT_COL = (128, 148, 178)
UNIT = re.compile(
    r"(GHz~?|MHz~?|GiB~?|MiB~?|KiB~?|GB/s~?|MT/s~?|TOPS~?|TFLOPs~?|TFLOPS~?|TF~?|"
    r"(?:gen\d+)|x\d+|%|B~?)"
)


def draw_line(d, x, y, text, kind, f_reg, f_bold, width):
    base = {
        "banner": TEAL,
        "rule": (45, 58, 78),
        "head": ACCENT,
        "absent": RED,
        "body": FG,
        "blank": FG,
    }.get(kind, FG)

    if kind == "head":
        d.text((x, y), text, font=f_bold, fill=ACCENT)
        return
    if kind == "banner":
        d.text((x, y), text, font=f_bold, fill=TEAL)
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


def render(src: pathlib.Path, title: str, out: pathlib.Path, max_lines=None):
    raw = src.read_text(encoding="utf-8", errors="replace").splitlines()
    raw = [ln for ln in raw if not ln.startswith("bash: warning:")]
    if max_lines and len(raw) > max_lines:
        raw = raw[:max_lines]

    f_reg = load_font(FSIZE * SCALE)
    f_bold = load_font(FSIZE * SCALE, bold=True)
    f_title = load_font((FSIZE - 2) * SCALE)

    probe = ImageDraw.Draw(Image.new("RGB", (10, 10)))
    text_w = max(probe.textlength(ln, font=f_reg) for ln in raw) if raw else 400
    win_w = int(text_w) + PAD * 2 * SCALE
    win_w = max(win_w, int(probe.textlength(title, font=f_reg)) + PAD * 2 * SCALE)
    body_h = len(raw) * LINE_H * SCALE
    win_h = TITLEBAR * SCALE + PAD * SCALE + body_h + PAD * SCALE

    img = Image.new("RGB", (win_w, win_h), BG)
    d = ImageDraw.Draw(img)

    for yy in range(win_h):
        t = yy / max(1, win_h)
        c = tuple(int(BG_TOP[i] + (BG[i] - BG_TOP[i]) * t) for i in range(3))
        d.line([(0, yy), (win_w, yy)], fill=c)

    d.rectangle([0, 0, win_w - 1, TITLEBAR * SCALE], fill=(19, 25, 36))
    cy = TITLEBAR * SCALE // 2
    r = 5 * SCALE // 2
    for i, col in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        cxx = (16 + i * 15) * SCALE
        d.ellipse([cxx - r, cy - r, cxx + r, cy + r], fill=col)
    d.text((80 * SCALE, cy - (FSIZE - 2) * SCALE // 2), title, font=f_title, fill=DIM)
    d.line([(0, TITLEBAR * SCALE), (win_w, TITLEBAR * SCALE)], fill=(30, 40, 55))

    y = TITLEBAR * SCALE + PAD * SCALE
    for ln in raw:
        kind, text = classify(ln)
        draw_line(d, PAD * SCALE, y, text, kind, f_reg, f_bold, win_w)
        y += LINE_H * SCALE

    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"{out.relative_to(ROOT)}  {img.width}x{img.height}  {len(raw)} lines")


def main():
    jobs = [
        ("macos-m5.txt", "devinfo · Apple M5 · macOS", "macos-m5.png"),
        ("linux-8x4090.txt", "devinfo · 2x EPYC 7402 + 8x RTX 4090 · Linux", "linux-8x4090.png"),
        ("jetson-orin.txt", "devinfo · Jetson AGX Orin · Linux arm64", "jetson-orin.png"),
    ]
    if not CAPS.exists():
        sys.exit(f"missing {CAPS}")
    for src, title, dst in jobs:
        p = CAPS / src
        if not p.exists():
            print(f"skip {src}: not captured")
            continue
        render(p, title, OUT / dst)


if __name__ == "__main__":
    main()
