#!/usr/bin/env python3
"""Generate the split-fish line art used at the bottom of the reel player skin.

Modelled on docs/fish.mov: a fish cut in two. The head half faces left with the cut
behind the gills; the tail half shows the oval cross-section (flesh rings and spine)
on its left side. The firmware draws a sound-wave of vertical bars between them.

Like the reference, both halves sway gently: each is pre-rendered as FRAMES frames
across its swing (head tilt; tail tilt plus a larger caudal-fin flex), and the
firmware switches frames, so no runtime rotation buffers are needed.

Each frame is an LVGL A8 (alpha-only) image, recoloured at runtime, so it costs one
byte per pixel of flash and no RAM. Drawn at 4x and downsampled for anti-aliasing.
Pass --preview to also write a PNG composited on the skin's green background.
Requires Pillow.
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets/images/bp_fish.c"
SS = 4
STROKE = 1.25
SCALE = 0.8             # design units -> screen pixels
PAD = 3                 # room for the swing
FRAMES = 9              # frames across the swing, from -1 to +1
HEAD_DEG = 2.5          # head tilt at the ends of the swing
TAIL_DEG = 2.5          # tail-body tilt
FIN_DEG = 7.0           # extra caudal-fin flex about the peduncle


def rotate(pt, pivot, deg):
    a = math.radians(deg)
    x, y = pt[0] - pivot[0], pt[1] - pivot[1]
    return (pivot[0] + x * math.cos(a) - y * math.sin(a), pivot[1] + x * math.sin(a) + y * math.cos(a))


class Pen:
    """Draws in design units; points are rotated about `pivot` by `deg`, and while
    `fin` is set, first about the fin pivot by `fin_deg`."""

    def __init__(self, w: int, h: int, pivot, deg=0.0, fin_pivot=None, fin_deg=0.0):
        self.size = (math.ceil(w * SCALE) + 2 * PAD, math.ceil(h * SCALE) + 2 * PAD)
        self.img = Image.new("L", (self.size[0] * SS, self.size[1] * SS), 0)
        self.d = ImageDraw.Draw(self.img)
        self.pivot, self.deg = pivot, deg
        self.fin_pivot, self.fin_deg, self.fin = fin_pivot, fin_deg, False

    def _p(self, pts):
        out = []
        for pt in pts:
            if self.fin:
                pt = rotate(pt, self.fin_pivot, self.fin_deg)
            x, y = rotate(pt, self.pivot, self.deg)
            out.append(((x * SCALE + PAD) * SS, (y * SCALE + PAD) * SS))
        return out

    def line(self, pts, width=STROKE):
        self.d.line(self._p(pts), fill=255, width=round(width * 0.9 * SS), joint="curve")

    def curve(self, p0, p1, p2, p3, width=STROKE, n=16):
        pts = []
        for i in range(n + 1):
            t = i / n
            u = 1 - t
            pts.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                        u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
        self.line(pts, width)

    def ellipse(self, cx, cy, rx, ry, width=STROKE, fill=False):
        pts = [(cx + rx * math.cos(i * math.pi / 24), cy + ry * math.sin(i * math.pi / 24)) for i in range(49)]
        if fill:
            self.d.polygon(self._p(pts), fill=255)
        else:
            self.line(pts, width)

    def image(self) -> Image.Image:
        return self.img.resize(self.size, Image.LANCZOS)


def head(swing: float) -> Image.Image:
    """Head half, facing left; the cut is on the right (x ~ 50)."""
    p = Pen(58, 46, pivot=(30, 24), deg=HEAD_DEG * swing)
    # Back and belly outline from the snout to the cut.
    p.curve((4, 23), (10, 15), (22, 10), (34, 10))
    p.curve((34, 10), (40, 10), (46, 10), (50, 11))
    p.curve((4, 26), (8, 31), (18, 35), (30, 36))
    p.curve((30, 36), (38, 37), (45, 37), (50, 36))
    # Ragged cut edge.
    p.line([(50, 11), (51, 15), (49.5, 19), (51, 24), (49.5, 29), (51, 33), (50, 36)])
    # Mouth: upper lip, lower jaw, corner.
    p.line([(2.5, 24), (6, 23.5), (10, 25)])
    p.line([(3, 26.5), (7, 27.5), (10, 25)])
    # Eye and pupil.
    p.ellipse(14, 20, 3.6, 3.6)
    p.ellipse(14.3, 20, 1.5, 1.5, fill=True)
    # Gill covers.
    p.curve((24, 11), (19, 18), (19, 28), (25, 35))
    p.curve((28, 12), (24, 19), (24, 27), (29, 34), width=1.0)
    # Cheek lines.
    p.curve((12, 28), (15, 30), (19, 30), (21, 28), width=0.9)
    # Dorsal fin: spiny, raked back.
    base = [(31, 10.5), (35, 10), (39, 10), (43, 10.2), (47, 10.6)]
    tips = [(34, 1.5), (38, 1), (42, 2.5), (45.5, 3.5), (49.5, 6)]
    for b, t in zip(base, tips):
        p.line([b, t], width=1.0)
    membrane = [base[0]]
    for i, t in enumerate(tips):   # membrane sags between the spines
        membrane.append(t)
        if i + 1 < len(tips):
            membrane.append(((t[0] + tips[i + 1][0]) / 2 + 0.5, (t[1] + tips[i + 1][1]) / 2 + 2.5))
    p.line(membrane, width=0.9)
    # Pectoral fin fanning back across the cut.
    p.curve((30, 23), (38, 19), (48, 18), (56, 19.5), width=1.0)
    p.curve((30, 26), (38, 27), (48, 28), (55, 27.5), width=1.0)
    p.curve((56, 19.5), (57.5, 22), (57.5, 25), (55, 27.5), width=1.0)
    for t in [(56.5, 21.5), (57, 24), (56, 26)]:
        p.line([(32, 24.5), t], width=0.6)
    # Pelvic fin pointing down and back.
    p.line([(30, 36), (37, 44), (40, 37)])
    p.line([(33, 37), (37, 43)], width=0.8)
    return p.image()


def tail(swing: float) -> Image.Image:
    """Tail half; the oval cut face is on the left, the caudal fin on the right."""
    p = Pen(64, 40, pivot=(14, 20), deg=TAIL_DEG * swing, fin_pivot=(49, 20), fin_deg=FIN_DEG * swing)
    cx, cy = 8, 20
    # Cut face: outer skin, flesh ring, spine and the two muscle blocks.
    p.ellipse(cx, cy, 6.5, 15)
    p.ellipse(cx, cy, 4.2, 11, width=1.0)
    p.ellipse(cx, cy, 1.8, 1.8, fill=True)
    p.ellipse(cx, cy - 6, 1.8, 2.8, width=0.9)
    p.ellipse(cx, cy + 6, 1.8, 2.8, width=0.9)
    # Body outline tapering to the caudal peduncle.
    p.curve((8, 5), (22, 5), (38, 7), (48, 14))
    p.curve((8, 35), (22, 35), (38, 33), (48, 26))
    # Soft dorsal fin along the back.
    p.curve((12, 5), (20, 1.5), (32, 2), (42, 9), width=1.0)
    for x in range(15, 41, 4):
        y0 = 5 + (x - 12) * 0.08
        p.line([(x, y0), (x + 2, y0 - 3.2 + (x - 15) * 0.07)], width=0.8)
    # Anal fin.
    p.line([(32, 33.5), (37, 38), (42, 31)], width=1.0)
    p.line([(36, 32.5), (38, 37)], width=0.8)
    # Lateral line and a few scale marks.
    p.curve((15, 21), (25, 22), (36, 21), (47, 20), width=0.8)
    for x, y in ((19, 26), (24, 27), (30, 26), (35, 27)):
        p.line([(x, y), (x + 2.5, y)], width=0.8)
        p.line([(x + 1.2, y - 1.5), (x + 1.2, y + 1)], width=0.8)
    # Caudal fin: fan from the peduncle, lobed trailing edge, with rays; it flexes.
    p.fin = True
    p.line([(48, 14), (51, 13.5)])
    p.line([(48, 26), (51, 26.5)])
    p.line([(51, 13.5), (62, 3)])
    p.line([(51, 26.5), (62, 37)])
    p.curve((62, 3), (59, 12), (59, 16), (60.5, 20))
    p.curve((60.5, 20), (59, 24), (59, 28), (62, 37))
    for ty in (6, 10, 14, 18, 22, 26, 30, 34):
        tx = 61.5 - 2.2 * (1 - abs(ty - 20) / 17) if ty not in (6, 34) else 61
        p.line([(51, 20 + (ty - 20) * 0.32), (tx, ty)], width=0.7)
    p.fin = False
    return p.image()


def emit(name: str, frames: list[Image.Image]) -> list[str]:
    w, h = frames[0].size
    out = []
    for i, img in enumerate(frames):
        data = img.tobytes()
        rows = [", ".join(f"0x{b:02x}" for b in data[j:j + 24]) for j in range(0, len(data), 24)]
        out.append(f"static const uint8_t {name}_{i}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "\n};")
    out.append(f"const lv_image_dsc_t {name}[{len(frames)}] = {{")
    for i in range(len(frames)):
        out.append(f"    {{.header.magic = LV_IMAGE_HEADER_MAGIC, .header.cf = LV_COLOR_FORMAT_A8,"
                   f" .header.w = {w}, .header.h = {h}, .header.stride = {w},"
                   f" .data_size = sizeof({name}_{i}), .data = {name}_{i}}},")
    out += ["};", ""]
    return out


def main() -> None:
    swings = [-1 + 2 * i / (FRAMES - 1) for i in range(FRAMES)]
    parts = {"bp_fish_head": [head(sw) for sw in swings], "bp_fish_tail": [tail(sw) for sw in swings]}
    lines = ["// Generated by tools/generate_fish.py. Do not edit.", '#include "lvgl.h"', ""]
    for name, frames in parts.items():
        lines += emit(name, frames)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines))
    total = sum(f.size[0] * f.size[1] for fr in parts.values() for f in fr)
    print(f"bp_fish: {FRAMES} frames, head {parts['bp_fish_head'][0].size} tail {parts['bp_fish_tail'][0].size},"
          f" {total} bytes -> {OUT.relative_to(ROOT)}")
    if "--preview" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--preview") + 1])
        sheet = Image.new("RGB", (240, 3 * 56), (0x20, 0xE4, 0x7C))
        for row, fi in enumerate((0, FRAMES // 2, FRAMES - 1)):
            for name, x, y in (("bp_fish_head", 17, 6), ("bp_fish_tail", 165, 9)):
                mask = parts[name][fi]
                sheet.paste(Image.new("RGB", mask.size, (255, 255, 255)), (x, y + row * 56), mask)
            d = ImageDraw.Draw(sheet)
            import random
            random.seed(row)
            for i in range(15):
                bx = 74 + i * 6
                hh = random.choice([1, 4, 7, 10, 12])
                d.rounded_rectangle([bx, row * 56 + 28 - hh, bx + 1, row * 56 + 28 + hh], radius=1, fill=(255, 255, 255))
        sheet.resize((960, 3 * 224), Image.NEAREST).save(out)


if __name__ == "__main__":
    main()
