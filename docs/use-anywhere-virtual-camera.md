# Use Censr in any app — via OBS Virtual Camera

Censr is an OBS **filter**, but you don't have to *stream from OBS* to benefit from
it. OBS ships a built-in **Virtual Camera** — a fake webcam that other apps can
select. Point your meeting or chat app at it and everything Censr covers is already
redacted **before it leaves your machine**.

That means Censr protects you in **Zoom, Google Meet, Microsoft Teams, Discord,
Slack huddles, Webex, OBS itself — anything that lets you pick a webcam.**

```
your screen / window / camera
        │
        ▼
   OBS source  ──  Censr Redaction filter  (covers secrets, opaque)
        │
        ▼
  OBS Virtual Camera   ──►  Zoom / Meet / Teams / Discord / …
```

## One-time setup

1. **Install Censr** (see the [README](../README.md#install)) and open OBS.
2. Add the thing you want to share as a source:
   - **Screen share** → *Sources* → **+** → **Display Capture** (a whole monitor)
     or **Window Capture** (one app window).
   - **Webcam** → **Video Capture Device**.
3. Right-click that source → **Filters** → **+** → **Censr Redaction**. Tune it in
   the filter properties (keep **Zero-leak delay** on for the strongest protection —
   a secret is never shown uncovered, not even for one frame).
4. Close the Filters window.

## Every time you go live

1. In OBS, click **Start Virtual Camera** (bottom-right, under *Controls*).
2. In your meeting/chat app, open its **camera / video settings** and choose
   **"OBS Virtual Camera"** as the webcam.
3. That's it — the other side now sees the Censr-redacted feed.

When you're done, click **Stop Virtual Camera** in OBS.

> **Tip — sharing your screen, not your face.** Most meeting apps have a *separate*
> "Share screen" button that bypasses the webcam entirely — that path does **not**
> go through Censr. To stay protected, share your screen **as the OBS Virtual
> Camera** (select it as your camera and turn your video on) instead of using the
> app's native screen-share. In apps that support a second video source you can also
> add the Virtual Camera as a "camera" and present that.

## Why this works (and its limits)

- **Upstream of the app.** Censr redacts inside OBS's pipeline; the Virtual Camera
  only ever receives frames that have already passed through the filter. The
  receiving app can't see the original pixels.
- **100% local.** The virtual camera is a local device — no cloud, no extra
  network hop. Your video still never leaves your machine except through the call
  you were already making.
- **Zero-leak still applies.** The ~1.5 s delay buffer works exactly as it does for
  streaming: a flagged secret is held until its cover is ready. Expect the same
  slight latency on the virtual-camera feed.
- **Frame rate / resolution** follow OBS's canvas and Virtual Camera settings
  (*Settings → Video*). 1080p30 is the tested target.

## Requirements

- OBS Studio 31+ with the **Censr Redaction** filter installed (Windows 10/11 x64).
- The OBS Virtual Camera (installed with OBS by default on Windows).

## Coming later — a standalone Censr virtual camera

This route needs OBS running in the background. A standalone Censr app — capture +
the same opaque-redaction engine + its own system virtual-camera device, no OBS
required — is on the roadmap. The detection core already sits behind a clean
interface, so it ports directly; the new work is frame capture and a signed
virtual-camera device. Until then, the OBS Virtual Camera above is the supported way
to use Censr in any app.
