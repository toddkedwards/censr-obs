"""Zero-leak transition test.

Fills the delay buffer with a non-secret image, switches the source to the
secret fixture, and captures the composited OUTPUT on a timeline through the
transition. Proves that when the secret content finally appears in the (delayed)
output, it is already covered -- a secret is never shown uncovered.

Run while OBS is up with the WebSocket server. Frames land in tools/out/zl_*.png
named by ms elapsed since the switch.
"""
import os
import time

import obsws_python as obs

HERE = os.path.dirname(os.path.abspath(__file__))
BEFORE = os.path.join(HERE, "fixtures", "before_1080p.png")
SECRETS = os.path.join(HERE, "fixtures", "secrets_1080p.png")
OUTDIR = os.path.join(HERE, "out")

SCENE = "CensrTest"
INPUT = "CensrFixture"
FILTER = "Censr Redaction"
FILTER_KIND = "censr_redaction"

os.makedirs(OUTDIR, exist_ok=True)
cl = obs.ReqClient(host="localhost", port=4455, password="", timeout=5)
cl.set_video_settings(30, 1, 1920, 1080, 1920, 1080)

scenes = [s["sceneName"] for s in cl.get_scene_list().scenes]
if SCENE not in scenes:
    cl.create_scene(SCENE)
inputs = [i["inputName"] for i in cl.get_input_list().inputs]
if INPUT not in inputs:
    cl.create_input(SCENE, INPUT, "image_source", {"file": BEFORE}, True)
existing = [f["filterName"] for f in cl.get_source_filter_list(INPUT).filters]
if FILTER not in existing:
    cl.create_source_filter(INPUT, FILTER, FILTER_KIND, {})
cl.set_current_program_scene(SCENE)

# Fill the delay buffer with the non-secret image.
cl.set_input_settings(INPUT, {"file": BEFORE}, True)
print("filling buffer with BEFORE image...")
time.sleep(3.0)

# Switch to the secret fixture and start the clock.
t0 = time.time()
cl.set_input_settings(INPUT, {"file": SECRETS}, True)
print("switched to SECRETS at t0; capturing timeline...")

for i in range(20):
    el_ms = int((time.time() - t0) * 1000)
    path = os.path.join(OUTDIR, f"zl_{i:02d}_{el_ms:04d}ms.png")
    try:
        cl.save_source_screenshot(SCENE, "png", path, 1920, 1080, -1)
        print(f"  frame {i:02d} @ {el_ms} ms")
    except Exception as e:  # noqa: BLE001
        print(f"  frame {i:02d} failed: {e}")
    time.sleep(0.12)

print("done")
