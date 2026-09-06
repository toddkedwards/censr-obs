/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

The Censr media source + stream (Media Foundation). MFCreateVirtualCamera backs
the "Censr Virtual Camera" with this in-proc COM object; the frame server pulls
RGB32 frames, which we fill from the host's shared redacted-frame stream (see
shared_frame.h). Fixed 1920x1080@30 RGB32 (== BGRA in memory, so the host frames
copy in directly, scaled if needed).

NOTE: this compiles and links against the real MF virtual-camera API, and reads
the live frame stream, but it has not yet been validated inside a real camera app
(that needs COM registration, code signing, and a Win11 capture app). Attribute
tuning may be required during that validation. See docs/roadmap-standalone-vcam.md.
*/

#include "media_source.h"
#include "censr_vcam_guid.h"
#include "shared_frame.h"

#include <windows.h>

#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <vector>

using namespace Microsoft::WRL;

namespace {

constexpr LONGLONG kFrameDuration = 10000000LL / CENSR_VCAM_FPS; /* 100ns units */

HRESULT create_video_type(IMFMediaType **out)
{
	ComPtr<IMFMediaType> mt;
	HRESULT hr = MFCreateMediaType(&mt);
	if (FAILED(hr))
		return hr;
	mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
	MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, CENSR_VCAM_WIDTH,
			   CENSR_VCAM_HEIGHT);
	MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, CENSR_VCAM_FPS, 1);
	MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	mt->SetUINT32(MF_MT_DEFAULT_STRIDE, CENSR_VCAM_WIDTH * 4); /* top-down */
	mt->SetUINT32(MF_MT_SAMPLE_SIZE, CENSR_VCAM_WIDTH * CENSR_VCAM_HEIGHT * 4);
	*out = mt.Detach();
	return S_OK;
}

/* Nearest-neighbour scale of a BGRA src (sw x sh) into a BGRA dst
 * (CENSR_VCAM_WIDTH x CENSR_VCAM_HEIGHT), top-down. */
void scale_bgra(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst)
{
	const uint32_t dw = CENSR_VCAM_WIDTH, dh = CENSR_VCAM_HEIGHT;
	for (uint32_t y = 0; y < dh; ++y) {
		uint32_t sy = sh ? (y * sh / dh) : 0;
		const uint8_t *srow = src + (size_t)sy * sw * 4;
		uint8_t *drow = dst + (size_t)y * dw * 4;
		for (uint32_t x = 0; x < dw; ++x) {
			uint32_t sx = sw ? (x * sw / dw) : 0;
			memcpy(drow + x * 4, srow + sx * 4, 4);
		}
	}
}

class CensrStream;

/* ------------------------------------------------------------------ source -- */
class CensrSource
	: public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaSourceEx> {
public:
	HRESULT RuntimeClassInitialize();

	/* IMFMediaEventGenerator */
	IFACEMETHODIMP GetEvent(DWORD f, IMFMediaEvent **e) override;
	IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *s) override;
	IFACEMETHODIMP EndGetEvent(IMFAsyncResult *r, IMFMediaEvent **e) override;
	IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT s,
				  const PROPVARIANT *v) override;
	/* IMFMediaSource */
	IFACEMETHODIMP GetCharacteristics(DWORD *c) override;
	IFACEMETHODIMP CreatePresentationDescriptor(
		IMFPresentationDescriptor **pd) override;
	IFACEMETHODIMP Start(IMFPresentationDescriptor *pd, const GUID *time,
			     const PROPVARIANT *start) override;
	IFACEMETHODIMP Stop() override;
	IFACEMETHODIMP Pause() override;
	IFACEMETHODIMP Shutdown() override;
	/* IMFMediaSourceEx */
	IFACEMETHODIMP GetSourceAttributes(IMFAttributes **a) override;
	IFACEMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes **a) override;
	IFACEMETHODIMP SetD3DManager(IUnknown *m) override;

private:
	ComPtr<IMFMediaEventQueue> queue_;
	ComPtr<IMFPresentationDescriptor> pd_;
	ComPtr<CensrStream> stream_; /* concrete type; also an IMFMediaStream */
	ComPtr<IMFAttributes> attributes_;
	bool shutdown_ = false;
	CRITICAL_SECTION lock_;

	HRESULT check() { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }
public:
	~CensrSource() { DeleteCriticalSection(&lock_); }
};

/* ------------------------------------------------------------------ stream -- */
class CensrStream : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
					IMFMediaStream> {
public:
	HRESULT Init(IMFMediaSource *source, IMFStreamDescriptor *sd);

	/* IMFMediaEventGenerator */
	IFACEMETHODIMP GetEvent(DWORD f, IMFMediaEvent **e) override;
	IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *s) override;
	IFACEMETHODIMP EndGetEvent(IMFAsyncResult *r, IMFMediaEvent **e) override;
	IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT s,
				  const PROPVARIANT *v) override;
	/* IMFMediaStream */
	IFACEMETHODIMP GetMediaSource(IMFMediaSource **s) override;
	IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor **sd) override;
	IFACEMETHODIMP RequestSample(IUnknown *token) override;

	HRESULT QueueStreamStarted(const PROPVARIANT *start);
	void Shutdown();

private:
	HRESULT DeliverSample(IUnknown *token);

	ComPtr<IMFMediaEventQueue> queue_;
	ComPtr<IMFMediaSource> source_; /* strong; broken on Shutdown */
	ComPtr<IMFStreamDescriptor> sd_;
	censr::SharedFrameReader reader_;
	LONGLONG time_ = 0;
	bool opened_ = false;
	bool shutdown_ = false;
};

/* ---- stream impl ---- */
HRESULT CensrStream::Init(IMFMediaSource *source, IMFStreamDescriptor *sd)
{
	source_ = source;
	sd_ = sd;
	return MFCreateEventQueue(&queue_);
}

IFACEMETHODIMP CensrStream::GetEvent(DWORD f, IMFMediaEvent **e)
{
	return queue_->GetEvent(f, e);
}
IFACEMETHODIMP CensrStream::BeginGetEvent(IMFAsyncCallback *cb, IUnknown *s)
{
	return queue_->BeginGetEvent(cb, s);
}
IFACEMETHODIMP CensrStream::EndGetEvent(IMFAsyncResult *r, IMFMediaEvent **e)
{
	return queue_->EndGetEvent(r, e);
}
IFACEMETHODIMP CensrStream::QueueEvent(MediaEventType t, REFGUID ext, HRESULT s,
				       const PROPVARIANT *v)
{
	return queue_->QueueEventParamVar(t, ext, s, v);
}
IFACEMETHODIMP CensrStream::GetMediaSource(IMFMediaSource **s)
{
	if (shutdown_)
		return MF_E_SHUTDOWN;
	return source_.CopyTo(s);
}
IFACEMETHODIMP CensrStream::GetStreamDescriptor(IMFStreamDescriptor **sd)
{
	if (shutdown_)
		return MF_E_SHUTDOWN;
	return sd_.CopyTo(sd);
}

HRESULT CensrStream::QueueStreamStarted(const PROPVARIANT *start)
{
	time_ = 0;
	return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, start);
}

IFACEMETHODIMP CensrStream::RequestSample(IUnknown *token)
{
	if (shutdown_)
		return MF_E_SHUTDOWN;
	return DeliverSample(token);
}

HRESULT CensrStream::DeliverSample(IUnknown *token)
{
	const DWORD size = CENSR_VCAM_WIDTH * CENSR_VCAM_HEIGHT * 4;
	ComPtr<IMFMediaBuffer> buf;
	HRESULT hr = MFCreateMemoryBuffer(size, &buf);
	if (FAILED(hr))
		return hr;

	BYTE *dst = nullptr;
	hr = buf->Lock(&dst, nullptr, nullptr);
	if (FAILED(hr))
		return hr;

	if (!opened_)
		opened_ = reader_.open();
	std::vector<uint8_t> frame;
	uint32_t w = 0, h = 0;
	uint64_t seq = 0;
	if (opened_ && reader_.read(frame, w, h, seq) && w && h)
		scale_bgra(frame.data(), w, h, dst);
	else
		memset(dst, 32, size); /* dark grey until the host is up */
	buf->Unlock();
	buf->SetCurrentLength(size);

	ComPtr<IMFSample> sample;
	hr = MFCreateSample(&sample);
	if (FAILED(hr))
		return hr;
	sample->AddBuffer(buf.Get());
	sample->SetSampleTime(time_);
	sample->SetSampleDuration(kFrameDuration);
	time_ += kFrameDuration;
	if (token)
		sample->SetUnknown(MFSampleExtension_Token, token);

	return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK,
					  sample.Get());
}

void CensrStream::Shutdown()
{
	shutdown_ = true;
	if (queue_)
		queue_->Shutdown();
	source_.Reset();
}

/* ---- source impl ---- */
HRESULT CensrSource::RuntimeClassInitialize()
{
	InitializeCriticalSection(&lock_);
	HRESULT hr = MFCreateEventQueue(&queue_);
	if (FAILED(hr))
		return hr;
	hr = MFCreateAttributes(&attributes_, 1);
	if (FAILED(hr))
		return hr;

	ComPtr<IMFMediaType> mt;
	hr = create_video_type(&mt);
	if (FAILED(hr))
		return hr;
	ComPtr<IMFStreamDescriptor> sd;
	hr = MFCreateStreamDescriptor(0, 1, mt.GetAddressOf(), &sd);
	if (FAILED(hr))
		return hr;
	/* Advertise this as a video-capture stream. */
	sd->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
	sd->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
	{
		ComPtr<IMFMediaTypeHandler> h;
		sd->GetMediaTypeHandler(&h);
		h->SetCurrentMediaType(mt.Get());
	}

	stream_ = Make<CensrStream>();
	if (!stream_)
		return E_OUTOFMEMORY;
	hr = stream_->Init(this, sd.Get());
	if (FAILED(hr))
		return hr;

	IMFStreamDescriptor *sds[] = {sd.Get()};
	hr = MFCreatePresentationDescriptor(1, sds, &pd_);
	if (FAILED(hr))
		return hr;
	pd_->SelectStream(0);
	return S_OK;
}

IFACEMETHODIMP CensrSource::GetEvent(DWORD f, IMFMediaEvent **e)
{
	return queue_->GetEvent(f, e);
}
IFACEMETHODIMP CensrSource::BeginGetEvent(IMFAsyncCallback *cb, IUnknown *s)
{
	return queue_->BeginGetEvent(cb, s);
}
IFACEMETHODIMP CensrSource::EndGetEvent(IMFAsyncResult *r, IMFMediaEvent **e)
{
	return queue_->EndGetEvent(r, e);
}
IFACEMETHODIMP CensrSource::QueueEvent(MediaEventType t, REFGUID ext, HRESULT s,
				       const PROPVARIANT *v)
{
	return queue_->QueueEventParamVar(t, ext, s, v);
}

IFACEMETHODIMP CensrSource::GetCharacteristics(DWORD *c)
{
	if (!c)
		return E_POINTER;
	*c = MFMEDIASOURCE_IS_LIVE;
	return S_OK;
}

IFACEMETHODIMP CensrSource::CreatePresentationDescriptor(
	IMFPresentationDescriptor **pd)
{
	EnterCriticalSection(&lock_);
	HRESULT hr = check();
	if (SUCCEEDED(hr))
		hr = pd_->Clone(pd);
	LeaveCriticalSection(&lock_);
	return hr;
}

IFACEMETHODIMP CensrSource::Start(IMFPresentationDescriptor *pd, const GUID *,
				  const PROPVARIANT *start)
{
	EnterCriticalSection(&lock_);
	HRESULT hr = check();
	if (SUCCEEDED(hr)) {
		/* Announce the stream, then started state. */
		PROPVARIANT var;
		if (start)
			PropVariantCopy(&var, start);
		else
			PropVariantInit(&var);

		ComPtr<IMFMediaStream> s;
		stream_.As(&s);
		queue_->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK, s.Get());
		stream_->QueueStreamStarted(&var);
		queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &var);
		PropVariantClear(&var);
	}
	LeaveCriticalSection(&lock_);
	return hr;
}

IFACEMETHODIMP CensrSource::Stop()
{
	EnterCriticalSection(&lock_);
	HRESULT hr = check();
	if (SUCCEEDED(hr))
		hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK,
						nullptr);
	LeaveCriticalSection(&lock_);
	return hr;
}

IFACEMETHODIMP CensrSource::Pause()
{
	return MF_E_INVALID_STATE_TRANSITION;
}

IFACEMETHODIMP CensrSource::Shutdown()
{
	EnterCriticalSection(&lock_);
	shutdown_ = true;
	if (stream_)
		stream_->Shutdown();
	if (queue_)
		queue_->Shutdown();
	LeaveCriticalSection(&lock_);
	return S_OK;
}

IFACEMETHODIMP CensrSource::GetSourceAttributes(IMFAttributes **a)
{
	if (!a)
		return E_POINTER;
	return attributes_.CopyTo(a);
}
IFACEMETHODIMP CensrSource::GetStreamAttributes(DWORD, IMFAttributes **a)
{
	if (!a)
		return E_POINTER;
	return attributes_.CopyTo(a);
}
IFACEMETHODIMP CensrSource::SetD3DManager(IUnknown *)
{
	return S_OK;
}

} // namespace

HRESULT CreateCensrMediaSource(REFIID riid, void **ppv)
{
	ComPtr<CensrSource> src = Make<CensrSource>();
	if (!src)
		return E_OUTOFMEMORY;
	HRESULT hr = src->RuntimeClassInitialize();
	if (FAILED(hr))
		return hr;
	return src.CopyTo(riid, ppv);
}
