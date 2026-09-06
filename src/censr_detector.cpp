/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/


#include "censr_detector.h"
#include "secret_rules.h"
#include "qr_zxing.h"
#include "censr_log.h"

#ifdef _WIN32
#include <winrt/Windows.Foundation.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace censr {

namespace {

/* Expand a matched line box horizontally to the actual contiguous text run on
 * that row, read from the binarized image (text pixels are black, value < 128).
 * Bridges word gaps but stops at a large gap (e.g. a column break), so a secret
 * value that OCR dropped is still covered while a separate right-hand column is
 * left visible. Coordinates are detection-image pixels. */
RectF expand_to_text_extent(RectF box, const uint8_t *bin, int w, int h)
{
	const int GAP = 40; /* px of blank that ends a run (native-res scale) */
	const int y0 = std::max(0, (int)box.y);
	const int y1 = std::min(h, (int)(box.y + box.h));
	auto band_has_text = [&](int x) -> bool {
		if (x < 0 || x >= w)
			return false;
		for (int y = y0; y < y1; ++y)
			if (bin[((size_t)y * w + x) * 4] < 128)
				return true;
		return false;
	};

	int left = std::max(0, (int)box.x);
	int right = std::min(w, (int)(box.x + box.w));
	int gap = 0;
	for (int x = right; x < w; ++x) {
		if (band_has_text(x)) {
			right = x + 1;
			gap = 0;
		} else if (++gap > GAP) {
			break;
		}
	}
	gap = 0;
	for (int x = left - 1; x >= 0; --x) {
		if (band_has_text(x)) {
			left = x;
			gap = 0;
		} else if (++gap > GAP) {
			break;
		}
	}
	RectF out = box;
	out.x = (float)left;
	out.w = (float)(right - left);
	return out;
}

} // namespace

Detector::Detector(CoverCallback cb, std::string det_model, std::string rec_model,
		   Engine engine)
	: cb_(std::move(cb)), det_model_(std::move(det_model)),
	  rec_model_(std::move(rec_model)), engine_(engine)
{
	thread_ = std::thread(&Detector::run, this);
}

Detector::~Detector()
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		stop_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void Detector::submit(std::vector<uint8_t> rgba, int w, int h, int src_w, int src_h,
		      uint64_t ts)
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		pending_.rgba = std::move(rgba);
		pending_.w = w;
		pending_.h = h;
		pending_.src_w = src_w;
		pending_.src_h = src_h;
		pending_.ts = ts;
		has_pending_ = true;
	}
	cv_.notify_one();
}

void Detector::run()
{
#ifdef _WIN32
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	Engine eng = engine_;
	const char *sel = getenv("CENSR_OCR"); /* dev override */
	if (sel && strcmp(sel, "onnx") == 0)
		eng = Engine::ONNX;
	else if (sel && strcmp(sel, "winrt") == 0)
		eng = Engine::WINRT;

	bool want_onnx = !det_model_.empty() && !rec_model_.empty() &&
			 eng != Engine::WINRT;
	/* User explicitly asked for GPU but the models aren't available. */
	if (eng == Engine::ONNX && !want_onnx)
		censr_log(LOG_WARNING,
			  "Censr detector: GPU OCR requested but models not found; "
			  "falling back to Windows.Media.Ocr");
	if (want_onnx) {
		ocr_ = create_onnx_ocr(det_model_, rec_model_);
		if (ocr_)
			censr_log(LOG_INFO, "Censr detector: using GPU OCR (ONNX/DirectML)");
		else if (eng == Engine::ONNX)
			censr_log(LOG_WARNING,
				  "Censr detector: GPU OCR init failed; "
				  "falling back to Windows.Media.Ocr");
	}
	if (!ocr_) {
		ocr_ = create_winrt_ocr();
		if (ocr_)
			censr_log(LOG_INFO, "Censr detector: using Windows.Media.Ocr");
	}
#else
	ocr_ = create_vision_ocr();
#endif
	if (!ocr_)
		censr_log(LOG_WARNING, "Censr detector: OCR unavailable, text redaction disabled");

	std::vector<uint8_t> bgra;
	std::vector<uint8_t> ocr_bgra;

	while (true) {
		Frame frame;
		{
			std::unique_lock<std::mutex> lk(mutex_);
			cv_.wait(lk, [&] { return has_pending_ || stop_; });
			if (stop_)
				break;
			frame = std::move(pending_);
			has_pending_ = false;
		}

		const int w = frame.w, h = frame.h;
		if (w <= 0 || h <= 0 || frame.rgba.size() < (size_t)w * h * 4)
			continue;

		/* RGBA (readback) -> BGRA (OCR/ZXing expect BGRA). */
		bgra.resize((size_t)w * h * 4);
		for (size_t i = 0; i < (size_t)w * h; ++i) {
			bgra[i * 4 + 0] = frame.rgba[i * 4 + 2];
			bgra[i * 4 + 1] = frame.rgba[i * 4 + 1];
			bgra[i * 4 + 2] = frame.rgba[i * 4 + 0];
			bgra[i * 4 + 3] = frame.rgba[i * 4 + 3];
		}

		/* Binarize a copy for OCR: crisp black-on-white raises OCR
		 * confidence on dense/random secret tokens that it otherwise
		 * skips. Bright pixels (text on a dark UI) -> black, else white.
		 * Kept separate so QR detection can use the original BGRA. */
		ocr_bgra.resize((size_t)w * h * 4);
		for (size_t i = 0; i < (size_t)w * h; ++i) {
			int b = bgra[i * 4 + 0], g = bgra[i * 4 + 1],
			    r = bgra[i * 4 + 2];
			int gray = (r * 299 + g * 587 + b * 114) / 1000;
			uint8_t v = (gray >= 110) ? 0 : 255;
			ocr_bgra[i * 4 + 0] = v;
			ocr_bgra[i * 4 + 1] = v;
			ocr_bgra[i * 4 + 2] = v;
			ocr_bgra[i * 4 + 3] = 255;
		}

		const uint32_t mask = rule_mask_.load();

		std::vector<RectF> det_boxes; /* detection-image space */
		if (ocr_ && (mask & (CENSR_RULE_APIKEYS | CENSR_RULE_LABELS |
				     CENSR_RULE_EMAILS))) {
			/* Pass the ORIGINAL image; the WinRT engine binarizes
			 * internally, the ONNX engine wants the natural image.
			 * ocr_bgra (binarized) is still used for extent growth. */
			bool pw = per_word_.load();
			auto lines = ocr_->recognize(bgra.data(), w, h, w * 4);
			auto secret_boxes = find_secret_boxes(lines, mask, pw);
			/* Line mode: grow each match to the full text run on its row
			 * so values OCR dropped are still covered. Per-word mode
			 * keeps the precise token box (no expansion). */
			if (!pw)
				for (auto &b : secret_boxes)
					b = expand_to_text_extent(b, ocr_bgra.data(),
								  w, h);
			det_boxes.insert(det_boxes.end(), secret_boxes.begin(),
					 secret_boxes.end());
		}

		/* QR / barcodes (ZXing) on the original (non-binarized) image. */
		if (mask & CENSR_RULE_QR) {
			auto qr_boxes = detect_qr_boxes(bgra.data(), w, h);
			det_boxes.insert(det_boxes.end(), qr_boxes.begin(),
					 qr_boxes.end());
		}

		/* Map to source pixel space. */
		const float sx = (float)frame.src_w / (float)w;
		const float sy = (float)frame.src_h / (float)h;
		std::vector<RectF> src_boxes;
		src_boxes.reserve(det_boxes.size());
		for (const auto &b : det_boxes)
			src_boxes.push_back({b.x * sx, b.y * sy, b.w * sx, b.h * sy});

		if (cb_)
			cb_(frame.ts, src_boxes);
	}

	ocr_.reset();
#ifdef _WIN32
	winrt::uninit_apartment();
#endif
}

} // namespace censr
