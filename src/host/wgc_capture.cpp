/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "wgc_capture.h"
#include "censr_log.h"

#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WDX = winrt::Windows::Graphics::DirectX;
namespace WD3D = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace censr {

/* Pull the ID3D11Texture2D backing a WinRT Direct3D surface. */
static winrt::com_ptr<ID3D11Texture2D>
surface_texture(const WD3D::IDirect3DSurface &surface)
{
	auto access =
		surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
	winrt::com_ptr<ID3D11Texture2D> tex;
	winrt::check_hresult(
		access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()));
	return tex;
}

bool WgcCapture::start(ID3D11Device *device, HMONITOR mon)
{
	if (!WGC::GraphicsCaptureSession::IsSupported()) {
		censr_log(LOG_WARNING,
			  "Censr host: Windows.Graphics.Capture not supported");
		return false;
	}
	try {
		/* Wrap the D3D11 device as a WinRT IDirect3DDevice. */
		winrt::com_ptr<IDXGIDevice> dxgi;
		winrt::check_hresult(device->QueryInterface(
			winrt::guid_of<IDXGIDevice>(), dxgi.put_void()));
		winrt::com_ptr<::IInspectable> insp;
		winrt::check_hresult(
			CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
		rt_device_ = insp.as<WD3D::IDirect3DDevice>();

		/* Build a capture item for the monitor. */
		auto interop = winrt::get_activation_factory<
			WGC::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
		winrt::check_hresult(interop->CreateForMonitor(
			mon, winrt::guid_of<WGC::GraphicsCaptureItem>(),
			winrt::put_abi(item_)));

		auto size = item_.Size();
		width_ = (uint32_t)size.Width;
		height_ = (uint32_t)size.Height;

		pool_ = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
			rt_device_, WDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			2, size);
		session_ = pool_.CreateCaptureSession(item_);
		session_.StartCapture();
		censr_log(LOG_INFO, "Censr host: capturing %ux%u monitor", width_,
			  height_);
		return true;
	} catch (const winrt::hresult_error &e) {
		censr_log(LOG_WARNING, "Censr host: capture start failed: 0x%08x",
			  (unsigned)e.code());
		stop();
		return false;
	}
}

void WgcCapture::stop()
{
	if (session_) {
		session_.Close();
		session_ = nullptr;
	}
	if (pool_) {
		pool_.Close();
		pool_ = nullptr;
	}
	item_ = nullptr;
	rt_device_ = nullptr;
	latest_ = nullptr;
}

bool WgcCapture::poll(ID3D11DeviceContext *ctx)
{
	if (!pool_)
		return latest_ != nullptr;

	/* Drain to the newest buffered frame. */
	WGC::Direct3D11CaptureFrame frame{nullptr};
	for (;;) {
		auto next = pool_.TryGetNextFrame();
		if (!next)
			break;
		frame = next;
	}
	if (!frame)
		return latest_ != nullptr;

	auto src = surface_texture(frame.Surface());
	D3D11_TEXTURE2D_DESC desc{};
	src->GetDesc(&desc);

	/* (Re)create our owned copy target if size/format changed. */
	if (!latest_ || width_ != desc.Width || height_ != desc.Height) {
		latest_ = nullptr;
		D3D11_TEXTURE2D_DESC cd = desc;
		cd.Usage = D3D11_USAGE_DEFAULT;
		cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		cd.CPUAccessFlags = 0;
		cd.MiscFlags = 0;
		winrt::com_ptr<ID3D11Device> dev;
		ctx->GetDevice(dev.put());
		if (FAILED(dev->CreateTexture2D(&cd, nullptr, latest_.put())))
			return false;
		width_ = desc.Width;
		height_ = desc.Height;
	}

	ctx->CopyResource(latest_.get(), src.get());
	frame.Close();
	return true;
}

} // namespace censr
