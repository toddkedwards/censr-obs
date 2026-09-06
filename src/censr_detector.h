/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ocr_engine.h"
#include "secret_rules.h"

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

	/* Which OCR engine to use. AUTO = ONNX if models load, else Windows. */
	enum class Engine { AUTO, ONNX, WINRT };

	Detector(CoverCallback cb, std::string det_model, std::string rec_model,
		 Engine engine);
	~Detector();

	/* Called on the render thread. rgba is tightly packed (w*4 stride). */
	void submit(std::vector<uint8_t> rgba, int w, int h, int src_w, int src_h,
		    uint64_t ts);

	/* Live-updatable enabled rule categories (CensrRuleFlags bitmask). */
	void set_rules(uint32_t mask) { rule_mask_.store(mask); }

	/* Live-updatable per-word (vs per-line) redaction granularity. */
	void set_per_word(bool v) { per_word_.store(v); }

private:
	struct Frame {
		std::vector<uint8_t> rgba;
		int w = 0, h = 0, src_w = 0, src_h = 0;
		uint64_t ts = 0;
	};

	void run();

	CoverCallback cb_;
	std::unique_ptr<ICensrOcr> ocr_;
	std::string det_model_, rec_model_;
	Engine engine_ = Engine::AUTO;
	std::atomic<uint32_t> rule_mask_{CENSR_RULE_ALL};
	std::atomic<bool> per_word_{false};

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	Frame pending_;
	bool has_pending_ = false;
	std::atomic<bool> stop_{false};
};

} // namespace censr
