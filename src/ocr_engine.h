/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace censr {

/* Axis-aligned rectangle in pixels (origin top-left). */
struct RectF {
	float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

struct OcrWord {
	std::string text;
	RectF box; /* detection-image pixel space */
};

struct OcrLine {
	std::string text;
	std::vector<OcrWord> words;
	RectF box; /* union of word boxes */
};

/* OCR engine behind an interface so a GPU engine (PaddleOCR/ONNX on the RTX
 * 3090) can be swapped in later without touching the pipeline. */
struct ICensrOcr {
	virtual ~ICensrOcr() = default;

	/* Recognize text in a BGRA8 image. Must be called on a thread with an
	 * initialized apartment (the WinRT impl requires it). */
	virtual std::vector<OcrLine> recognize(const uint8_t *bgra, int w, int h,
					       int stride) = 0;
};

/* Creates the Windows.Media.Ocr engine. Returns nullptr if unavailable.
 * Call on a thread that has already initialized a WinRT apartment. */
std::unique_ptr<ICensrOcr> create_winrt_ocr();

/* Creates the ONNX Runtime (DirectML) PP-OCR engine from model file paths.
 * Returns nullptr if the models can't be loaded or the GPU EP is unavailable. */
std::unique_ptr<ICensrOcr> create_onnx_ocr(const std::string &det_model,
					   const std::string &rec_model);

} // namespace censr
