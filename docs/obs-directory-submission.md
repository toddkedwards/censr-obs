# OBS Resources submission — Censr

Paste-ready copy for listing Censr in the OBS **Resources** directory
(https://obsproject.com/forum/resources/ → "Add resource"). Fill the form fields
from the top block; paste the **Description** into the resource body (the forum
editor accepts formatting — adapt headings to BBCode if you like).

---

## Form fields

| Field | Value |
|---|---|
| **Title** | Censr — Real-Time Secret Redaction |
| **Tag line** | Automatically cover API keys, passwords, emails & QR codes — on stream or in any video call — in real time, with opaque fills. |
| **Category** | OBS Studio Plugins |
| **Version** | 0.1.1 |
| **License** | GPL-2.0-or-later |
| **Compatibility** | OBS Studio 31.1.1 or newer (built against 31.1.1; tested on 32.2); Windows 10/11 (x64). A macOS universal build is available from source. |
| **Download / external URL** | https://github.com/toddkedwards/censr-obs/releases/latest |
| **Source code** | https://github.com/toddkedwards/censr-obs |

---

## Description (paste into the resource body)

**Censr** is a video-filter plugin that watches any OBS source and covers
sensitive on-screen content — **API keys, passwords, emails, and QR codes** —
with an **opaque fill** in real time. Add it to a source, and if a secret shows
up in your capture, Censr blocks it out before your viewers can read it.

Built for streamers who live-code, demo dashboards, or share their screen and
don't want a stray token or password to leak on camera.

### How it works

A two-rate pipeline keeps it real-time:

- **Every frame** it draws opaque rounded rectangles over the current set of
  detected regions — cheap, so your framerate is unaffected.
- **~5×/second**, on a background thread, it reads the frame, runs OCR + a
  secret-pattern rule set + QR/barcode detection, and updates what to cover.
- **Opaque-cover-first:** covers are solid fills, never blur or pixelation
  (which can be reversed). If it's flagged, it's completely hidden.
- **Zero-leak delay buffer:** an optional ~1.5 s output delay means a frame is
  only shown *after* its detection has finished — so a detected secret is never
  displayed uncovered, not even for a single frame. Turn the delay down to 0 for
  lowest latency.

### Features

- Detects API keys / tokens (`sk-…`, `ghp_…`, `AKIA…`, high-entropy strings),
  `password/secret/token = …` lines, email addresses, and QR codes / barcodes.
- **GPU-accelerated OCR** (PP-OCRv4 via ONNX Runtime / DirectML on any DX12 GPU;
  Windows OCR fallback; optional CUDA/TensorRT build).
- Configurable from the filter panel: delay, cover color, which categories to
  redact, OCR engine, and **per-word redaction** (hide only the secret value,
  keep the label visible).
- Holds a steady framerate — covers are drawn on the GPU; detection is off-thread.
- Everything runs **locally**. No network calls, no telemetry, nothing leaves
  your machine.

### Not just for streaming — use it in any app

You don't have to be live-streaming to benefit. Add Censr to a source, click
**Start Virtual Camera** in OBS, and select **"OBS Virtual Camera"** as your webcam
in **Zoom, Google Meet, Microsoft Teams, Discord** — anything that takes a camera.
The feed is redacted before it leaves your machine, so a screen-share in a meeting
gets the same protection as a stream. (Tip: present *as the Virtual Camera* rather
than the app's native screen-share, which bypasses the filter.)

*Coming soon:* a standalone **Censr Virtual Camera** app that does this without OBS
running — same redaction engine, its own system webcam. In development on GitHub.

### Requirements

- Windows 10/11 (x64) and OBS Studio 31.x.
- A Direct3D 11 capable GPU (essentially any modern GPU).

### Install

1. Close OBS.
2. Run the installer, **or** extract the zip into
   `%ProgramData%\obs-studio\plugins\`.
3. Reopen OBS, right-click a source → **Filters** → **+** → **Censr Redaction**.

To use the redacted feed in a meeting app, also see the short guide
[Use Censr in any app](https://github.com/toddkedwards/censr-obs/blob/main/docs/use-anywhere-virtual-camera.md).

### Privacy & limitations (please read)

Censr is a **safety net, not a guarantee.** Detection is automated (OCR + pattern
matching), so it can miss secrets in unusual fonts, tiny text, heavy motion, or
formats the rules don't cover — and it may occasionally over-cover. Keep the
zero-leak delay on for the strongest protection, and don't rely on Censr alone
when handling highly sensitive material. It processes video entirely on your
machine and never transmits anything.

Free and open source (GPL-2.0-or-later). Bug reports and contributions welcome on
GitHub.

---

## Media to attach

Upload as screenshots (the forum shows the first as the thumbnail):

1. `docs/demo.png` — secrets covered on a mock terminal (headline shot).
2. A filter-properties panel screenshot (open the Censr Redaction filter in OBS).
3. Optional: a per-word redaction shot (`tools/out/perword.png` if kept).

A short screen-recording GIF (secret appears → instantly covered) makes a strong
first impression if you can capture one. A clip of the redacted feed showing up as
the webcam in a Zoom/Meet call reinforces the "works in any app" angle.

## Pre-submission checklist

- [ ] A published GitHub release exists (v0.1.1 ✓) with the zip attached.
- [ ] OBS forum account created and verified.
- [ ] Screenshots exported/ready to upload.
- [ ] Decide: host the zip on the forum, or link to the GitHub release (link is
      fine and keeps a single source of truth).
- [ ] Confirm the version/compatibility fields match the release.
- [ ] (Recommended before wide distribution) code-sign the binaries so Windows
      SmartScreen doesn't warn on install — the signing pipeline is already wired
      (see README); it just needs a certificate.
