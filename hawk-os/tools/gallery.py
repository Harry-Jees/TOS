#!/usr/bin/env python3
"""Build an HTML gallery of the newest Hawk OS screenshots.

A native window cannot be shown in a browser panel, so this gives you the next
best thing: a live contact sheet of everything Hawk OS has captured, which is
also a record of what the agent saw and did.

    python tools/gallery.py [count]
"""

from __future__ import annotations

import glob
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "shots")
OUT = os.path.join(SHOTS, "gallery.html")


def human(path: str) -> str:
    size = os.path.getsize(path)
    when = time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(path)))
    for unit in ("B", "KB", "MB"):
        if size < 1024 or unit == "MB":
            return f"{size:.0f} {unit} · {when}"
        size /= 1024.0
    return str(size)


def main() -> int:
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    files = sorted(glob.glob(os.path.join(SHOTS, "*.png")), key=os.path.getmtime, reverse=True)
    files = files[:count]

    if not files:
        body = '<p class="empty">No screenshots yet. Press <b>Capture</b> in the Hawk OS window ' \
               'or run <code>hawk.exe --shot</code>.</p>'
    else:
        cards = []
        for f in files:
            name = os.path.basename(f)
            cards.append(
                f'<figure><a href="{name}" target="_blank"><img src="{name}" alt="{name}"></a>'
                f'<figcaption><b>{name}</b><span>{human(f)}</span></figcaption></figure>'
            )
        body = "\n".join(cards)

    html = f"""<!doctype html>
<meta charset="utf-8">
<title>Hawk OS - screen captures</title>
<style>
  :root {{ color-scheme: dark; }}
  * {{ box-sizing: border-box; }}
  body {{ margin: 0; padding: 22px; background: #14141a; color: #ececf2;
         font: 14px/1.5 "Segoe UI", system-ui, sans-serif; }}
  h1 {{ font-size: 18px; margin: 0 0 4px; letter-spacing: .2px; }}
  p.sub {{ margin: 0 0 20px; color: #96969e; font-size: 12.5px; }}
  .grid {{ display: grid; gap: 16px; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); }}
  figure {{ margin: 0; background: #1e1e24; border: 1px solid #33333c; border-radius: 12px;
            overflow: hidden; }}
  img {{ display: block; width: 100%; height: auto; background: #000; }}
  figcaption {{ padding: 8px 11px 10px; display: flex; flex-direction: column; gap: 2px; }}
  figcaption b {{ font-weight: 600; font-size: 12.5px; }}
  figcaption span {{ color: #8a8a94; font-size: 11.5px; }}
  .empty {{ color: #96969e; }}
  code {{ background: #26262e; padding: 1px 5px; border-radius: 4px; }}
</style>
<h1>Hawk OS · screen captures</h1>
<p class="sub">{len(files)} newest shots from <code>shots/</code>, refreshing on reload. Click any
image to open it full size.</p>
<div class="grid">
{body}
</div>
"""
    os.makedirs(SHOTS, exist_ok=True)
    open(OUT, "w", encoding="utf-8").write(html)
    print(f"wrote {OUT} ({len(files)} image(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
