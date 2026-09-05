/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ocr_engine.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace censr {

/* Async detection worker. The render thread hands it downscaled RGBA frames via
 * submit(); a background thread runs OCR + secret rules (+ QR) and calls the
 * cover callback with boxes in SOURCE pixel space. Latest-frame-wins: if the
 * worker is busy, a newer submit replaces the pending frame instead of queueing.
 */
class Detector {
public:
	/* ts identifies the source frame the boxes were computed from, so the
	 * zero-leak buffer can match covers to the delayed frame's era. */
	using CoverCallback =
		std::function<void(uint64_t ts, const std::vector<RectF> &source_boxes)>;

	explicit Detector(CoverCallback cb);
	~Detector();

	/* Called on the render thread. rgba is tightly packed (w*4 stride). */
	void submit(std::vector<uint8_t> rgba, int w, int h, int src_w, int src_h,
		    uint64_t ts);

private:
	struct Frame {
		std::vector<uint8_t> rgba;
		int w = 0, h = 0, src_w = 0, src_h = 0;
		uint64_t ts = 0;
	};

	void run();

	CoverCallback cb_;
	std::unique_ptr<ICensrOcr> ocr_;

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	Frame pending_;
	bool has_pending_ = false;
	std::atomic<bool> stop_{false};
};

} // namespace censr
