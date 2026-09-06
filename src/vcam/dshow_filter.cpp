/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

A self-contained (no DirectShow BaseClasses) push-source capture filter that
serves the host's shared redacted-frame stream to legacy apps that enumerate
DirectShow video-input devices. Fixed 1920x1080 RGB32; frames are read from
SharedFrameReader, scaled, and flipped to DirectShow's bottom-up RGB.

Like the MF source, this compiles and links against the SDK and reads the live
stream, but is not yet validated in a real DirectShow app (needs signed
registration under the video-input category + a capture app). The connection /
allocator negotiation is the usual place a hand-rolled filter needs tuning.
See docs/virtual-camera-install.md.
*/

#include "dshow_filter.h"
#include "dshow_guid.h"
#include "shared_frame.h"

#include <windows.h>

#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>

#include <vector>

#include <wrl/client.h>
#include <wrl/implements.h>

using namespace Microsoft::WRL;

namespace {

constexpr REFERENCE_TIME kFrameDur = 10000000LL / CENSR_DSHOW_FPS;
constexpr DWORD kImageSize = CENSR_DSHOW_WIDTH * CENSR_DSHOW_HEIGHT * 4;

void fill_media_type(AM_MEDIA_TYPE *mt, VIDEOINFOHEADER *vih)
{
	ZeroMemory(vih, sizeof(*vih));
	vih->AvgTimePerFrame = kFrameDur;
	vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	vih->bmiHeader.biWidth = CENSR_DSHOW_WIDTH;
	vih->bmiHeader.biHeight = CENSR_DSHOW_HEIGHT; /* bottom-up RGB */
	vih->bmiHeader.biPlanes = 1;
	vih->bmiHeader.biBitCount = 32;
	vih->bmiHeader.biCompression = BI_RGB;
	vih->bmiHeader.biSizeImage = kImageSize;

	ZeroMemory(mt, sizeof(*mt));
	mt->majortype = MEDIATYPE_Video;
	mt->subtype = MEDIASUBTYPE_RGB32;
	mt->formattype = FORMAT_VideoInfo;
	mt->bFixedSizeSamples = TRUE;
	mt->lSampleSize = kImageSize;
	mt->cbFormat = sizeof(VIDEOINFOHEADER);
	mt->pbFormat = (BYTE *)vih;
}

/* Allocate a heap AM_MEDIA_TYPE (+ format) for handing out via COM. */
AM_MEDIA_TYPE *alloc_media_type()
{
	auto *mt = (AM_MEDIA_TYPE *)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
	auto *vih =
		(VIDEOINFOHEADER *)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
	fill_media_type(mt, vih);
	return mt;
}

void free_media_type(AM_MEDIA_TYPE *mt)
{
	if (mt->pbFormat)
		CoTaskMemFree(mt->pbFormat);
	CoTaskMemFree(mt);
}

/* Scale BGRA src (sw x sh, top-down) into dst (1920x1080) flipped bottom-up. */
void scale_flip(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst)
{
	const uint32_t dw = CENSR_DSHOW_WIDTH, dh = CENSR_DSHOW_HEIGHT;
	for (uint32_t y = 0; y < dh; ++y) {
		uint32_t sy = sh ? (y * sh / dh) : 0;
		const uint8_t *srow = src + (size_t)sy * sw * 4;
		uint8_t *drow = dst + (size_t)(dh - 1 - y) * dw * 4; /* flip */
		for (uint32_t x = 0; x < dw; ++x) {
			uint32_t sx = sw ? (x * sw / dw) : 0;
			memcpy(drow + x * 4, srow + sx * 4, 4);
		}
	}
}

class CensrFilter;

/* ---- IEnumMediaTypes over our single format ---- */
class EnumMT : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
				   IEnumMediaTypes> {
public:
	void Init(ULONG pos) { pos_ = pos; }
	IFACEMETHODIMP Next(ULONG c, AM_MEDIA_TYPE **out, ULONG *fetched) override
	{
		ULONG got = 0;
		while (got < c && pos_ < 1) {
			out[got++] = alloc_media_type();
			++pos_;
		}
		if (fetched)
			*fetched = got;
		return got == c ? S_OK : S_FALSE;
	}
	IFACEMETHODIMP Skip(ULONG c) override
	{
		pos_ += c;
		return pos_ <= 1 ? S_OK : S_FALSE;
	}
	IFACEMETHODIMP Reset() override
	{
		pos_ = 0;
		return S_OK;
	}
	IFACEMETHODIMP Clone(IEnumMediaTypes **out) override
	{
		auto e = Make<EnumMT>();
		e->Init(pos_);
		return e.CopyTo(out);
	}

private:
	ULONG pos_ = 0;
};

/* ---- output pin ---- */
class CensrPin : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IPin,
				     IAMStreamConfig, IKsPropertySet> {
public:
	void Init(CensrFilter *filter) { filter_ = filter; }

	/* IPin */
	IFACEMETHODIMP Connect(IPin *recv, const AM_MEDIA_TYPE *) override;
	IFACEMETHODIMP ReceiveConnection(IPin *, const AM_MEDIA_TYPE *) override
	{
		return E_UNEXPECTED; /* output pin */
	}
	IFACEMETHODIMP Disconnect() override;
	IFACEMETHODIMP ConnectedTo(IPin **p) override
	{
		if (!peer_)
			return VFW_E_NOT_CONNECTED;
		return peer_.CopyTo(p);
	}
	IFACEMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *mt) override
	{
		if (!peer_)
			return VFW_E_NOT_CONNECTED;
		VIDEOINFOHEADER vih;
		fill_media_type(mt, &vih);
		mt->pbFormat = (BYTE *)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
		memcpy(mt->pbFormat, &vih, sizeof(vih));
		return S_OK;
	}
	IFACEMETHODIMP QueryPinInfo(PIN_INFO *info) override;
	IFACEMETHODIMP QueryDirection(PIN_DIRECTION *d) override
	{
		*d = PINDIR_OUTPUT;
		return S_OK;
	}
	IFACEMETHODIMP QueryId(LPWSTR *id) override
	{
		*id = (LPWSTR)CoTaskMemAlloc(sizeof(L"Capture"));
		memcpy(*id, L"Capture", sizeof(L"Capture"));
		return S_OK;
	}
	IFACEMETHODIMP QueryAccept(const AM_MEDIA_TYPE *mt) override
	{
		return (mt->majortype == MEDIATYPE_Video &&
			mt->subtype == MEDIASUBTYPE_RGB32)
			       ? S_OK
			       : S_FALSE;
	}
	IFACEMETHODIMP EnumMediaTypes(IEnumMediaTypes **e) override
	{
		auto en = Make<EnumMT>();
		en->Init(0);
		return en.CopyTo(e);
	}
	IFACEMETHODIMP QueryInternalConnections(IPin **, ULONG *) override
	{
		return E_NOTIMPL;
	}
	IFACEMETHODIMP EndOfStream() override { return S_OK; }
	IFACEMETHODIMP BeginFlush() override { return S_OK; }
	IFACEMETHODIMP EndFlush() override { return S_OK; }
	IFACEMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME,
				  double) override
	{
		return S_OK;
	}

	/* IAMStreamConfig */
	IFACEMETHODIMP SetFormat(AM_MEDIA_TYPE *) override { return S_OK; }
	IFACEMETHODIMP GetFormat(AM_MEDIA_TYPE **mt) override
	{
		*mt = alloc_media_type();
		return S_OK;
	}
	IFACEMETHODIMP GetNumberOfCapabilities(int *count, int *size) override
	{
		*count = 1;
		*size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
		return S_OK;
	}
	IFACEMETHODIMP GetStreamCaps(int i, AM_MEDIA_TYPE **mt,
				     BYTE *scc) override
	{
		if (i != 0)
			return S_FALSE;
		*mt = alloc_media_type();
		auto *caps = (VIDEO_STREAM_CONFIG_CAPS *)scc;
		ZeroMemory(caps, sizeof(*caps));
		caps->guid = FORMAT_VideoInfo;
		caps->MinFrameInterval = kFrameDur;
		caps->MaxFrameInterval = kFrameDur;
		caps->MinOutputSize.cx = caps->MaxOutputSize.cx =
			CENSR_DSHOW_WIDTH;
		caps->MinOutputSize.cy = caps->MaxOutputSize.cy =
			CENSR_DSHOW_HEIGHT;
		return S_OK;
	}

	/* IKsPropertySet: report the pin category (capture). */
	IFACEMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override
	{
		return E_NOTIMPL;
	}
	IFACEMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data,
			   DWORD bytes, DWORD *ret) override
	{
		if (set != AMPROPSETID_Pin || id != AMPROPERTY_PIN_CATEGORY)
			return E_PROP_ID_UNSUPPORTED;
		if (data && bytes >= sizeof(GUID))
			*(GUID *)data = PIN_CATEGORY_CAPTURE;
		if (ret)
			*ret = sizeof(GUID);
		return S_OK;
	}
	IFACEMETHODIMP QuerySupported(REFGUID set, DWORD id,
				      DWORD *support) override
	{
		if (set != AMPROPSETID_Pin || id != AMPROPERTY_PIN_CATEGORY)
			return E_PROP_ID_UNSUPPORTED;
		if (support)
			*support = KSPROPERTY_SUPPORT_GET;
		return S_OK;
	}

	/* streaming control (called by the filter) */
	HRESULT Active();
	HRESULT Inactive();

private:
	static DWORD WINAPI thunk(void *p)
	{
		((CensrPin *)p)->StreamLoop();
		return 0;
	}
	void StreamLoop();

	CensrFilter *filter_ = nullptr; /* weak */
	ComPtr<IPin> peer_;
	ComPtr<IMemInputPin> input_;
	ComPtr<IMemAllocator> alloc_;
	censr::SharedFrameReader reader_;
	HANDLE thread_ = nullptr;
	volatile bool stop_ = false;
};

/* ---- IEnumPins over our single pin ---- */
class CensrEnumPins
	: public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumPins> {
public:
	void Init(IPin *pin, ULONG pos)
	{
		pin_ = pin;
		pos_ = pos;
	}
	IFACEMETHODIMP Next(ULONG c, IPin **out, ULONG *fetched) override
	{
		ULONG got = 0;
		while (got < c && pos_ < 1) {
			pin_.CopyTo(&out[got++]);
			++pos_;
		}
		if (fetched)
			*fetched = got;
		return got == c ? S_OK : S_FALSE;
	}
	IFACEMETHODIMP Skip(ULONG c) override
	{
		pos_ += c;
		return pos_ <= 1 ? S_OK : S_FALSE;
	}
	IFACEMETHODIMP Reset() override
	{
		pos_ = 0;
		return S_OK;
	}
	IFACEMETHODIMP Clone(IEnumPins **out) override
	{
		auto e = Make<CensrEnumPins>();
		e->Init(pin_.Get(), pos_);
		return e.CopyTo(out);
	}

private:
	ComPtr<IPin> pin_;
	ULONG pos_ = 0;
};

/* ---- filter ---- */
class CensrFilter : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
					IBaseFilter, IAMFilterMiscFlags> {
public:
	HRESULT RuntimeClassInitialize()
	{
		pin_ = Make<CensrPin>();
		if (!pin_)
			return E_OUTOFMEMORY;
		pin_->Init(this);
		return S_OK;
	}

	/* IPersist / IMediaFilter / IBaseFilter */
	IFACEMETHODIMP GetClassID(CLSID *id) override
	{
		*id = CLSID_CensrDShowSource;
		return S_OK;
	}
	IFACEMETHODIMP Stop() override
	{
		state_ = State_Stopped;
		pin_->Inactive();
		return S_OK;
	}
	IFACEMETHODIMP Pause() override
	{
		state_ = State_Paused;
		pin_->Active();
		return S_OK;
	}
	IFACEMETHODIMP Run(REFERENCE_TIME) override
	{
		state_ = State_Running;
		pin_->Active();
		return S_OK;
	}
	IFACEMETHODIMP GetState(DWORD, FILTER_STATE *s) override
	{
		*s = state_;
		return S_OK;
	}
	IFACEMETHODIMP SetSyncSource(IReferenceClock *c) override
	{
		clock_ = c;
		return S_OK;
	}
	IFACEMETHODIMP GetSyncSource(IReferenceClock **c) override
	{
		return clock_ ? clock_.CopyTo(c) : (*c = nullptr, S_OK);
	}
	IFACEMETHODIMP EnumPins(IEnumPins **e) override
	{
		auto en = Make<CensrEnumPins>();
		ComPtr<IPin> p;
		pin_.As(&p);
		en->Init(p.Get(), 0);
		return en.CopyTo(e);
	}
	IFACEMETHODIMP FindPin(LPCWSTR, IPin **p) override
	{
		return pin_.CopyTo(p);
	}
	IFACEMETHODIMP QueryFilterInfo(FILTER_INFO *info) override
	{
		ZeroMemory(info, sizeof(*info));
		wcscpy_s(info->achName, CENSR_DSHOW_FRIENDLY_NAME);
		info->pGraph = graph_.Get();
		if (graph_)
			graph_->AddRef();
		return S_OK;
	}
	IFACEMETHODIMP JoinFilterGraph(IFilterGraph *g, LPCWSTR) override
	{
		graph_ = g; /* weak: graph owns filter */
		return S_OK;
	}
	IFACEMETHODIMP QueryVendorInfo(LPWSTR *v) override
	{
		*v = (LPWSTR)CoTaskMemAlloc(sizeof(L"Censr"));
		memcpy(*v, L"Censr", sizeof(L"Censr"));
		return S_OK;
	}

	/* IAMFilterMiscFlags */
	IFACEMETHODIMP_(ULONG) GetMiscFlags() override
	{
		return AM_FILTER_MISC_FLAGS_IS_SOURCE;
	}

	CensrPin *pin() { return pin_.Get(); }

private:
	ComPtr<CensrPin> pin_;
	ComPtr<IReferenceClock> clock_;
	ComPtr<IFilterGraph> graph_; /* weak */
	FILTER_STATE state_ = State_Stopped;
};

/* ---- pin methods needing the filter type ---- */
IFACEMETHODIMP CensrPin::QueryPinInfo(PIN_INFO *info)
{
	ZeroMemory(info, sizeof(*info));
	info->dir = PINDIR_OUTPUT;
	wcscpy_s(info->achName, L"Capture");
	info->pFilter = filter_;
	if (filter_)
		filter_->AddRef();
	return S_OK;
}

IFACEMETHODIMP CensrPin::Connect(IPin *recv, const AM_MEDIA_TYPE *)
{
	if (peer_)
		return VFW_E_ALREADY_CONNECTED;

	VIDEOINFOHEADER vih;
	AM_MEDIA_TYPE mt;
	fill_media_type(&mt, &vih);
	HRESULT hr = recv->ReceiveConnection(this, &mt);
	if (FAILED(hr))
		return hr;

	hr = recv->QueryInterface(IID_PPV_ARGS(&input_));
	if (FAILED(hr))
		return hr;

	if (FAILED(input_->GetAllocator(&alloc_))) {
		hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr,
				      CLSCTX_INPROC_SERVER,
				      IID_PPV_ARGS(&alloc_));
		if (FAILED(hr))
			return hr;
	}
	ALLOCATOR_PROPERTIES req{}, act{};
	req.cBuffers = 2;
	req.cbBuffer = kImageSize;
	req.cbAlign = 1;
	alloc_->SetProperties(&req, &act);
	input_->NotifyAllocator(alloc_.Get(), FALSE);

	peer_ = recv;
	return S_OK;
}

IFACEMETHODIMP CensrPin::Disconnect()
{
	Inactive();
	peer_.Reset();
	input_.Reset();
	alloc_.Reset();
	return S_OK;
}

HRESULT CensrPin::Active()
{
	if (thread_ || !alloc_)
		return S_OK;
	alloc_->Commit();
	stop_ = false;
	thread_ = CreateThread(nullptr, 0, thunk, this, 0, nullptr);
	return S_OK;
}

HRESULT CensrPin::Inactive()
{
	stop_ = true;
	if (thread_) {
		WaitForSingleObject(thread_, 2000);
		CloseHandle(thread_);
		thread_ = nullptr;
	}
	if (alloc_)
		alloc_->Decommit();
	return S_OK;
}

void CensrPin::StreamLoop()
{
	REFERENCE_TIME t = 0;
	bool opened = reader_.open();
	std::vector<uint8_t> frame;
	while (!stop_) {
		ComPtr<IMediaSample> s;
		if (FAILED(alloc_->GetBuffer(&s, nullptr, nullptr, 0)))
			break;
		BYTE *data = nullptr;
		s->GetPointer(&data);
		uint32_t w = 0, h = 0;
		uint64_t seq = 0;
		if (!opened)
			opened = reader_.open();
		if (opened && reader_.read(frame, w, h, seq) && w && h)
			scale_flip(frame.data(), w, h, data);
		else
			memset(data, 32, kImageSize);
		s->SetActualDataLength(kImageSize);
		REFERENCE_TIME start = t, stop = t + kFrameDur;
		s->SetTime(&start, &stop);
		s->SetSyncPoint(TRUE);
		t += kFrameDur;
		if (input_)
			input_->Receive(s.Get());
		Sleep(1000 / CENSR_DSHOW_FPS);
	}
}

} // namespace

HRESULT CreateCensrDShowFilter(REFIID riid, void **ppv)
{
	ComPtr<CensrFilter> f = Make<CensrFilter>();
	if (!f)
		return E_OUTOFMEMORY;
	HRESULT hr = f->RuntimeClassInitialize();
	if (FAILED(hr))
		return hr;
	return f.CopyTo(riid, ppv);
}
