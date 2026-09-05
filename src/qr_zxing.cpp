/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "qr_zxing.h"

#include <ReadBarcode.h>

#include <algorithm>
#include <climits>

namespace censr {

std::vector<RectF> detect_qr_boxes(const uint8_t *bgra, int w, int h)
{
	std::vector<RectF> out;
	if (w <= 0 || h <= 0)
		return out;

	using namespace ZXing;

	ImageView image(bgra, w, h, ImageFormat::BGRA);

	ReaderOptions opts;
	opts.setFormats(BarcodeFormat::LinearCodes | BarcodeFormat::MatrixCodes);
	opts.setTryHarder(true);
	opts.setTryRotate(true);
	opts.setMaxNumberOfSymbols(16);

	Barcodes results = ReadBarcodes(image, opts);
	for (const auto &r : results) {
		if (!r.isValid())
			continue;
		const Position pos = r.position();
		int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
		for (int i = 0; i < 4; ++i) {
			auto p = pos[i];
			minx = std::min(minx, p.x);
			miny = std::min(miny, p.y);
			maxx = std::max(maxx, p.x);
			maxy = std::max(maxy, p.y);
		}
		out.push_back({(float)minx, (float)miny, (float)(maxx - minx),
			       (float)(maxy - miny)});
	}
	return out;
}

} // namespace censr
