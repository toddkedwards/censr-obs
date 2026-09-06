"""Capture per-word redaction: only the secret token is covered, labels stay
visible. OBS must be up with the WebSocket server."""
import os
import time

import obsws_python as obs

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixtures", "secrets_1080p.png")
OUTDIR = os.path.join(HERE, "out")
SCENE, INPUT, FILTER, KIND = "CensrTest", "CensrFixture", "Censr Redaction", "censr_redaction"

os.makedirs(OUTDIR, exist_ok=True)
cl = obs.ReqClient(host="localhost", port=4455, password="", timeout=5)
cl.set_video_settings(30, 1, 1920, 1080, 1920, 1080)
scenes = [s["sceneName"] for s in cl.get_scene_list().scenes]
if SCENE not in scenes:
    cl.create_scene(SCENE)
inputs = [i["inputName"] for i in cl.get_input_list().inputs]
if INPUT not in inputs:
    cl.create_input(SCENE, INPUT, "image_source", {"file": FIXTURE}, True)
else:
    cl.set_input_settings(INPUT, {"file": FIXTURE}, True)
existing = [f["filterName"] for f in cl.get_source_filter_list(INPUT).filters]
if FILTER not in existing:
    cl.create_source_filter(INPUT, FILTER, KIND, {})
cl.set_current_program_scene(SCENE)
cl.set_source_filter_settings(INPUT, FILTER, {"per_word": True}, overlay=True)
print("per-word on; warming up...")
time.sleep(18)
cl.save_source_screenshot(SCENE, "png", os.path.join(OUTDIR, "perword.png"), 1920, 1080, -1)
print("captured perword.png")
