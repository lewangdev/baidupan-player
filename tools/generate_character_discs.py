#!/usr/bin/env python3
"""Generate the character reel discs for the character tape skins.

Each disc is a 76x76 RGB565A8 LVGL image: an outer flange ring at the reel radius,
a transparent band (the tape pack drawn behind it shows through as the reel fills)
and a round plate with a cartoon face. The faces are original fan-style drawings
(a blue pup, an orange pup and a pink pig) made here with plain shapes; no external
artwork is used.

A face has no rotational symmetry, so FRAMES frames cover a full turn; the firmware
switches frames, which needs no runtime transform buffers. A THUMB-pixel still of
each face is also emitted for the skin picker. Drawn at SS x and
downsampled for anti-aliasing. Pass --preview <png> to write a contact sheet.
Requires Pillow.
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets/images/bp_char_discs.c"
SIZE, SS = 76, 8
FRAMES = 36             # 10 degrees per frame over a full turn
THUMB = 36              # skin-picker thumbnail size
INK = (0x09, 0x21, 0x13, 255)
R_FLANGE = 37.0         # outer flange ring (matches the classic tape disc)
R_PLATE = 33.0          # face plate; the band between shows the tape pack when nearly full


def _xy(pts):
    c = SIZE * SS / 2
    return [(c + x * SS, c + y * SS) for x, y in pts]


def bez(p0, p1, p2, p3, n=24):
    out = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


def path(*segs):
    """Join cubic segments (each 4 points; consecutive segments share endpoints)."""
    out = []
    for sg in segs:
        out += bez(*sg)[1 if out else 0:]
    return out


def ellipse_pts(cx, cy, rx, ry, rot=0.0, n=48):
    a = math.radians(rot)
    return [(cx + rx * math.cos(t) * math.cos(a) - ry * math.sin(t) * math.sin(a),
             cy + rx * math.cos(t) * math.sin(a) + ry * math.sin(t) * math.cos(a))
            for t in (2 * math.pi * i / n for i in range(n))]


def fill(d, pts, color, outline=None, width=0.0):
    q = _xy(pts)
    d.polygon(q, fill=color)
    if outline:
        d.line(q + [q[0]], fill=outline, width=round(width * SS), joint="curve")


def stroke(d, pts, color, width):
    d.line(_xy(pts), fill=color, width=round(width * SS), joint="curve")


def ell(d, cx, cy, rx, ry, color, outline=None, width=0.0, rot=0.0):
    fill(d, ellipse_pts(cx, cy, rx, ry, rot), color, outline, width)


def hull(pts):
    pts = sorted(set(pts))
    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo, up = [], []
    for p in pts:
        while len(lo) >= 2 and cross(lo[-2], lo[-1], p) <= 0:
            lo.pop()
        lo.append(p)
    for p in reversed(pts):
        while len(up) >= 2 and cross(up[-2], up[-1], p) <= 0:
            up.pop()
        up.append(p)
    return lo[:-1] + up[:-1]


BLACK = (0x1A, 0x1A, 0x24, 255)
WHITE = (255, 255, 255, 255)


def heeler(d, fur, dark, ear_in, tan, plate, ear_h):
    """Front-facing heeler pup in the show's flat style: tall ears, dark crown,
    tan eyebrow spots and muzzle, big black nose."""
    ell(d, 0, 0, R_PLATE, R_PLATE, plate)
    for s in (-1, 1):   # ears: dark outer, lighter inner
        ear = path(((s * 4, -13), (s * 7, -18), (s * 11, -16 - ear_h), (s * 14, -18 - ear_h)),
                   ((s * 14, -18 - ear_h), (s * 18, -12 - ear_h * 0.6), (s * 20, -6), (s * 18, -3)))
        fill(d, ear + [(s * 4, -8)], dark)
        inner = path(((s * 8, -12), (s * 10, -16), (s * 12, -13 - ear_h * 0.8), (s * 13.5, -15 - ear_h * 0.8)),
                     ((s * 13.5, -15 - ear_h * 0.8), (s * 16, -10 - ear_h * 0.4), (s * 17, -7), (s * 15, -5)))
        fill(d, inner, ear_in)
    head = path(((0, -15), (12, -15.5), (21, -9), (21.5, 1)),
                ((21.5, 1), (22, 10), (14, 17), (0, 17)),
                ((0, 17), (-14, 17), (-22, 10), (-21.5, 1)),
                ((-21.5, 1), (-21, -9), (-12, -15.5), (0, -15)))
    fill(d, head, fur)
    crown = path(((-13, -12.5), (-8, -16), (8, -16), (13, -12.5)),
                 ((13, -12.5), (9, -9), (4, -10), (0, -6)),
                 ((0, -6), (-4, -10), (-9, -9), (-13, -12.5)))
    fill(d, crown, dark)
    for s in (-1, 1):
        ell(d, s * 8.5, -7.5, 2.6, 1.7, tan)                     # eyebrow spots
    muzzle = path(((-12, 6), (-12, -1), (-5, 0), (0, 1)),
                  ((0, 1), (5, 0), (12, -1), (12, 6)),
                  ((12, 6), (12, 13), (5, 16.5), (0, 16.5)),
                  ((0, 16.5), (-5, 16.5), (-12, 13), (-12, 6)))
    fill(d, muzzle, tan)
    for s in (-1, 1):
        ell(d, s * 8, -2, 2.3, 2.9, BLACK)
        ell(d, s * 8 + 0.8, -3.1, 0.8, 0.8, WHITE)
    nose = path(((0, 2.5), (4.5, 2.2), (5.6, 4.2), (3.6, 6.4)),
                ((3.6, 6.4), (2, 8), (-2, 8), (-3.6, 6.4)),
                ((-3.6, 6.4), (-5.6, 4.2), (-4.5, 2.2), (0, 2.5)))
    fill(d, nose, BLACK)
    ell(d, 1.4, 3.6, 1.1, 0.6, (0x55, 0x55, 0x66, 255))
    stroke(d, [(0, 8), (0, 10)], BLACK, 0.8)
    stroke(d, bez((-6, 10), (-3, 13.5), (3, 13.5), (6, 10)), BLACK, 0.9)
    stroke(d, head + [head[0]], (0x24, 0x2A, 0x48, 255), 0.6)


def piggy(d):
    """Pink pig in profile, the classic 'hair-dryer' head: a round head with a long
    snout pointing right, both eyes on top of the snout, rosy cheek, two ears."""
    pink, line = (0xF7, 0xA8, 0xC2, 255), (0xD9, 0x6A, 0x92, 255)
    ell(d, 0, 0, R_PLATE, R_PLATE, (0xFF, 0xF3, 0xD2, 255))
    for ex, tip in ((-15.5, (-18, -20)), (-9, (-10.5, -23))):   # ears
        ear = path(((ex - 3.5, -9), (ex - 3, -14), tip, tip), ((tip), tip, (ex + 2.5, -15), (ex + 3.5, -10)))
        fill(d, ear, pink, line, 0.8)
    head_c, head_r = (-10, 3), 12.5
    tip_c = (15.5, -7.5)
    body = hull(ellipse_pts(*head_c, head_r, head_r * 0.98, n=72) +
                ellipse_pts(*tip_c, 4.2, 7.6, rot=-18, n=48) +
                [(3, -13.5), (3, 6.5)])
    fill(d, body, pink, line, 0.9)
    ell(d, *tip_c, 4.2, 7.6, (0xF2, 0x94, 0xB3, 255), line, 0.8, rot=-18)
    ell(d, tip_c[0] - 1.3, tip_c[1] - 2.6, 0.9, 1.4, line, rot=-18)
    ell(d, tip_c[0] + 1.0, tip_c[1] + 2.4, 0.9, 1.4, line, rot=-18)
    for ex, ey in ((-0.5, -11.2), (5.5, -12.9)):                 # eyes on top of the snout
        ell(d, ex, ey, 2.9, 2.9, WHITE, line, 0.6)
        ell(d, ex + 0.7, ey - 0.2, 1.35, 1.35, BLACK)
    ell(d, -13, 6.5, 4.2, 4.2, (0xEE, 0x6A, 0x92, 255))           # cheek
    stroke(d, bez((-4.5, 3.5), (-1, 9), (6, 7.5), (9, 1.5)), line, 1.0)   # smile


def face(kind: str) -> Image.Image:
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    plate = (0xFF, 0xF8, 0xEC, 255)
    if kind == "bluey":
        heeler(d, (0x86, 0xB7, 0xEC, 255), (0x2E, 0x44, 0x8C, 255), (0x9C, 0xC1, 0xF2, 255),
               (0xF0, 0xDF, 0xA8, 255), plate, ear_h=12)
    elif kind == "bingo":
        heeler(d, (0xEE, 0x8A, 0x4E, 255), (0xB4, 0x45, 0x26, 255), (0xF5, 0xB2, 0x80, 255),
               (0xF8, 0xE4, 0xBE, 255), plate, ear_h=8)
    else:
        piggy(d)
    return img


def frame(base: Image.Image, angle: float, size: int = SIZE) -> Image.Image:
    n = SIZE * SS
    rot = base.rotate(-angle, resample=Image.BICUBIC, center=(n / 2, n / 2))
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).ellipse([n / 2 - R_PLATE * SS] * 2 + [n / 2 + R_PLATE * SS] * 2, fill=255)
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.paste(rot, (0, 0), mask)
    d = ImageDraw.Draw(out)
    c = n / 2
    d.ellipse([c - R_PLATE * SS, c - R_PLATE * SS, c + R_PLATE * SS, c + R_PLATE * SS],
              outline=INK, width=round(1.5 * SS))
    d.ellipse([c - R_FLANGE * SS, c - R_FLANGE * SS, c + R_FLANGE * SS, c + R_FLANGE * SS],
              outline=INK, width=round(1.5 * SS))
    return out.resize((size, size), Image.LANCZOS)


def to_rgb565a8(img: Image.Image) -> bytes:
    color, alpha = bytearray(), bytearray()
    raw = img.tobytes()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        color += bytes((v & 0xFF, v >> 8))
        alpha.append(a)
    return bytes(color + alpha)


def emit(lines: list[str], name: str, frames: list[Image.Image], array: bool) -> int:
    total = 0
    for f, img in enumerate(frames):
        data = to_rgb565a8(img)
        total += len(data)
        rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
        lines.append(f"static const uint8_t {name}_{f}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "\n};")
    w = frames[0].size[0]
    dsc = [f"{{.header.magic = LV_IMAGE_HEADER_MAGIC, .header.cf = LV_COLOR_FORMAT_RGB565A8,"
           f" .header.w = {w}, .header.h = {w}, .header.stride = {w * 2},"
           f" .data_size = sizeof({name}_{f}), .data = {name}_{f}}}" for f in range(len(frames))]
    if array:
        lines.append(f"const lv_image_dsc_t {name}[{len(frames)}] = {{")
        lines += [f"    {d}," for d in dsc]
        lines += ["};", ""]
    else:
        lines += [f"const lv_image_dsc_t {name} = {dsc[0]};", ""]
    return total


def main() -> None:
    kinds = ("bluey", "bingo", "peppa")
    lines = ["// Generated by tools/generate_character_discs.py. Do not edit.", '#include "lvgl.h"', ""]
    total, sheet_rows = 0, []
    for kind in kinds:
        base = face(kind)
        frames = [frame(base, f * 360.0 / FRAMES) for f in range(FRAMES)]
        sheet_rows.append(frames)
        total += emit(lines, f"bp_disc_{kind}", frames, True)
        total += emit(lines, f"bp_disc_{kind}_thumb", [frame(base, 0, THUMB)], False)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines))
    print(f"bp_char_discs: {len(kinds)} x {FRAMES} frames {SIZE}x{SIZE} RGB565A8, {total} bytes"
          f" -> {OUT.relative_to(ROOT)}")
    if "--preview" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--preview") + 1])
        sheet = Image.new("RGB", (SIZE * 6, SIZE * len(kinds)), (0x20, 0xE4, 0x7C))
        for r, frames in enumerate(sheet_rows):
            for i, f in enumerate(range(0, FRAMES, 6)):
                sheet.paste(frames[f], (i * SIZE, r * SIZE), frames[f])
        sheet.resize((sheet.width * 3, sheet.height * 3), Image.LANCZOS).save(out)


if __name__ == "__main__":
    main()
