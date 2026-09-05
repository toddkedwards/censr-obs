/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/* Windows.Media.Ocr implementation of ICensrOcr via C++/WinRT. */

#include <util/base.h>

#include "ocr_engine.h"
#include "plugin-support.h"

#include <algorithm>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

namespace censr {

namespace {

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;
using winrt::Windows::Storage::Streams::IBuffer;

class WinRtOcr : public ICensrOcr {
public:
	explicit WinRtOcr(OcrEngine engine) : engine_(std::move(engine)) {}

	std::vector<OcrLine> recognize(const uint8_t *bgra, int w, int h,
				       int stride) override
	{
		std::vector<OcrLine> out;
		if (!engine_ || w <= 0 || h <= 0)
			return out;

		try {
			/* Build a tightly-packed BGRA8 IBuffer, then a bitmap. */
			IBuffer buffer = CryptographicBuffer::CreateFromByteArray(
				winrt::array_view<uint8_t const>(
					bgra, bgra + (size_t)stride * h));
			SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(
				buffer, BitmapPixelFormat::Bgra8, w, h);

			OcrResult result = engine_.RecognizeAsync(bitmap).get();
			for (auto const &line : result.Lines()) {
				OcrLine ol;
				ol.text = to_string(line.Text());
				float minx = 1e9f, miny = 1e9f, maxx = -1e9f,
				      maxy = -1e9f;
				for (auto const &word : line.Words()) {
					auto r = word.BoundingRect();
					OcrWord ow;
					ow.text = to_string(word.Text());
					ow.box = {r.X, r.Y, r.Width, r.Height};
					ol.words.push_back(std::move(ow));
					minx = std::min(minx, r.X);
					miny = std::min(miny, r.Y);
					maxx = std::max(maxx, r.X + r.Width);
					maxy = std::max(maxy, r.Y + r.Height);
				}
				if (!ol.words.empty())
					ol.box = {minx, miny, maxx - minx,
						  maxy - miny};
				out.push_back(std::move(ol));
			}
		} catch (hresult_error const &e) {
			obs_log(LOG_WARNING, "Censr OCR error: 0x%08x %s",
				(unsigned)e.code(), to_string(e.message()).c_str());
		}
		return out;
	}

private:
	OcrEngine engine_{nullptr};
};

} // namespace

std::unique_ptr<ICensrOcr> create_winrt_ocr()
{
	try {
		OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
		if (!engine) {
			winrt::Windows::Globalization::Language en(L"en-US");
			if (OcrEngine::IsLanguageSupported(en))
				engine = OcrEngine::TryCreateFromLanguage(en);
		}
		if (!engine) {
			obs_log(LOG_WARNING,
				"Censr: no Windows OCR engine for installed languages");
			return nullptr;
		}
		obs_log(LOG_INFO, "Censr: Windows.Media.Ocr engine ready");
		return std::make_unique<WinRtOcr>(std::move(engine));
	} catch (winrt::hresult_error const &e) {
		obs_log(LOG_WARNING, "Censr: OCR init failed: 0x%08x",
			(unsigned)e.code());
		return nullptr;
	}
}

} // namespace censr
