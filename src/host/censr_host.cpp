/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

Standalone Censr host (Phase 1 MVP). Captures a monitor with
Windows.Graphics.Capture, runs the shared censr-core detection engine, and
composites opaque amber covers over a delayed copy of the frame in D3D11,
previewing the result in a window. No OBS, no virtual camera (that is Phase 2).
See docs/roadmap-standalone-vcam.md.
*/

#include "wgc_capture.h"

#include "censr_detector.h"
#include "censr_log.h"
#include "ocr_engine.h"
#include "secret_rules.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <winrt/base.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

/* ---- tunables (mirror the OBS filter) ---- */
constexpr uint64_t NS = 1000000000ull;
constexpr uint64_t TICK_INTERVAL_NS = NS / 30;          /* 30 Hz pipeline */
constexpr uint64_t DETECT_INTERVAL_NS = NS / 5;         /* 5 Hz detection */
constexpr uint64_t DELAY_NS = 1200000000ull;            /* zero-leak delay */
constexpr uint64_t GUARANTEE_MIN_DELAY_NS = 500000000ull;
constexpr uint64_t SNAPSHOT_KEEP_NS = 3000000000ull;
constexpr float COVER_PAD = 8.0f;
constexpr uint32_t DET_MAX_W = 1600; /* OCR runs on a frame no wider than this */

uint64_t now_ns()
{
	static LARGE_INTEGER freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f;
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}

struct Box {
	float x, y, w, h;
};
struct Snapshot {
	uint64_t ts = 0;
	std::vector<Box> boxes; /* capture pixel space, padded */
};

/* ---- HLSL: a fullscreen textured blit, and a solid-color quad from a cb ---- */
const char *kFrameShader = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VOut VSMain(uint id : SV_VertexID) {
	VOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.uv = uv;
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float4 PSMain(VOut i) : SV_TARGET { return tex.Sample(smp, i.uv); }
)";

const char *kRectShader = R"(
cbuffer RectCB : register(b0) { float4 rect; float4 color; };  // rect = x0,y0,x1,y1 (NDC)
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
	float2 p = float2((id == 1 || id == 3) ? rect.z : rect.x,
			  (id >= 2) ? rect.w : rect.y);
	return float4(p, 0, 1);
}
float4 PSMain() : SV_TARGET { return color; }
)";

struct RectCB {
	float rect[4];
	float color[4];
};

ComPtr<ID3DBlob> compile(const char *src, const char *entry, const char *target)
{
	ComPtr<ID3DBlob> blob, err;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry,
				target, 0, 0, &blob, &err);
	if (FAILED(hr)) {
		censr_log(LOG_ERROR, "Censr host: shader compile failed (%s): %s",
			  entry, err ? (const char *)err->GetBufferPointer() : "?");
		return nullptr;
	}
	return blob;
}

/* ---- delay ring + detection snapshots ---- */
struct RingSlot {
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11ShaderResourceView> srv;
	uint64_t ts = 0;
	bool has_content = false;
};

std::mutex g_snap_mutex;
std::deque<Snapshot> g_snapshots;

/* Detector callback: store boxes (padded) tagged with the frame ts. */
void on_detection(uint64_t ts, const std::vector<censr::RectF> &boxes)
{
	Snapshot s;
	s.ts = ts;
	for (const auto &b : boxes) {
		float x = std::max(0.f, b.x - COVER_PAD);
		float y = std::max(0.f, b.y - COVER_PAD);
		s.boxes.push_back({x, y, b.w + 2 * COVER_PAD, b.h + 2 * COVER_PAD});
	}
	std::lock_guard<std::mutex> lk(g_snap_mutex);
	g_snapshots.push_back(std::move(s));
	uint64_t now = now_ns();
	while (!g_snapshots.empty() &&
	       g_snapshots.front().ts + SNAPSHOT_KEEP_NS < now)
		g_snapshots.pop_front();
}

struct CoverSet {
	std::vector<Box> boxes;
	bool detection_current = false;
};

/* Union every snapshot within one detection cadence of display_ts, and report
 * whether detection has reached this era. (Same rule as the OBS filter.) */
CoverSet covers_for(uint64_t display_ts)
{
	const uint64_t win = DETECT_INTERVAL_NS + 2 * TICK_INTERVAL_NS;
	CoverSet out;
	std::lock_guard<std::mutex> lk(g_snap_mutex);
	for (const auto &s : g_snapshots) {
		uint64_t dt = s.ts > display_ts ? s.ts - display_ts
						: display_ts - s.ts;
		if (dt <= win)
			out.boxes.insert(out.boxes.end(), s.boxes.begin(),
					 s.boxes.end());
		if (s.ts >= display_ts)
			out.detection_current = true;
	}
	return out;
}

/* ---- source picker ---- */
struct MonInfo {
	HMONITOR h;
	int w, hgt;
	bool primary;
};
struct WinInfo {
	HWND h;
	std::string title;
};

std::vector<MonInfo> enum_monitors()
{
	std::vector<MonInfo> mons;
	EnumDisplayMonitors(
		nullptr, nullptr,
		[](HMONITOR h, HDC, LPRECT r, LPARAM lp) -> BOOL {
			MONITORINFO mi{sizeof(mi)};
			GetMonitorInfo(h, &mi);
			((std::vector<MonInfo> *)lp)
				->push_back({h, (int)(r->right - r->left),
					     (int)(r->bottom - r->top),
					     (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
			return TRUE;
		},
		(LPARAM)&mons);
	return mons;
}

std::vector<WinInfo> enum_windows()
{
	std::vector<WinInfo> wins;
	EnumWindows(
		[](HWND h, LPARAM lp) -> BOOL {
			if (!IsWindowVisible(h) || IsIconic(h))
				return TRUE;
			if (GetWindow(h, GW_OWNER))
				return TRUE;
			if (GetWindowLongPtr(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
				return TRUE;
			char t[256];
			int n = GetWindowTextA(h, t, sizeof(t));
			if (n <= 0)
				return TRUE;
			((std::vector<WinInfo> *)lp)
				->push_back({h, std::string(t, n)});
			return TRUE;
		},
		(LPARAM)&wins);
	return wins;
}

enum class SrcKind { Monitor, Window };
struct Source {
	SrcKind kind = SrcKind::Monitor;
	HMONITOR mon = nullptr;
	HWND win = nullptr;
};

/* Resolve the capture source from argv (--monitor N / --window N) or, absent
 * those, an interactive console list. Falls back to the primary monitor. */
Source pick_source(int argc, char **argv, const std::vector<MonInfo> &mons,
		   const std::vector<WinInfo> &wins)
{
	Source src;
	auto primary = [&] {
		for (const auto &m : mons)
			if (m.primary)
				return m.h;
		return mons.empty() ? (HMONITOR) nullptr : mons[0].h;
	};
	src.mon = primary();

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--monitor") && i + 1 < argc) {
			int n = atoi(argv[++i]);
			if (n >= 0 && n < (int)mons.size()) {
				src.kind = SrcKind::Monitor;
				src.mon = mons[n].h;
			}
			return src;
		}
		if (!strcmp(argv[i], "--window") && i + 1 < argc) {
			int n = atoi(argv[++i]);
			if (n >= 0 && n < (int)wins.size()) {
				src.kind = SrcKind::Window;
				src.win = wins[n].h;
			}
			return src;
		}
	}

	/* Interactive. */
	std::printf("\nSelect a capture source:\n");
	for (size_t i = 0; i < mons.size(); ++i)
		std::printf("  m%zu: monitor %dx%d%s\n", i, mons[i].w, mons[i].hgt,
			    mons[i].primary ? " (primary)" : "");
	for (size_t i = 0; i < wins.size(); ++i)
		std::printf("  w%zu: %s\n", i, wins[i].title.c_str());
	std::printf("Choice (e.g. m0 or w2) [default m-primary]: ");
	std::fflush(stdout);

	std::string line;
	std::getline(std::cin, line);
	if (line.size() >= 2 && (line[0] == 'm' || line[0] == 'w')) {
		int n = atoi(line.c_str() + 1);
		if (line[0] == 'm' && n >= 0 && n < (int)mons.size()) {
			src.kind = SrcKind::Monitor;
			src.mon = mons[n].h;
		} else if (line[0] == 'w' && n >= 0 && n < (int)wins.size()) {
			src.kind = SrcKind::Window;
			src.win = wins[n].h;
		}
	}
	return src;
}

/* ---- globals for the window proc ---- */
bool g_quit = false;

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_DESTROY || m == WM_CLOSE) {
		g_quit = true;
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(h, m, w, l);
}

} // namespace

int main(int argc, char **argv)
{
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	/* ---- pick a source before creating our own window (so it's excluded) ---- */
	auto mons = enum_monitors();
	auto wins = enum_windows();
	Source src = pick_source(argc, argv, mons, wins);

	/* ---- window ---- */
	HINSTANCE inst = GetModuleHandle(nullptr);
	WNDCLASS wc{};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.lpszClassName = L"CensrHost";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClass(&wc);
	HWND hwnd = CreateWindow(wc.lpszClassName, L"Censr - preview (redacted)",
				 WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
				 CW_USEDEFAULT, 1280, 720, nullptr, nullptr, inst,
				 nullptr);
	if (!hwnd) {
		censr_log(LOG_ERROR, "Censr host: CreateWindow failed");
		return 1;
	}

	/* ---- D3D11 device + swapchain ---- */
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL fl;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				     flags, nullptr, 0, D3D11_SDK_VERSION,
				     &device, &fl, &ctx))) {
		censr_log(LOG_ERROR, "Censr host: D3D11CreateDevice failed");
		return 1;
	}

	ComPtr<IDXGIDevice> dxgiDev;
	device.As(&dxgiDev);
	ComPtr<IDXGIAdapter> adapter;
	dxgiDev->GetAdapter(&adapter);
	ComPtr<IDXGIFactory2> factory;
	adapter->GetParent(IID_PPV_ARGS(&factory));

	DXGI_SWAP_CHAIN_DESC1 scd{};
	scd.Width = 0;
	scd.Height = 0;
	scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ComPtr<IDXGISwapChain1> swap;
	if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &scd,
						   nullptr, nullptr, &swap))) {
		censr_log(LOG_ERROR, "Censr host: CreateSwapChainForHwnd failed");
		return 1;
	}

	/* ---- shaders / states ---- */
	auto frameVS = compile(kFrameShader, "VSMain", "vs_5_0");
	auto framePS = compile(kFrameShader, "PSMain", "ps_5_0");
	auto rectVS = compile(kRectShader, "VSMain", "vs_5_0");
	auto rectPS = compile(kRectShader, "PSMain", "ps_5_0");
	if (!frameVS || !framePS || !rectVS || !rectPS)
		return 1;

	ComPtr<ID3D11VertexShader> vsFrame, vsRect;
	ComPtr<ID3D11PixelShader> psFrame, psRect;
	device->CreateVertexShader(frameVS->GetBufferPointer(),
				   frameVS->GetBufferSize(), nullptr, &vsFrame);
	device->CreatePixelShader(framePS->GetBufferPointer(),
				  framePS->GetBufferSize(), nullptr, &psFrame);
	device->CreateVertexShader(rectVS->GetBufferPointer(),
				   rectVS->GetBufferSize(), nullptr, &vsRect);
	device->CreatePixelShader(rectPS->GetBufferPointer(),
				  rectPS->GetBufferSize(), nullptr, &psRect);

	ComPtr<ID3D11SamplerState> sampler;
	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	device->CreateSamplerState(&sd, &sampler);

	ComPtr<ID3D11Buffer> rectCB;
	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = sizeof(RectCB);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	device->CreateBuffer(&bd, nullptr, &rectCB);

	/* ---- capture ---- */
	censr::WgcCapture capture;
	bool started = src.kind == SrcKind::Window
			       ? capture.start_window(device.Get(), src.win)
			       : capture.start_monitor(device.Get(), src.mon);
	if (!started) {
		censr_log(LOG_ERROR, "Censr host: could not start capture");
		return 1;
	}

	/* ---- detector (Windows.Media.Ocr — no ORT DLLs needed for the MVP) ---- */
	auto detector = std::make_unique<censr::Detector>(
		on_detection, "", "", censr::Detector::Engine::WINRT);

	/* ---- delay ring (built lazily once we know the capture size) ---- */
	std::deque<RingSlot> ring;
	/* Detection runs on a GPU-downscaled copy (<= DET_MAX_W wide): the frame
	 * is rendered into detTex, then read back via detStaging. Much cheaper
	 * than reading back and OCR-ing the full (e.g. 4K) frame; the detector
	 * scales the boxes back to capture space. */
	ComPtr<ID3D11Texture2D> detTex, detStaging;
	ComPtr<ID3D11RenderTargetView> detRtv;
	uint32_t capW = 0, capH = 0, detW = 0, detH = 0;
	size_t wanted = (size_t)(DELAY_NS / TICK_INTERVAL_NS);

	auto make_slot = [&](uint32_t w, uint32_t h) {
		RingSlot s;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		device->CreateTexture2D(&td, nullptr, &s.tex);
		device->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv);
		return s;
	};

	uint64_t last_tick = 0, last_detect = 0;

	/* ---- main loop ---- */
	while (!g_quit) {
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if (g_quit)
			break;

		uint64_t now = now_ns();
		if (now - last_tick < TICK_INTERVAL_NS) {
			Sleep(1);
			continue;
		}
		last_tick = now;

		if (!capture.poll(ctx.Get()))
			continue;

		/* Build the ring the first time we know the capture size. */
		if (capW != capture.width() || capH != capture.height()) {
			capW = capture.width();
			capH = capture.height();
			ring.clear();
			for (size_t i = 0; i < wanted; ++i)
				ring.push_back(make_slot(capW, capH));

			/* Detection target sized to <= DET_MAX_W, preserving aspect. */
			detW = std::min(capW, DET_MAX_W);
			detH = std::max(1u, (uint32_t)((uint64_t)capH * detW / capW));
			detTex.Reset();
			detRtv.Reset();
			detStaging.Reset();
			D3D11_TEXTURE2D_DESC dt{};
			dt.Width = detW;
			dt.Height = detH;
			dt.MipLevels = 1;
			dt.ArraySize = 1;
			dt.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			dt.SampleDesc.Count = 1;
			dt.Usage = D3D11_USAGE_DEFAULT;
			dt.BindFlags = D3D11_BIND_RENDER_TARGET;
			device->CreateTexture2D(&dt, nullptr, &detTex);
			device->CreateRenderTargetView(detTex.Get(), nullptr, &detRtv);
			dt.Usage = D3D11_USAGE_STAGING;
			dt.BindFlags = 0;
			dt.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			device->CreateTexture2D(&dt, nullptr, &detStaging);

			std::lock_guard<std::mutex> lk(g_snap_mutex);
			g_snapshots.clear();
		}
		if (ring.empty())
			continue;

		/* Recycle oldest slot -> copy the just-captured frame into it. */
		RingSlot slot = ring.front();
		ring.pop_front();
		ctx->CopyResource(slot.tex.Get(), capture.latest());
		slot.ts = now;
		slot.has_content = true;
		ring.push_back(slot);

		/* ~5 Hz: GPU-downscale the newest frame, read it back, and submit
		 * for detection (boxes come back in capture space via src_w/h). */
		if (now - last_detect >= DETECT_INTERVAL_NS && detStaging) {
			last_detect = now;

			D3D11_VIEWPORT dvp{0, 0, (float)detW, (float)detH, 0, 1};
			ctx->OMSetRenderTargets(1, detRtv.GetAddressOf(), nullptr);
			ctx->RSSetViewports(1, &dvp);
			ctx->IASetPrimitiveTopology(
				D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->IASetInputLayout(nullptr);
			ctx->VSSetShader(vsFrame.Get(), nullptr, 0);
			ctx->PSSetShader(psFrame.Get(), nullptr, 0);
			ctx->PSSetShaderResources(0, 1, ring.back().srv.GetAddressOf());
			ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
			ctx->Draw(3, 0);
			ID3D11ShaderResourceView *unbind = nullptr;
			ctx->PSSetShaderResources(0, 1, &unbind);

			ctx->CopyResource(detStaging.Get(), detTex.Get());
			D3D11_MAPPED_SUBRESOURCE map;
			if (SUCCEEDED(ctx->Map(detStaging.Get(), 0, D3D11_MAP_READ,
					       0, &map))) {
				std::vector<uint8_t> buf((size_t)detW * detH * 4);
				for (uint32_t y = 0; y < detH; ++y) {
					const uint8_t *row =
						(const uint8_t *)map.pData +
						(size_t)y * map.RowPitch;
					uint8_t *dst = buf.data() +
						       (size_t)y * detW * 4;
					for (uint32_t x = 0; x < detW; ++x) {
						/* BGRA -> RGBA for the detector. */
						dst[x * 4 + 0] = row[x * 4 + 2];
						dst[x * 4 + 1] = row[x * 4 + 1];
						dst[x * 4 + 2] = row[x * 4 + 0];
						dst[x * 4 + 3] = row[x * 4 + 3];
					}
				}
				ctx->Unmap(detStaging.Get(), 0);
				detector->submit(std::move(buf), (int)detW,
						 (int)detH, (int)capW, (int)capH,
						 now);
			}
		}

		/* ---- render the delayed frame + covers to the backbuffer ---- */
		ComPtr<ID3D11Texture2D> back;
		swap->GetBuffer(0, IID_PPV_ARGS(&back));
		ComPtr<ID3D11RenderTargetView> rtv;
		device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
		D3D11_TEXTURE2D_DESC bdesc{};
		back->GetDesc(&bdesc);
		D3D11_VIEWPORT vp{0, 0, (float)bdesc.Width, (float)bdesc.Height,
				  0, 1};
		ctx->RSSetViewports(1, &vp);
		ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
		float black[4] = {0, 0, 0, 1};
		ctx->ClearRenderTargetView(rtv.Get(), black);

		RingSlot &disp = ring.front();
		CoverSet cov = covers_for(disp.ts);
		bool guarantee = DELAY_NS >= GUARANTEE_MIN_DELAY_NS;
		bool show = disp.has_content && !(guarantee && !cov.detection_current);

		if (show) {
			/* Blit the delayed frame. */
			ctx->IASetPrimitiveTopology(
				D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->IASetInputLayout(nullptr);
			ctx->VSSetShader(vsFrame.Get(), nullptr, 0);
			ctx->PSSetShader(psFrame.Get(), nullptr, 0);
			ctx->PSSetShaderResources(0, 1, disp.srv.GetAddressOf());
			ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
			ctx->Draw(3, 0);

			/* Opaque amber covers. */
			ctx->IASetPrimitiveTopology(
				D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			ctx->VSSetShader(vsRect.Get(), nullptr, 0);
			ctx->PSSetShader(psRect.Get(), nullptr, 0);
			ctx->VSSetConstantBuffers(0, 1, rectCB.GetAddressOf());
			ID3D11ShaderResourceView *none = nullptr;
			ctx->PSSetShaderResources(0, 1, &none);
			for (const auto &b : cov.boxes) {
				RectCB cb{};
				cb.rect[0] = b.x / capW * 2.f - 1.f;
				cb.rect[1] = 1.f - b.y / capH * 2.f;
				cb.rect[2] = (b.x + b.w) / capW * 2.f - 1.f;
				cb.rect[3] = 1.f - (b.y + b.h) / capH * 2.f;
				cb.color[0] = 1.0f; /* amber */
				cb.color[1] = 0.749f;
				cb.color[2] = 0.0f;
				cb.color[3] = 1.0f;
				D3D11_MAPPED_SUBRESOURCE mm;
				if (SUCCEEDED(ctx->Map(rectCB.Get(), 0,
						       D3D11_MAP_WRITE_DISCARD, 0,
						       &mm))) {
					memcpy(mm.pData, &cb, sizeof(cb));
					ctx->Unmap(rectCB.Get(), 0);
				}
				ctx->Draw(4, 0);
			}
		}
		/* else: backbuffer stays black (fail closed). */

		swap->Present(1, 0);
	}

	detector.reset();
	capture.stop();
	censr_log(LOG_INFO, "Censr host: exiting");
	return 0;
}
