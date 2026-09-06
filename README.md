# Censr — real-time on-screen redaction for OBS (Windows)

Censr is an [OBS Studio](https://obsproject.com/) video-filter plugin that covers
sensitive on-screen content — API keys, passwords, emails, and QR codes — with an
**opaque** fill in real time, so streamers don't leak secrets on camera.

It's the Windows beachhead for an existing macOS app: this plugin reuses the
redaction logic and lets OBS handle capture, encode, streaming, and virtual-cam.

> **Opaque fills only — never blur or pixelate.** Blur/pixelation can be reversed;
> the whole security model is opaque-cover-first.

## Status — Milestone 1 gate: **met**

On a 1080p OBS source, add the **Censr Redaction** filter to any source and:

- API keys (`sk-…`, `ghp_…`, `AKIA…`, high-entropy tokens), `password/secret/token=…`
  lines, emails, and QR codes are covered with an opaque amber fill;
- innocuous text is left visible (no false positives on prose);
- first covers land **~160 ms** after content appears (< 500 ms target);
- OBS holds a steady **30 fps** with 0 dropped frames;
- OCR runs on the **GPU** (PP-OCRv4 via ONNX Runtime + DirectML), with a
  `Windows.Media.Ocr` fallback;
- a **1.5 s zero-leak delay buffer** guarantees a secret is never shown uncovered.

![Censr covering secrets on a 1080p source](docs/demo.png)

## Install

Close OBS, then either:

- **Installer** — run `censr-<version>-windows-x64-installer.exe`, or
- **Zip** — extract `censr-<version>-windows-x64.zip` into
  `%ProgramData%\obs-studio\plugins\` (so you get
  `…\plugins\censr\bin\64bit\censr.dll`).

Reopen OBS and add the **Censr Redaction** filter to any source (right-click the
source → Filters → **+** → Censr Redaction).

## Architecture — a two-rate pipeline + zero-leak buffer

- **Every frame (render thread):** capture the source into a GPU ring buffer and
  display the frame from ~1.5 s ago, drawing opaque amber rectangles over it
  (blending disabled so covers truly overwrite the pixels underneath).
- **~5 Hz (async worker thread):** read a captured frame back to CPU and run
  OCR + secret-regex + QR detection, producing cover boxes tagged with that
  frame's timestamp.
- **Zero-leak guarantee:** because output is delayed ~1.5 s — far longer than
  detection latency (~0.2–0.4 s) — every displayed frame's content has already
  been processed, and its covers are drawn from the matching-era detection. A
  secret is never shown uncovered, not even for one frame (during the initial
  buffer fill the output is black, also leak-free). Verified with a transition
  test: switching a live source to secret content, the secrets appear in the
  delayed output already covered.

Components (`src/`):

| File | Role |
|------|------|
| `censr-filter.cpp` | OBS filter: per-frame cover draw, GPU readback, cover set + TTL |
| `censr_detector.*` | Async detection worker (latest-frame-wins) |
| `ocr_engine.h` | `ICensrOcr` interface (engine swap point) |
| `onnx_ocr.cpp` | **GPU OCR** — PP-OCRv4 (DBNet + CRNN) on ONNX Runtime (DirectML default; CUDA/TensorRT optional) |
| `ocr_winrt.cpp` | Fallback OCR via `Windows.Media.Ocr` (C++/WinRT) |
| `secret_rules.*` | Data-driven secret patterns (keys, labels, emails, entropy) |
| `qr_zxing.cpp` | QR / barcode detection via [ZXing-cpp](https://github.com/zxing-cpp/zxing-cpp) |

OCR engine is selected at runtime: the GPU engine (ONNX/DirectML, runs on any
DX12 GPU) is used when the models are present, falling back to Windows OCR. Set
`CENSR_OCR=winrt` or `CENSR_OCR=onnx` to force one.

## Building (Windows)

**Prerequisites:** Visual Studio 2022 with the *Desktop development with C++*
workload (brings MSVC + the Windows SDK, which supplies the C++/WinRT headers),
CMake ≥ 3.28, Python 3 (for the fetch/verify scripts), and OBS Studio (to
load/test). ZXing-cpp is fetched automatically by CMake.

**Fetch the GPU-OCR dependencies first** (PP-OCR models + ONNX Runtime/DirectML —
large binaries kept out of git):

```bash
python tools/fetch_gpu_ocr.py
```

The template's Windows preset pins the SDK to `10.0.22621`. If you have a different
SDK, add a `CMakeUserPresets.json` (git-ignored) that retargets it:

```json
{
  "version": 8,
  "configurePresets": [
    { "name": "windows-x64-local", "inherits": ["windows-x64"],
      "architecture": "x64,version=10.0.26100" }
  ],
  "buildPresets": [
    { "name": "windows-x64-local", "configurePreset": "windows-x64-local",
      "configuration": "RelWithDebInfo" }
  ]
}
```

```bash
cmake --preset windows-x64-local      # downloads OBS deps + ZXing, configures
cmake --build --preset windows-x64-local
```

**GPU OCR backend.** By default OCR runs on ONNX Runtime's **DirectML** provider
(any DX12 GPU, no CUDA install). To instead use **CUDA/TensorRT**, fetch that ORT
build and configure with the option (also requires the CUDA Toolkit + cuDNN, and
TensorRT for the TensorRT provider, installed on the machine):

```bash
python tools/fetch_gpu_ocr.py --gpu          # adds third_party/ort-gpu
cmake --preset windows-x64-local -DCENSR_ORT_GPU=ON
cmake --build --preset windows-x64-local
```

At runtime the CUDA/TensorRT build picks TensorRT → CUDA → CPU (override with
`CENSR_ORT_EP=tensorrt|cuda`); deploy `onnxruntime.dll` + `onnxruntime_providers_*.dll`
next to `censr.dll`.

**Deploy** to the per-user OBS plugin folder (no admin needed):

```
%ProgramData%\obs-studio\plugins\censr\bin\64bit\censr.dll
%ProgramData%\obs-studio\plugins\censr\data\...
```

**Package a release** — assembles the plugin (DLLs + ONNX Runtime + models) into a
distributable zip, and optionally an installer (needs Inno Setup:
`winget install JRSoftware.InnoSetup`):

```bash
python tools/package.py --installer   # -> dist/censr-<ver>-windows-x64.zip (+ .exe)
```

**Code signing** (optional; removes the SmartScreen warning). If a certificate is
configured, `package.py` signs `censr.dll` and the installer with `signtool`;
otherwise they're left unsigned. Point it at a `.pfx`:

```bash
set CENSR_SIGN_PFX=C:\path\to\cert.pfx
set CENSR_SIGN_PASSWORD=...            # or use CENSR_SIGN_THUMBPRINT for a store cert
python tools/package.py --installer
```

Or use **Azure Trusted Signing** (cloud, ~$10/mo, no cert file — recommended for
CI): set `CENSR_TS_ENDPOINT`, `CENSR_TS_ACCOUNT`, `CENSR_TS_PROFILE`, and
`CENSR_TS_DLIB` (path to `Azure.CodeSigning.Dlib.dll` from the
`Microsoft.Trusted.Signing.Client` NuGet), authenticated via the standard Azure
env (`AZURE_CLIENT_ID`/`AZURE_TENANT_ID`/`AZURE_CLIENT_SECRET`, `az login`, or a
managed identity).

In CI, add repo secrets and the release workflow signs automatically:
- **Trusted Signing:** `CENSR_TS_ENDPOINT`, `CENSR_TS_ACCOUNT`, `CENSR_TS_PROFILE`
  + `AZURE_CLIENT_ID`/`AZURE_TENANT_ID`/`AZURE_CLIENT_SECRET` (a service principal
  with the *Trusted Signing Certificate Profile Signer* role).
- **or PFX:** `CENSR_SIGN_PFX_BASE64` (base64 of the `.pfx`) + `CENSR_SIGN_PASSWORD`.

A real (trusted) certificate is required to actually suppress SmartScreen — a
self-signed cert only proves the pipeline.

## Configuration

Add the **Censr Redaction** filter to a source and open its properties (all apply
live):

- **Zero-leak delay (ms)** — 0–2000; how long output is held so covers are ready
  before a frame shows. 0 ≈ live.
- **Cover color** — the opaque fill color (default amber).
- **OCR engine** — Auto (GPU, else Windows) / GPU (ONNX·DirectML) / Windows OCR.
- **Rule toggles** — API keys & tokens, password/secret/token lines, emails, QR
  codes.
- **Per-word redaction** — cover only the secret token (e.g. hide the key value,
  leave the label visible) instead of the whole line.

## Verification harness

`tools/` contains a Python harness that drives OBS via its WebSocket server to
build a test scene, apply the filter, capture screenshots, and report fps — so
detection and the fps gate can be checked automatically. See `tools/obs_verify.py`
and `tools/make_fixture.py` (generates a 1080p fixture with **fake** secrets).

Other checks: `tools/test_zeroleak.py` (a secret appears in the delayed output
already covered — no uncovered frame) and `tools/test_motion.py` (a moving secret
clip; measures leaked-secret pixels vs a filter-off baseline and the cover's
travel — the cover tracks with near-zero leak).

## Roadmap

- Signed installer / packaged release.

## License

GPL-2.0-or-later (OBS plugins link libobs, which is GPL). See [LICENSE](LICENSE).
