# Roadmap — standalone Censr virtual camera (Path B)

**Goal.** A Censr app that redacts a screen / window / webcam and exposes its output
as a **system virtual camera**, so *any* app (Zoom, Meet, Teams, Discord, browsers)
sees the redacted feed **without OBS running**. Path A (OBS Virtual Camera, see
[use-anywhere-virtual-camera.md](use-anywhere-virtual-camera.md)) is the interim
answer; this is the real product.

## What we already have (reuse as-is)

The detection engine has **no libobs dependency** — it's plain C++ behind interfaces:

| Component | File | Reuse |
|---|---|---|
| Async detection worker (latest-frame-wins, boxes tagged with frame ts) | `censr_detector.*` | **verbatim** |
| OCR interface + 3 backends (ONNX·DirectML, Windows.Media.Ocr, Vision) | `ocr_engine.h`, `onnx_ocr.cpp`, `ocr_winrt.cpp` | **verbatim** |
| Secret rules (keys, labels, emails, entropy, per-word spans) | `secret_rules.*` | **verbatim** |
| QR / barcode detection | `qr_zxing.*` | **verbatim** |
| Rounded opaque cover HLSL (SDF) | inline in `censr-filter.cpp` | port the shader |

What's OBS-specific and must be **rebuilt** (all inside `censr-filter.cpp` today):
GPU frame capture, the zero-leak ring/delay buffer, the cover compositor (libobs
`gs_*`), and the properties UI.

**Phase 0 is done** — the engine now builds as a standalone `censr-core` static
library (`add_library(censr-core STATIC …)`) that the plugin links; it carries its
own `censr_log` shim (`src/censr_log.{h,cpp}`) instead of calling libobs, so it has
no `obs`/`util/*` includes. The plugin installs a handler forwarding core logs to
`obs_log`. The standalone host will link the same `censr-core`.

## Target architecture — 3 pieces

```
 ┌── censr-host (our process: GPU + models) ──────────────┐
 │  capture ─► Detector (5 Hz) ─► cover boxes             │
 │     │                              │                   │
 │     └──► D3D11 delay buffer ◄──────┘                   │
 │              │  (composite opaque covers, zero-leak)   │
 │              ▼                                          │
 │        shared-memory frame ring (BGRA + ts)            │
 └──────────────│─────────────────────────────────────────┘
                ▼   read-only, tiny, no GPU/model deps
        censr-vcam device  ──►  Zoom / Meet / Teams / Discord / …
```

1. **`censr-core`** — static lib: `Detector` + `ocr_engine` + `secret_rules` +
   `qr_zxing`, extracted from the OBS target so both plugins and the host link it.
2. **`censr-host`** — standalone app/service. Captures frames, runs `censr-core`,
   composites covers with the zero-leak delay buffer in D3D11, and publishes finished
   frames into a **shared-memory ring**. Owns the GPU, the models, and the UI.
3. **`censr-vcam`** — the registered virtual-camera device apps enumerate. It only
   **reads** the shared-memory ring — no GPU, no models, no detection. This split
   mirrors OBS's own virtual-cam: the camera source is loaded *into the consuming
   app's process* by the OS frame server, so it must be minimal and rock-solid.

## Capture — `Windows.Graphics.Capture` (WGC)

- **Per-window and per-monitor** capture, the actual use case (share one app window,
  not a whole screen). Win10 1903+.
- Desktop Duplication API is the fallback for full-monitor only.
- Webcam passthrough via Media Foundation for the "redact my camera" case.
- Capture yields a D3D11 texture; downscale + CPU-readback a copy for the Detector
  exactly as the OBS filter does today (5 Hz, off-thread).

## Cover compositor + zero-leak (rewrite of the OBS bits in D3D11)

- Same two-rate design: every frame draws opaque rounded rects (existing SDF HLSL)
  over the delayed frame; detection runs at ~5 Hz and tags boxes with a frame ts.
- Own the ring/delay buffer in D3D11 (libobs did it before). The zero-leak invariant
  is unchanged: hold output ~1.5 s so every displayed frame's covers are ready.
- Linear-space amber fill carries over (already worked out).

## The virtual-camera device — the hard, risky part

Two device backends; ship both for broad reach:

- **DirectShow source filter** (a registered COM DLL). OBS-proven, seen by the widest
  set of apps including many that predate the modern frame server. Deprecated by MS
  but still the compatibility workhorse. Needs COM registration (installer, admin) +
  **code signing**.
- **Media Foundation virtual camera** (`MFCreateVirtualCamera`, Win11 22000+). The
  modern, sanctioned API; required by some UWP/Store apps that ignore DirectShow.
  Needs the source packaged/registered (MSIX) and signed.

**Signing is mandatory** here (Windows increasingly blocks unsigned camera
components, and SmartScreen blocks the installer) — this reuses the **Azure Trusted
Signing** pipeline already wired for the plugin.

## License note (business-relevant)

The OBS plugin is **GPL-2.0** *only because it links libobs*. The standalone host
links none of that — its dependencies are permissive: ZXing-cpp (Apache-2.0), ONNX
Runtime (MIT), PP-OCR models (Apache-2.0), Windows SDK. So **the standalone virtual
camera need not be GPL** and can ship as a conventional closed commercial product —
a cleaner story for the paid Lemon Squeezy SKU than the plugin.

## Phasing

| Phase | Deliverable | Risk |
|---|---|---|
| **0 ✅** | Extract `censr-core` static lib; OBS plugin links it (no behavior change) — **done** | low |
| **1 ✅** | `censr-host` MVP: WGC capture → detect → D3D11 cover composite → **preview window** (no vcam). Proves the engine runs outside OBS — **done** | low–med |
| **2 ◐** | Shared-mem ring **✅** + **DirectShow** vcam reading it (next); signed installer that registers it. *This is the "works with everyone" milestone.* | **high** (device reg, signing, cross-app testing) |
| **3** | **MF** virtual camera variant + MSIX for modern Win11 apps | high |
| **4** | Tray UI / config parity with the filter (rules, delay, color, per-word, engine) + source picker (which monitor/window/cam) | med |
| **5** | Packaging, signing, SmartScreen-clean, docs | med |

Phases 0–1 are mostly mechanical given the decoupled core. **Phase 2 is the real
project** — a signed, registered system camera device tested across the target apps —
and is where the bulk of the schedule and risk sits.

### Phase 1 as built

`src/host/` — an opt-in target (`-DCENSR_BUILD_HOST=ON`, then run `censr-host.exe`):

- `wgc_capture.{h,cpp}` — Windows.Graphics.Capture of a monitor, poll-based
  (`CreateFreeThreaded` + `TryGetNextFrame`) so all D3D11 immediate-context work
  stays on the render thread.
- `censr_host.cpp` — a D3D11 device + swapchain preview window; a delay ring of
  GPU textures; the shared `censr::Detector` (Windows.Media.Ocr for the MVP, so no
  ORT DLLs are required to run); and the same **two-rate + fail-closed** cover
  logic as the OBS filter (union covers within one detect cadence, black until
  detection reaches a frame's era). Covers are opaque amber solid rects drawn in
  D3D11.

Source selection: `--monitor N` / `--window N`, or an interactive console list of
monitors and windows (WGC `CreateForMonitor` / `CreateForWindow`). Detection runs on
a GPU-downscaled copy (≤ 1600 px wide) rather than the full (e.g. 4K) frame; the
detector scales boxes back to capture space.

Verified: launches, enumerates and captures a chosen monitor or window, initializes
the detector, and runs the capture→detect→composite loop without OBS. Known MVP
limitations (Phase 2+ work): solid rects, not the rounded SDF covers; and it previews
rather than exposing a virtual camera (the Phase 2 device).

### Phase 2 progress — the frame transport (done)

The backbone both device backends will consume is built and proven:

- `src/host/shared_frame.{h,cpp}` — a single-producer, triple-buffered shared-memory
  frame ring (`Local\CensrVCamFrame`), tear-free via an interlocked sequence number.
  Deliberately free of `censr-core` so the tiny device DLL can link it alone.
- `censr-host` now composites the finished (redacted) frame into an offscreen output
  texture (≤ 1920 px wide), reads it back, and **publishes it to the ring every tick**;
  the preview window draws that same output, so what you see is exactly what the
  camera will emit.
- `src/host/vcam_probe.cpp` (`censr-vcam-probe`) — a dependency-free reader that
  attaches to a running host and confirms live frames are flowing; stands in for the
  device until it exists.
- Tests: `tools/test_shared_frame.cpp` (round-trip: write patterns, read back tear-
  free, sequence advances; a ctest). End-to-end verified: host publishes 1920×1080
  BGRA, a separate probe process reads ~20 fps of live redacted frames.

### Phase 2 progress — the MF virtual camera device (skeleton built)

`src/vcam/` — an MF software camera backed by the shared frame stream:

- `media_source.{h,cpp}` — `CensrSource` (`IMFMediaSourceEx`) + `CensrStream`
  (`IMFMediaStream`) serving 1920×1080@30 **RGB32** (== BGRA, so host frames copy in
  directly, scaled as needed). Reads the ring via `SharedFrameReader`; shows a dark
  placeholder until the host is up.
- `dllmain.cpp` + `censr-mfcam.def` + `guid.cpp` — the in-proc COM server
  (class factory, `DllGetClassObject`, `DllRegister/UnregisterServer`).
- `control.cpp` (`censr-vcam.exe`) — registers the server and calls
  `MFCreateVirtualCamera` to create/remove "Censr Virtual Camera".

Verified: **compiles and links against the real MF virtual-camera API** (SDK
10.0.26100), the DLL exports a valid COM server, and the control tool runs. **Not
yet runtime-validated** inside a real camera app — that needs the elevated, signed
install in `docs/virtual-camera-install.md` plus a Win11 capture app, and is where a
from-scratch MF media source typically needs attribute/media-type iteration.

**Still to do for Phase 2:** validate + tune the MF source in real apps; add the
**DirectShow source filter** for apps that bypass the Frame Server; and fold the DLL
+ tool + host into the **signed installer** that registers on install. That is the
part that needs a real code-signing cert and elevated/MSIX install — see the
device-backend and signing notes above and `docs/virtual-camera-install.md`.

## Open questions to settle before Phase 2

- **Win10 support?** MF virtual camera is Win11-only; committing to Win10 means the
  DirectShow path is required, not optional.
- **Per-window capture correctness** when the shared window is occluded/minimized
  (WGC returns the last frame or nothing — define behavior so nothing leaks).
- **Frame server robustness:** the vcam DLL runs inside untrusted host apps; crash/
  hang there = the user's whole call breaks. Needs a hard isolation + watchdog story.
- **Startup ordering:** the camera must show *something safe* (a "Censr not running"
  card) if an app opens it before `censr-host` is up — never a raw uncovered frame.
