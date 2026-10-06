#!/usr/bin/env python3
"""Fail if firmware images contain the Baidu credentials in plaintext.

Reads main/bp_baidu_keys.h (skipped when absent or still placeholders) and
searches each given file for every credential value. Never prints the values.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEYS = ROOT / "main/bp_baidu_keys.h"


def main(paths: list[str]) -> int:
    if not KEYS.is_file():
        print("Firmware secrets: SKIP (no main/bp_baidu_keys.h)")
        return 0
    values = dict(re.findall(r'#define\s+(BP_BAIDU_\w+)\s+"([^"\\]+)"', KEYS.read_text()))
    values = {k: v for k, v in values.items() if not v.startswith("REPLACE_WITH_")}
    if not values:
        print("Firmware secrets: SKIP (placeholder credentials)")
        return 0
    leaks = []
    for path in paths:
        data = pathlib.Path(path).read_bytes()
        leaks += [f"{pathlib.Path(path).name}: {name}" for name, value in values.items()
                  if value.encode() in data]
    if leaks:
        print("Firmware secrets: FAIL, plaintext credential found in " + "; ".join(leaks))
        return 1
    print(f"Firmware secrets: PASS ({len(values)} values absent from {len(paths)} image(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
