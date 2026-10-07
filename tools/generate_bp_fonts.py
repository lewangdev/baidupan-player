#!/usr/bin/env python3
"""Generate the player's Chinese LVGL fonts with a pinned lv_font_conv.

bp_font_16: ASCII + every GB2312 character, so arbitrary Netdisk file names render.
bp_font_24: ASCII + the CJK characters used by fixed UI strings (page titles).
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets/fonts"
FONT = ASSETS / "SourceHanSansSC-Regular.otf"
SOURCES = ("bp_ui.c", "bp_ui_reel.c", "main.c", "bp_baidu.c", "bp_player.c")
NON_ASCII = re.compile(r"[^\x00-\x7f]")


def ui_symbols() -> set[int]:
    text = ""
    for name in SOURCES:
        source = (ROOT / "main" / name).read_text(encoding="utf-8")
        # String literals only: comments must not inflate the subset.
        text += "".join(re.findall(r'"(?:\\.|[^"\\])*"', source))
    return {ord(c) for c in NON_ASCII.findall(text)}


def gb2312() -> set[int]:
    chars = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(ord(bytes((hi, lo)).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return chars


def convert(name: str, size: int, symbols: set[int]) -> None:
    target = ASSETS / f"{name}.c"
    ranges = ",".join(hex(c) for c in sorted(symbols))
    subprocess.run(
        ["npx", "--yes", "lv_font_conv@1.5.3", "--font", str(FONT),
         "--range", "0x20-0x7e", "--range", ranges, "--size", str(size), "--bpp", "4",
         "--format", "lvgl", "--no-compress", "--lv-include", "lvgl.h",
         "--lv-font-name", name, "--output", str(target)],
        check=True)
    covered = {int(x, 16) for x in re.findall(r"/\* U\+([0-9A-Fa-f]+)", target.read_text())}
    missing = (symbols | set(range(0x20, 0x7F))) - covered
    # Source Han Sans SC lacks a handful of GB2312 punctuation forms; report, don't fail.
    ui_missing = missing & ui_symbols()
    assert not ui_missing, f"{name}: UI glyphs missing: {sorted(hex(c) for c in ui_missing)}"
    print(f"{name}: {len(covered)} glyphs, {len(missing)} unavailable, "
          f"{target.stat().st_size} source bytes")


if __name__ == "__main__":
    ui = ui_symbols()
    convert("bp_font_16", 16, gb2312() | ui)
    convert("bp_font_24", 24, ui)
