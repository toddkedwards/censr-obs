# License gating for the host + virtual cameras (Windows) — handoff

The Lemon Squeezy license gate now lives in **`censr-core`** (`src/license.{h,cpp}`
+ `src/http.*`), libobs-free, so the standalone **host** and the **virtual cameras**
(which already link `censr-core`) can use it directly. The OBS plugin is done and
verified on macOS. This is the Windows follow-up.

## What's already done (in `censr-core`)

- `censr::License` (singleton via `censr::license()`):
  - `load(const std::string &config_dir)` — pass `""` to use the **shared per-user
    dir** (`%APPDATA%\Censr` on Windows). All three binaries using `""` read/write
    the same file, so **a key activated anywhere licenses the whole suite.**
  - `bool active()` — cheap, lock-free; read it every frame.
  - `bool activate(const std::string &key)` — blocking network call; run OFF the
    render thread.
  - `std::string status_text()` — one line for a settings UI.
  - `deactivate()`.
- Activation is **locked to product 1342934** (Censr for OBS). A key from any other
  store/product is rejected and its LS instance released. 14-day offline grace.
- HTTP: `http_win.cpp` (WinHTTP) — `censr-core` links `winhttp` on Windows already.
- QA: set env `CENSR_ACTIVATE_KEY=<key>` to auto-activate headlessly on load.

## To do

### 1. Both host + vcam: load the license at startup
```cpp
#include "license.h"
censr::license().load(""); // shared %APPDATA%\Censr dir
```
`active()` is then valid to read on the render/compositor thread.

### 2. Standalone host (has a UI): add activation + watermark
- A **license-key text field + "Activate" button** that calls
  `censr::license().activate(key)` on a worker thread (it blocks on the network),
  then shows `status_text()`.
- When `!censr::license().active()`, draw the **unlicensed watermark** into the
  composited D3D11 frame (see spec below).

### 3. Virtual cameras (no UI): watermark only
- No activation UI — they inherit the license activated by the host/plugin via the
  shared file. Just `load("")` once, then in the frame composite:
  `if (!censr::license().active()) draw_watermark(...)`.

### Watermark spec (match the OBS plugin so it's consistent)
- Amber `rgb(255, 191, 0)` at **alpha 0.72**, a rounded rect ~**250 x 40 px**,
  bottom-right, **20 px** margin. Redaction ALWAYS runs; the watermark only nags.

## Acceptance test (on the PC)
1. Do a **test-mode checkout** of product 1342934 (Censr for OBS) to mint a real
   OBS-product key (test card `4242 4242 4242 4242`).
2. Activate it in the host → the host, the OBS plugin, and the vcam should all show
   licensed (they share `%APPDATA%\Censr`), watermark gone.
3. A key from a different product should be rejected (watermark stays).

Branch: `license-core`. Merge after the host/vcam gating is wired + verified.
