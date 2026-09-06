"""Generate a 1080p test clip for the motion stress test: a RED secret
(API key) glides smoothly around the screen (seamless loop) while a static white
non-secret label stays put. Output: tools/fixtures/motion_secret.mp4

The secret is pure-ish red so the motion test can measure "leaked" secret pixels
(red visible outside the amber cover) vs a filter-off baseline.
"""
import math
import os

import numpy as np
import imageio.v2 as imageio
from PIL import Image, ImageDraw, ImageFont

W, H, FPS, DUR = 1920, 1080, 30, 6
OUT = os.path.join(os.path.dirname(__file__), "fixtures", "motion_secret.mp4")
SECRET = "API_KEY=sk-live-ExampleSecret1234567890abcd"


def font(sz):
    for n in ("consola.ttf", "cour.ttf"):
        p = os.path.join(r"C:\Windows\Fonts", n)
        if os.path.exists(p):
            return ImageFont.truetype(p, sz)
    return ImageFont.load_default()


f = font(36)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
w = imageio.get_writer(OUT, fps=FPS, codec="libx264", quality=9,
                       macro_block_size=8)

cx, cy = 760, 500          # center of motion
ax, ay = 200, 110          # amplitude (px); one full period over DUR -> seamless
n = FPS * DUR
for i in range(n):
    t = i / n
    img = Image.new("RGB", (W, H), (24, 26, 33))
    d = ImageDraw.Draw(img)
    # static non-secret label (white) — must stay visible
    d.text((60, 40), "safe: streaming setup notes (no secrets here)",
           font=f, fill=(205, 210, 215))
    # moving secret (red)
    x = cx + int(ax * math.sin(2 * math.pi * t))
    y = cy + int(ay * math.sin(2 * math.pi * t))
    d.text((x, y), SECRET, font=f, fill=(235, 30, 30))
    w.append_data(np.asarray(img))
w.close()
print("wrote", OUT, f"{n} frames, amp=({ax},{ay})")
