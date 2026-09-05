"""Censr OBS verification harness.

Drives a running OBS (via obs-websocket v5, localhost:4455, no auth) to:
  - force a 1920x1080 / 30fps canvas,
  - build a scene with the 1080p secrets fixture as an image source,
  - add the 'censr_redaction' filter to that source,
  - capture a screenshot of the rendered (filtered) scene,
  - report render stats (fps, render time, skipped frames).

Usage: python tools/obs_verify.py [--no-filter] [--out PATH]
OBS must already be running with the WebSocket server enabled.
"""
import argparse
import os
import sys
import time

import obsws_python as obs

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixtures", "secrets_1080p.png")
OUTDIR = os.path.join(HERE, "out")

SCENE = "CensrTest"
INPUT = "CensrFixture"
FILTER = "Censr Redaction"          # display name of the filter instance
FILTER_KIND = "censr_redaction"     # registered id


def safe(fn, *a, **k):
    """Call an obsws request, swallow 'already exists / not found' noise."""
    try:
        return fn(*a, **k)
    except Exception as e:  # noqa: BLE001
        print(f"  (ignored) {fn.__name__}: {e}")
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-filter", action="store_true",
                    help="do not add the Censr filter (baseline capture)")
    ap.add_argument("--out", default=os.path.join(OUTDIR, "scene.png"))
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds to let OBS render before screenshot")
    args = ap.parse_args()
    os.makedirs(OUTDIR, exist_ok=True)

    cl = obs.ReqClient(host="localhost", port=4455, password="", timeout=5)
    v = cl.get_version()
    print(f"Connected: OBS {v.obs_version}, ws {v.obs_web_socket_version}")

    # 1080p / 30fps canvas so this is a true 1080p @ 30 test
    safe(cl.set_video_settings, 30, 1, 1920, 1080, 1920, 1080)

    # Scene
    scenes = [s["sceneName"] for s in cl.get_scene_list().scenes]
    if SCENE not in scenes:
        cl.create_scene(SCENE)
        print(f"Created scene {SCENE}")

    # Image-source input
    inputs = [i["inputName"] for i in cl.get_input_list().inputs]
    if INPUT not in inputs:
        cl.create_input(SCENE, INPUT, "image_source",
                        {"file": FIXTURE}, True)
        print(f"Created input {INPUT} -> {FIXTURE}")
    else:
        safe(cl.set_input_settings, INPUT, {"file": FIXTURE}, True)

    # Filter on the input
    existing = [f["filterName"] for f in cl.get_source_filter_list(INPUT).filters]
    if args.no_filter:
        if FILTER in existing:
            safe(cl.remove_source_filter, INPUT, FILTER)
            print("Removed filter (baseline mode)")
    else:
        if FILTER not in existing:
            cl.create_source_filter(INPUT, FILTER, FILTER_KIND, {})
            print(f"Added filter {FILTER} ({FILTER_KIND})")
        else:
            print("Filter already present")

    cl.set_current_program_scene(SCENE)
    time.sleep(args.settle)

    # Screenshot of the composited scene
    try:
        cl.save_source_screenshot(SCENE, "png", args.out, 1920, 1080, -1)
        print(f"Saved screenshot -> {args.out}")
    except Exception as e:  # noqa: BLE001
        print(f"screenshot FAILED: {e}", file=sys.stderr)

    # Stats
    s = cl.get_stats()
    print("--- stats ---")
    print(f"  activeFps            = {s.active_fps:.2f}")
    print(f"  avgFrameRenderTime   = {s.average_frame_render_time:.3f} ms")
    print(f"  renderSkippedFrames  = {s.render_skipped_frames} / {s.render_total_frames}")
    print(f"  outputSkippedFrames  = {s.output_skipped_frames} / {s.output_total_frames}")


if __name__ == "__main__":
    main()
