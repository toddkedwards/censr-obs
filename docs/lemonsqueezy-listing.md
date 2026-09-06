# Lemon Squeezy product listing — Censr

Paste-ready copy for a Lemon Squeezy product. Because Censr is GPL-2.0 (it links
OBS's libobs), you're selling **convenience + support + funding development**, not
exclusivity — the framing below leans into that honestly, which builds trust for
a security tool and keeps you license-compliant.

---

## Product fields

| Field | Value |
|---|---|
| **Name** | Censr — Real-Time Secret Redaction for OBS |
| **Tagline / summary** | Stop leaking API keys, passwords & QR codes on stream. One-click install, signed & ready to run. |
| **Pricing model** | Pay-what-you-want (suggested **$10**, minimum **$3**) — see notes below |
| **Product file** | The signed release: `censr-<version>-windows-x64-installer.exe` (and/or the `.zip`) |
| **Category / tags** | OBS, Streaming, Privacy, Security, Screen Recording, Developer Tools |
| **License delivered** | GPL-2.0 (state clearly; see the transparency block in the description) |

---

## Description (paste into the product body)

**Never leak a secret on stream again.**

If you live-code, demo dashboards, or share your screen, one stray API key,
password, or QR code can end up on camera — and once it's out, it's out. **Censr**
is an OBS Studio filter that watches your capture and covers sensitive content
with a solid, opaque block **in real time**, before your viewers can read it.

Add it to any source, hit go, and stream with peace of mind.

### What it catches

- **API keys & tokens** — `sk-…`, `ghp_…`, `AKIA…`, and other high-entropy secrets
- **Passwords & secrets** — `password = …`, `token: …`, and similar lines
- **Email addresses**
- **QR codes & barcodes** — which can silently encode links or credentials

### Why it's different

- **Opaque covers, never blur.** Blur and pixelation can be reversed — Censr uses
  solid fills, so a covered secret is *gone*.
- **Zero-leak mode.** An optional ~1.5-second delay means a frame is only shown
  *after* detection finishes — a flagged secret is never displayed uncovered, not
  even for a single frame.
- **GPU-accelerated & smooth.** Detection runs off the render thread on your GPU,
  so your framerate stays put.
- **100% local.** No cloud, no network calls, no telemetry — your video never
  leaves your machine.
- **Configurable.** Choose which categories to redact, the cover color, the delay,
  and even per-word redaction (hide the secret value, keep the label visible).

### What you get

- A **signed, prebuilt installer** — double-click and go, no compiling.
- Free updates for this major version.
- Support, and you're directly funding continued development. ❤️

### Requirements

Windows 10/11 (x64) and OBS Studio 31 or newer. Any modern (Direct3D 11) GPU.

### Open source & honest by design 🔍

Censr is **free and open source (GPL-2.0)** — the full code is on GitHub, so you
(or anyone) can audit exactly what a security tool that watches your screen is
doing. Buying here gets you the **convenient signed build, updates, and support**,
and directly funds the project. Prefer to build it yourself? That's always free.

**A note on what it can and can't do:** Censr is a powerful safety net, not a
guarantee. Automated detection can miss secrets in unusual fonts, tiny text, or
heavy motion. Keep zero-leak mode on for the strongest protection, and don't rely
on it alone for highly sensitive material.

---

## Pricing strategy notes (not part of the listing)

Because the binary is freely buildable from GitHub, don't price it as exclusive
software. Good options:

- **Pay-what-you-want** (suggested $10, min $3) — lowest friction, captures
  goodwill, feels fair for open source. *Recommended.*
- **Low fixed price** ($7–$12) — "skip the build, get the signed installer + support."
- **Two tiers** — *Supporter* ($9: installer + updates + support) and
  *Team/Studio* ($39–$49: same + priority support / invoice), if you want a
  business SKU.

Consider a **free $0 tier or a visible "also free on GitHub" link** — for a
security tool, transparency converts better than a paywall and avoids any
impression you're hiding the source.

## Assets to upload

- **Product file:** the signed `censr-<version>-windows-x64-installer.exe` (build
  it once you have the signing cert wired, so buyers get a signed binary).
- **Cover image / thumbnail:** `docs/demo.png` (secrets covered on a mock
  terminal). The `docs/demo.gif` works great in the description if Lemon Squeezy
  renders it.

## Pre-publish checklist

- [ ] Sign the installer (Trusted Signing / cert) so buyers don't hit SmartScreen.
- [ ] Upload the signed installer as the product file.
- [ ] Upload cover image (demo.png) + optional GIF.
- [ ] Set price / pay-what-you-want.
- [ ] Add the GitHub + OBS-forum links so buyers can verify the source.
- [ ] (Recommended) do the real-world live-capture smoke test first.
