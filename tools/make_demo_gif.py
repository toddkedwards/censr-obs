"""Capture a short demo GIF of Censr covering a moving secret, for the OBS
listing / README. Plays the motion clip through the filter and grabs frames as
fast as OBS WebSocket allows, then assembles docs/demo.gif.

OBS must be up with the WebSocket server.
"""
import os
import time

import imageio.v2 as imageio
import numpy as np
from PIL import Image
import obsws_python as obs

HERE = os.path.dirname(os.path.abspath(__file__))
VIDEO = os.path.join(HERE, "fixtures", "motion_secret.mp4")
TMP = os.path.join(HERE, "out", "gifframes")
GIF = os.path.join(os.path.dirname(HERE), "docs", "demo.gif")
SCENE, INPUT, FILTER, KIND = "CensrMotion", "CensrClip", "Censr Redaction", "censr_redaction"

os.makedirs(TMP, exist_ok=True)
os.makedirs(os.path.dirname(GIF), exist_ok=True)

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
if FILTER not in [f["filterName"] for f in cl.get_source_filter_list(INPUT).filters]:
    cl.create_source_filter(INPUT, FILTER, KIND, {})
cl.set_current_program_scene(SCENE)

print("warming up (GPU + zero-leak buffer)...")
time.sleep(7)

N = 40
print(f"capturing {N} frames ...")
for i in range(N):
    p = os.path.join(TMP, f"g{i:03d}.png")
    cl.save_source_screenshot(SCENE, "png", p, 960, 540, -1)

frames = []
for i in range(N):
    p = os.path.join(TMP, f"g{i:03d}.png")
    frames.append(np.asarray(Image.open(p).convert("RGB")))

# ~10 fps GIF, infinite loop, palette-optimized by imageio.
imageio.mimsave(GIF, frames, duration=0.1, loop=0)
print(f"wrote {GIF}  ({os.path.getsize(GIF)/1e6:.1f} MB, {len(frames)} frames)")
