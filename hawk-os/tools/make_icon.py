#!/usr/bin/env python3
"""Generate assets/hawk.ico (multi-size) from assets/hawkos-logo.png."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "hawkos-logo.png"
DST = ROOT / "assets" / "hawk.ico"
SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def main() -> None:
    img = Image.open(SRC).convert("RGBA")
    # Make the square the icon canvas; the source is already square.
    side = max(img.size)
    if img.size != (side, side):
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
        img = canvas
    img.save(DST, format="ICO", sizes=SIZES)
    print(f"wrote {DST} ({DST.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
