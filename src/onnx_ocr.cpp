/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/*
 * GPU OCR via ONNX Runtime (DirectML EP) running PP-OCRv4:
 *   - DBNet text detection -> probability map -> threshold -> connected
 *     components -> boxes (simple postproc, no OpenCV/pyclipper).
 *   - CRNN recognition per box -> CTC greedy decode using the char dictionary
 *     embedded in the rec model metadata.
 * Pre/post-processing mirrors RapidOCR's PP-OCRv4 pipeline (BGR input,
 * (px/255-0.5)/0.5 normalization).
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <util/base.h>

#include "ocr_engine.h"
#include "plugin-support.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>
#ifndef CENSR_ORT_GPU
#include <dml_provider_factory.h>
#endif

namespace censr {

namespace {

std::wstring to_wstring(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
}

/* Python str.splitlines() equivalent. */
std::vector<std::string> splitlines(const std::string &s)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : s) {
		if (c == '\n') {
			if (!cur.empty() && cur.back() == '\r')
				cur.pop_back();
			out.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) {
		if (cur.back() == '\r')
			cur.pop_back();
		out.push_back(cur);
	}
	return out;
}

int round32(int v)
{
	int r = (int)std::lround(v / 32.0) * 32;
	return r < 32 ? 32 : r;
}

/* Bilinear resize of a tightly-packed BGR image. */
void resize_bgr(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
	for (int y = 0; y < dh; ++y) {
		float fy = (y + 0.5f) * sh / dh - 0.5f;
		int y0 = (int)std::floor(fy);
		float wy = fy - y0;
		int y0c = std::clamp(y0, 0, sh - 1);
		int y1c = std::clamp(y0 + 1, 0, sh - 1);
		for (int x = 0; x < dw; ++x) {
			float fx = (x + 0.5f) * sw / dw - 0.5f;
			int x0 = (int)std::floor(fx);
			float wx = fx - x0;
			int x0c = std::clamp(x0, 0, sw - 1);
			int x1c = std::clamp(x0 + 1, 0, sw - 1);
			for (int c = 0; c < 3; ++c) {
				float a = src[(y0c * sw + x0c) * 3 + c];
				float b = src[(y0c * sw + x1c) * 3 + c];
				float cc = src[(y1c * sw + x0c) * 3 + c];
				float d = src[(y1c * sw + x1c) * 3 + c];
				float top = a + (b - a) * wx;
				float bot = cc + (d - cc) * wx;
				dst[(y * dw + x) * 3 + c] =
					(uint8_t)std::clamp(top + (bot - top) * wy,
							    0.f, 255.f);
			}
		}
	}
}

/* Configure the GPU execution provider. Default build uses DirectML (runs on any
 * DX12 GPU, no CUDA install). A -DCENSR_ORT_GPU=ON build links the CUDA/TensorRT
 * ONNX Runtime and picks TensorRT -> CUDA -> CPU, honoring CENSR_ORT_EP. */
void append_ep(Ort::SessionOptions &so)
{
#ifdef CENSR_ORT_GPU
	const char *sel = getenv("CENSR_ORT_EP");
	std::string pref = sel ? sel : "tensorrt";
	if (pref == "tensorrt") {
		try {
			OrtTensorRTProviderOptions o{};
			o.device_id = 0;
			so.AppendExecutionProvider_TensorRT(o);
			obs_log(LOG_INFO, "Censr: ONNX execution provider = TensorRT");
			return;
		} catch (const Ort::Exception &e) {
			obs_log(LOG_WARNING,
				"Censr: TensorRT EP unavailable (%s); trying CUDA",
				e.what());
			pref = "cuda";
		}
	}
	if (pref == "cuda") {
		try {
			OrtCUDAProviderOptions o{};
			o.device_id = 0;
			so.AppendExecutionProvider_CUDA(o);
			obs_log(LOG_INFO, "Censr: ONNX execution provider = CUDA");
			return;
		} catch (const Ort::Exception &e) {
			obs_log(LOG_WARNING,
				"Censr: CUDA EP unavailable (%s); using CPU", e.what());
		}
	}
	obs_log(LOG_INFO, "Censr: ONNX execution provider = CPU");
#else
	so.SetExecutionMode(ORT_SEQUENTIAL);
	so.DisableMemPattern();
	Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so, 0));
	obs_log(LOG_INFO, "Censr: ONNX execution provider = DirectML");
#endif
}

class OnnxOcr : public ICensrOcr {
public:
	OnnxOcr(const std::string &det_model, const std::string &rec_model)
		: env_(ORT_LOGGING_LEVEL_WARNING, "censr")
	{
		Ort::SessionOptions so;
		append_ep(so);

		det_ = Ort::Session(env_, to_wstring(det_model).c_str(), so);
		rec_ = Ort::Session(env_, to_wstring(rec_model).c_str(), so);

		Ort::AllocatorWithDefaultOptions alloc;
		det_in_ = det_.GetInputNameAllocated(0, alloc).get();
		det_out_ = det_.GetOutputNameAllocated(0, alloc).get();
		rec_in_ = rec_.GetInputNameAllocated(0, alloc).get();
		rec_out_ = rec_.GetOutputNameAllocated(0, alloc).get();

		/* Character list from rec model metadata: [blank] + dict + [space]. */
		auto meta = rec_.GetModelMetadata();
		Ort::AllocatorWithDefaultOptions a2;
		auto chars = meta.LookupCustomMetadataMapAllocated("character", a2);
		chars_.push_back(""); /* index 0 = CTC blank */
		if (chars) {
			for (auto &line : splitlines(chars.get()))
				chars_.push_back(line);
		}
		chars_.push_back(" "); /* trailing space class */

		obs_log(LOG_INFO,
			"Censr: ONNX PP-OCRv4 (DirectML) ready, %zu character classes",
			chars_.size());
	}

	std::vector<OcrLine> recognize(const uint8_t *bgra, int w, int h,
				       int stride) override
	{
		std::vector<OcrLine> lines;
		if (w <= 0 || h <= 0)
			return lines;

		/* BGRA -> tightly-packed BGR. */
		std::vector<uint8_t> bgr((size_t)w * h * 3);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) {
				const uint8_t *s = bgra + (size_t)y * stride + x * 4;
				uint8_t *d = bgr.data() + ((size_t)y * w + x) * 3;
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
			}

		std::vector<RectF> boxes;
		try {
			boxes = detect(bgr.data(), w, h);
		} catch (const Ort::Exception &e) {
			obs_log(LOG_WARNING, "Censr ONNX det error: %s", e.what());
			return lines;
		}

		for (const auto &b : boxes) {
			std::string text;
			try {
				text = recognize_crop(bgr.data(), w, h, b);
			} catch (const Ort::Exception &e) {
				obs_log(LOG_WARNING, "Censr ONNX rec error: %s",
					e.what());
				continue;
			}
			if (text.empty())
				continue;
			OcrLine ol;
			ol.text = text;
			ol.box = b;
			OcrWord ow;
			ow.text = text;
			ow.box = b;
			ol.words.push_back(std::move(ow));
			lines.push_back(std::move(ol));
		}
		return lines;
	}

private:
	/* --- detection --- */
	std::vector<RectF> detect(const uint8_t *bgr, int w, int h)
	{
		/* Resize: limit_type "min", limit_side_len 736, to a /32 multiple. */
		float ratio = 1.0f;
		int mn = std::min(w, h);
		if (mn < 736)
			ratio = 736.0f / mn;
		int rw = round32((int)(w * ratio));
		int rh = round32((int)(h * ratio));

		std::vector<uint8_t> rz((size_t)rw * rh * 3);
		resize_bgr(bgr, w, h, rz.data(), rw, rh);

		/* NCHW float, (px/255-0.5)/0.5, BGR order preserved. */
		std::vector<float> in((size_t)3 * rh * rw);
		for (int c = 0; c < 3; ++c)
			for (int y = 0; y < rh; ++y)
				for (int x = 0; x < rw; ++x) {
					float v = rz[((size_t)y * rw + x) * 3 + c] / 255.f;
					in[((size_t)c * rh + y) * rw + x] =
						(v - 0.5f) / 0.5f;
				}

		std::array<int64_t, 4> shape{1, 3, rh, rw};
		auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
						      OrtMemTypeDefault);
		Ort::Value tin = Ort::Value::CreateTensor<float>(
			mem, in.data(), in.size(), shape.data(), shape.size());
		const char *inames[] = {det_in_.c_str()};
		const char *onames[] = {det_out_.c_str()};
		auto outs = det_.Run(Ort::RunOptions{nullptr}, inames, &tin, 1,
				     onames, 1);
		const float *prob = outs[0].GetTensorData<float>();
		/* prob shape [1,1,rh,rw]. */

		return boxes_from_prob(prob, rw, rh, w, h);
	}

	std::vector<RectF> boxes_from_prob(const float *prob, int mw, int mh,
					   int ow, int oh)
	{
		const float thresh = 0.3f;
		const float box_thresh = 0.5f;
		std::vector<uint8_t> mask((size_t)mw * mh);
		for (size_t i = 0; i < mask.size(); ++i)
			mask[i] = prob[i] > thresh ? 1 : 0;

		std::vector<RectF> boxes;
		std::vector<uint8_t> seen((size_t)mw * mh, 0);
		std::queue<int> q;
		for (int start = 0; start < mw * mh; ++start) {
			if (!mask[start] || seen[start])
				continue;
			int x0 = mw, y0 = mh, x1 = 0, y1 = 0;
			double psum = 0;
			int cnt = 0;
			seen[start] = 1;
			q.push(start);
			while (!q.empty()) {
				int p = q.front();
				q.pop();
				int px = p % mw, py = p / mw;
				x0 = std::min(x0, px);
				y0 = std::min(y0, py);
				x1 = std::max(x1, px);
				y1 = std::max(y1, py);
				psum += prob[p];
				cnt++;
				const int dx[] = {1, -1, 0, 0};
				const int dy[] = {0, 0, 1, -1};
				for (int k = 0; k < 4; ++k) {
					int nx = px + dx[k], ny = py + dy[k];
					if (nx < 0 || ny < 0 || nx >= mw || ny >= mh)
						continue;
					int np = ny * mw + nx;
					if (mask[np] && !seen[np]) {
						seen[np] = 1;
						q.push(np);
					}
				}
			}
			int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
			if (std::min(bw, bh) < 3)
				continue;
			if (psum / cnt < box_thresh)
				continue;

			/* Approximate DB unclip (ratio 1.6): expand outward by
			 * area*ratio/perimeter. */
			double area = (double)bw * bh;
			double peri = 2.0 * (bw + bh);
			double dist = area * 1.6 / peri;
			float fx0 = (float)(x0 - dist), fy0 = (float)(y0 - dist);
			float fx1 = (float)(x1 + dist), fy1 = (float)(y1 + dist);

			/* Scale to original image coordinates. */
			float sx = (float)ow / mw, sy = (float)oh / mh;
			RectF r;
			r.x = std::max(0.f, fx0 * sx);
			r.y = std::max(0.f, fy0 * sy);
			r.w = std::min((float)ow, fx1 * sx) - r.x;
			r.h = std::min((float)oh, fy1 * sy) - r.y;
			if (r.w > 2 && r.h > 2)
				boxes.push_back(r);
		}
		return boxes;
	}

	/* --- recognition --- */
	std::string recognize_crop(const uint8_t *bgr, int w, int h, const RectF &b)
	{
		int cx0 = std::clamp((int)b.x, 0, w - 1);
		int cy0 = std::clamp((int)b.y, 0, h - 1);
		int cx1 = std::clamp((int)(b.x + b.w), cx0 + 1, w);
		int cy1 = std::clamp((int)(b.y + b.h), cy0 + 1, h);
		int cw = cx1 - cx0, ch = cy1 - cy0;
		if (cw < 2 || ch < 2)
			return "";

		std::vector<uint8_t> crop((size_t)cw * ch * 3);
		for (int y = 0; y < ch; ++y)
			memcpy(crop.data() + (size_t)y * cw * 3,
			       bgr + ((size_t)(cy0 + y) * w + cx0) * 3,
			       (size_t)cw * 3);

		const int H = 48;
		int Wt = (int)std::ceil(H * (float)cw / ch);
		if (Wt < 1)
			Wt = 1;
		int modelW = std::max(320, Wt);

		std::vector<uint8_t> rz((size_t)Wt * H * 3);
		resize_bgr(crop.data(), cw, ch, rz.data(), Wt, H);

		std::vector<float> in((size_t)3 * H * modelW, 0.f);
		for (int c = 0; c < 3; ++c)
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < Wt; ++x) {
					float v = rz[((size_t)y * Wt + x) * 3 + c] /
						  255.f;
					in[((size_t)c * H + y) * modelW + x] =
						(v - 0.5f) / 0.5f;
				}

		std::array<int64_t, 4> shape{1, 3, H, modelW};
		auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
						      OrtMemTypeDefault);
		Ort::Value tin = Ort::Value::CreateTensor<float>(
			mem, in.data(), in.size(), shape.data(), shape.size());
		const char *inames[] = {rec_in_.c_str()};
		const char *onames[] = {rec_out_.c_str()};
		auto outs = rec_.Run(Ort::RunOptions{nullptr}, inames, &tin, 1,
				     onames, 1);
		auto info = outs[0].GetTensorTypeAndShapeInfo();
		auto oshape = info.GetShape(); /* [1, T, C] */
		int T = (int)oshape[1];
		int C = (int)oshape[2];
		const float *pd = outs[0].GetTensorData<float>();

		std::string text;
		int prev = -1;
		for (int t = 0; t < T; ++t) {
			const float *row = pd + (size_t)t * C;
			int best = 0;
			float bestv = row[0];
			for (int c = 1; c < C; ++c)
				if (row[c] > bestv) {
					bestv = row[c];
					best = c;
				}
			if (best != 0 && best != prev && best < (int)chars_.size())
				text += chars_[best];
			prev = best;
		}
		return text;
	}

	Ort::Env env_;
	Ort::Session det_{nullptr};
	Ort::Session rec_{nullptr};
	std::string det_in_, det_out_, rec_in_, rec_out_;
	std::vector<std::string> chars_;
};

} // namespace

std::unique_ptr<ICensrOcr> create_onnx_ocr(const std::string &det_model,
					   const std::string &rec_model)
{
	try {
		return std::make_unique<OnnxOcr>(det_model, rec_model);
	} catch (const Ort::Exception &e) {
		obs_log(LOG_WARNING, "Censr: ONNX OCR init failed: %s", e.what());
		return nullptr;
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "Censr: ONNX OCR init failed: %s", e.what());
		return nullptr;
	}
}

} // namespace censr
