/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/* Windows.Graphics.Capture wrapper for the standalone host. Captures a monitor
 * into D3D11 textures. Poll-based (CreateFreeThreaded + TryGetNextFrame) so all
 * D3D11 immediate-context work happens on the caller's render thread — no
 * cross-thread context use, no DispatcherQueue needed. */

#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d11.h>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace censr {

class WgcCapture {
public:
	/* Begin capturing `mon` with `device`. Returns false if WGC is
	 * unsupported or setup fails. */
	bool start(ID3D11Device *device, HMONITOR mon);
	void stop();

	/* Drain buffered frames on the render thread and copy the newest into an
	 * internally owned texture. Returns true if a frame is now available. The
	 * capture size is reported via width()/height(). */
	bool poll(ID3D11DeviceContext *ctx);

	/* The most recently polled frame (nullptr until poll() first succeeds).
	 * Owned by WgcCapture; valid until the next poll()/stop(). */
	ID3D11Texture2D *latest() const { return latest_.get(); }

	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }

	~WgcCapture() { stop(); }

private:
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rt_device_{nullptr};

	winrt::com_ptr<ID3D11Texture2D> latest_;
	uint32_t width_ = 0, height_ = 0;
};

} // namespace censr
