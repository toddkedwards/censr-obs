"""Motion stress test: play the moving-secret clip through the filter and measure
how well the cover tracks the moving secret.

Metric: "leaked" red pixels (the red secret visible outside the amber cover),
compared to a filter-off baseline, across a series of output frames. Also tracks
the amber cover centroid to confirm the cover moves with the secret.
"""
import os
import time

import numpy as np
from PIL import Image
import obsws_python as obs

HERE = os.path.dirname(os.path.abspath(__file__))
VIDEO = os.path.join(HERE, "fixtures", "motion_secret.mp4")
OUTDIR = os.path.join(HERE, "out")
SCENE, INPUT, FILTER, KIND = "CensrMotion", "CensrClip", "Censr Redaction", "censr_redaction"

os.makedirs(OUTDIR, exist_ok=True)


def analyze(png):
    a = np.asarray(Image.open(png).convert("RGB")).astype(int)
    R, G, B = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    red = (R > 170) & (G < 110) & (B < 110)
    amber = (R > 200) & (G > 140) & (G < 225) & (B < 100)
    rn, an = int(red.sum()), int(amber.sum())
    cen = None
    if an > 50:
        ys, xs = np.where(amber)
        cen = (int(xs.mean()), int(ys.mean()))
    return rn, an, cen


def shot(cl, name):
    p = os.path.join(OUTDIR, name)
    cl.save_source_screenshot(SCENE, "png", p, 1920, 1080, -1)
    return p


cl = obs.ReqClient(host="localhost", port=4455, password="", timeout=5)
cl.set_video_settings(30, 1, 1920, 1080, 1920, 1080)

scenes = [s["sceneName"] for s in cl.get_scene_list().scenes]
if SCENE not in scenes:
    cl.create_scene(SCENE)
inputs = [i["inputName"] for i in cl.get_input_list().inputs]
if INPUT not in inputs:
    cl.create_input(SCENE, INPUT, "ffmpeg_source",
                    {"local_file": VIDEO, "looping": True, "is_local_file": True}, True)
else:
    cl.set_input_settings(INPUT, {"local_file": VIDEO, "looping": True,
                                  "is_local_file": True}, True)
cl.set_current_program_scene(SCENE)
time.sleep(1.0)

# Baseline with filter OFF: how much red (secret) is visible unobstructed.
existing = [f["filterName"] for f in cl.get_source_filter_list(INPUT).filters]
if FILTER in existing:
    cl.remove_source_filter(INPUT, FILTER)
time.sleep(1.0)
base_red, _, _ = analyze(shot(cl, "motion_baseline.png"))
print(f"baseline visible-red pixels (secret, filter off): {base_red}")

# Filter ON.
cl.create_source_filter(INPUT, FILTER, KIND, {})
print("warming up GPU + filling zero-leak buffer ...")
time.sleep(6)

print(f"{'t(s)':>5} {'leak_red':>9} {'leak%':>6} {'amber_px':>9} {'cover_centroid':>16}")
cens = []
for i in range(12):
    p = shot(cl, f"motion_{i:02d}.png")
    rn, an, cen = analyze(p)
    pct = 100.0 * rn / base_red if base_red else 0.0
    cens.append(cen)
    print(f"{i*0.4:5.1f} {rn:9d} {pct:6.1f} {an:9d} {str(cen):>16}")
    time.sleep(0.4)

# Did the cover actually move?
valid = [c for c in cens if c]
if len(valid) >= 2:
    dx = max(c[0] for c in valid) - min(c[0] for c in valid)
    dy = max(c[1] for c in valid) - min(c[1] for c in valid)
    print(f"cover centroid travel: dx={dx}px dy={dy}px (confirms tracking)")
print("done")
