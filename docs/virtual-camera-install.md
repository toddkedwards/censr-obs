# Censr Virtual Camera — install & test (Windows 11)

The **Censr Virtual Camera** makes the redacted output available to *any* app that
picks a webcam (Zoom, Meet, Teams, Discord, browsers) — no OBS. It is a Media
Foundation software camera (`MFCreateVirtualCamera`) whose media source serves the
frames `censr-host` publishes to shared memory.

> **Status:** the device builds and links against the real MF virtual-camera API,
> exports a valid COM server, and reads the live frame stream. It has **not yet
> been validated inside a real camera app** — that validation (and any media-type/
> attribute tuning it turns up) is the next step, and it requires the elevated,
> signed install below plus a Win11 capture app. Treat these steps as the test
> procedure, not a shipping installer.

## Components

- `censr-mfcam.dll` — the in-proc COM media source (the camera). Reads the shared
  frame stream; serves 1920×1080 @ 30 RGB32.
- `censr-vcam.exe` — control tool: registers the COM server and creates/removes the
  camera.
- `censr-host.exe` — the capture + redaction app that must be **running** to feed
  the camera (otherwise the camera shows a dark placeholder).

Build them with `-DCENSR_BUILD_HOST=ON`.

## Sign first (required in practice)

Windows will not let the Frame Server load an unsigned camera media source on most
systems, and SmartScreen blocks an unsigned installer. Sign `censr-mfcam.dll` (and
`censr-vcam.exe`) with the same pipeline the plugin uses — Azure Trusted Signing or
a cert — before registering. A self-signed cert only proves the mechanics; a real
(trusted) cert is needed for the camera to load on other machines.

## Install (elevated)

Registering a COM server and creating a **system** virtual camera is a machine
change — run from an **Administrator** prompt:

```
censr-vcam.exe install "C:\path\to\censr-mfcam.dll"
```

This registers `censr-mfcam.dll` under `HKCR\CLSID\{…}` and calls
`MFCreateVirtualCamera` (system lifetime) so "Censr Virtual Camera" appears to apps.

## Use

1. Start `censr-host.exe` and pick a monitor/window to redact.
2. In your app's camera settings, choose **Censr Virtual Camera**.

## Remove

```
censr-vcam.exe uninstall "C:\path\to\censr-mfcam.dll"
```

## Legacy apps — the DirectShow filter

Modern apps (Zoom, Teams, Discord, browsers) see the MF camera above via the
Frame Server. Older apps that only enumerate **DirectShow** video-input devices
need the separate `censr-dshowcam.dll`, which registers itself under
`CLSID_VideoInputDeviceCategory`. Sign it, then register elevated:

```
regsvr32 censr-dshowcam.dll        REM install (elevated)
regsvr32 /u censr-dshowcam.dll     REM remove
```

Both cameras share the same friendly name and read the same host frame stream, so
run whichever the target app enumerates. (Same status caveat as the MF source —
compiles and reads the stream, not yet validated in a real DirectShow app; the
connection/allocator negotiation is the usual spot that needs tuning.)

## Known follow-ups before this is shippable

- Validate both sources in real apps; tune MF media-type attributes and the
  DirectShow connection/allocator negotiation as needed (this is where from-scratch
  filters usually need iteration).
- Package the DLLs + tool + host into the signed installer and register on install.
- Handle output sizes other than the fixed 1080p (currently the sources scale the
  incoming frame to 1920×1080).
