/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/*
 * Zero-leak pipeline (step 3):
 *   - Every frame is captured into a GPU ring buffer and displayed ~DELAY later.
 *   - ~5 Hz the just-captured frame is read back to CPU and sent to the async
 *     detection worker (OCR + secret rules + QR), which returns cover boxes
 *     tagged with that frame's timestamp (a "snapshot").
 *   - When a buffered frame is finally displayed, we draw the snapshot whose
 *     timestamp matches that frame's era. Because DELAY (~1.5 s) is far larger
 *     than detection latency (~0.2-0.4 s), detection for the displayed frame has
 *     always finished before it is shown -> a secret is never displayed
 *     uncovered, not even for one frame. During the initial buffer fill the
 *     output is black (also leak-free).
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "censr-filter.h"
#include "censr_detector.h"
#include "ocr_engine.h"
#include "plugin-support.h"

#define CENSR_FILTER_ID "censr_redaction"

static constexpr uint64_t CENSR_DELAY_NS = 1500000000ull;          /* 1.5 s delay */
static constexpr uint64_t CENSR_DETECT_INTERVAL_NS = 200000000ull; /* 5 Hz */
static constexpr uint64_t CENSR_SNAPSHOT_KEEP_NS = 3000000000ull;  /* retain 3 s */
static constexpr float CENSR_COVER_PAD = 8.0f;                     /* src px */

namespace {

struct censr_box {
	float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

/* One buffered source frame (full resolution) in the delay ring. */
struct FrameSlot {
	gs_texrender_t *render = nullptr;
	uint64_t ts = 0;
	bool has_content = false;
};

/* Detection result for the frame captured at ts. */
struct DetectionSnapshot {
	uint64_t ts = 0;
	std::vector<censr_box> boxes; /* source pixel space, already padded */
};

struct censr_filter {
	obs_source_t *context = nullptr;
	std::unique_ptr<censr::Detector> detector;

	/* Time-stamped detection results. */
	std::mutex snap_mutex;
	std::deque<DetectionSnapshot> snapshots;

	/* Delay ring (render thread only). front = oldest. */
	std::deque<FrameSlot> frames;
	uint32_t cx = 0, cy = 0;
	uint64_t interval_ns = 0;
	bool target_valid = false;

	/* Detection readback. */
	gs_stagesurf_t *stage = nullptr;
	uint32_t stage_w = 0, stage_h = 0;
	uint64_t last_detect_ns = 0;

	uint64_t last_delaylog_ns = 0;
};

const char *censr_get_name(void *)
{
	return "Censr Redaction";
}

/* --- detection snapshots ------------------------------------------------- */

void apply_detection(censr_filter *filter, uint64_t ts,
		     const std::vector<censr::RectF> &boxes)
{
	DetectionSnapshot snap;
	snap.ts = ts;
	for (const auto &nb : boxes) {
		float x = nb.x - CENSR_COVER_PAD;
		float y = nb.y - CENSR_COVER_PAD;
		if (x < 0)
			x = 0;
		if (y < 0)
			y = 0;
		snap.boxes.push_back(censr_box{x, y, nb.w + 2 * CENSR_COVER_PAD,
					       nb.h + 2 * CENSR_COVER_PAD});
	}

	uint64_t now = os_gettime_ns();
	std::lock_guard<std::mutex> lk(filter->snap_mutex);
	filter->snapshots.push_back(std::move(snap));
	while (!filter->snapshots.empty() &&
	       filter->snapshots.front().ts + CENSR_SNAPSHOT_KEEP_NS < now)
		filter->snapshots.pop_front();
}

/* Covers for the frame captured at display_ts: the snapshot nearest in time. */
std::vector<censr_box> covers_for(censr_filter *filter, uint64_t display_ts)
{
	std::lock_guard<std::mutex> lk(filter->snap_mutex);
	const DetectionSnapshot *best = nullptr;
	uint64_t best_dt = UINT64_MAX;
	for (const auto &s : filter->snapshots) {
		uint64_t dt = s.ts > display_ts ? s.ts - display_ts
						: display_ts - s.ts;
		if (dt < best_dt) {
			best_dt = dt;
			best = &s;
		}
	}
	if (best)
		return best->boxes;
	return {};
}

/* --- ring management ----------------------------------------------------- */

void free_frames(censr_filter *filter)
{
	obs_enter_graphics();
	for (auto &f : filter->frames)
		if (f.render)
			gs_texrender_destroy(f.render);
	obs_leave_graphics();
	filter->frames.clear();
}

size_t wanted_frames(const censr_filter *filter)
{
	return filter->interval_ns ? (size_t)(CENSR_DELAY_NS / filter->interval_ns)
				   : 0;
}

void resize_ring(censr_filter *filter)
{
	size_t want = wanted_frames(filter);
	if (want == 0)
		return;
	obs_enter_graphics();
	while (filter->frames.size() < want) {
		FrameSlot slot;
		slot.render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		filter->frames.push_back(slot);
	}
	while (filter->frames.size() > want) {
		if (filter->frames.front().render)
			gs_texrender_destroy(filter->frames.front().render);
		filter->frames.pop_front();
	}
	obs_leave_graphics();
}

/* --- tick: track source size + output fps -------------------------------- */

void censr_video_tick(void *data, float)
{
	auto *filter = static_cast<censr_filter *>(data);

	obs_source_t *target = obs_filter_get_target(filter->context);
	uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	uint32_t cy = target ? obs_source_get_base_height(target) : 0;
	filter->target_valid = target && cx && cy;

	if (filter->target_valid && (cx != filter->cx || cy != filter->cy)) {
		filter->cx = cx;
		filter->cy = cy;
		free_frames(filter);
		if (filter->stage) {
			obs_enter_graphics();
			gs_stagesurface_destroy(filter->stage);
			obs_leave_graphics();
			filter->stage = nullptr;
			filter->stage_w = filter->stage_h = 0;
		}
	}

	struct obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && ovi.fps_num) {
		uint64_t interval =
			(uint64_t)ovi.fps_den * 1000000000ull / ovi.fps_num;
		if (interval != filter->interval_ns) {
			filter->interval_ns = interval;
			if (filter->target_valid)
				resize_ring(filter);
		}
	}
	if (filter->target_valid && filter->frames.empty())
		resize_ring(filter);
}

/* --- drawing ------------------------------------------------------------- */

void draw_black(uint32_t cx, uint32_t cy)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 black;
	vec4_set(&black, 0.f, 0.f, 0.f, 1.f);
	gs_effect_set_vec4(color, &black);
	gs_blend_state_push();
	gs_enable_blending(false);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, cx, cy);
	gs_blend_state_pop();
}

void draw_delayed_frame(FrameSlot &f, uint32_t cx, uint32_t cy)
{
	gs_texture_t *tex = gs_texrender_get_texture(f.render);
	if (!tex) {
		draw_black(cx, cy);
		return;
	}
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const bool prev = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, cx, cy);
	gs_enable_framebuffer_srgb(prev);
}

void draw_covers(const std::vector<censr_box> &boxes)
{
	if (boxes.empty())
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 amber;
	vec4_set(&amber, 1.0f, 0.749f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &amber);

	gs_blend_state_push();
	gs_enable_blending(false);
	while (gs_effect_loop(solid, "Solid")) {
		for (const auto &b : boxes) {
			gs_matrix_push();
			gs_matrix_translate3f(b.x, b.y, 0.0f);
			gs_draw_sprite(nullptr, 0, (uint32_t)b.w, (uint32_t)b.h);
			gs_matrix_pop();
		}
	}
	gs_blend_state_pop();
}

/* --- capture (into ring) + readback (for detection) ---------------------- */

void capture_into(censr_filter *filter, FrameSlot &slot, obs_source_t *target,
		  obs_source_t *parent, uint64_t now)
{
	gs_texrender_reset(slot.render);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(slot.render, filter->cx, filter->cy)) {
		uint32_t flags = obs_source_get_output_flags(target);
		bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)filter->cx, 0.0f, (float)filter->cy, -100.0f,
			 100.0f);
		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);
		gs_texrender_end(slot.render);
		slot.ts = now;
		slot.has_content = true;
	}
	gs_blend_state_pop();
}

void readback_for_detection(censr_filter *filter, FrameSlot &slot, uint64_t now)
{
	if (!filter->stage || filter->stage_w != filter->cx ||
	    filter->stage_h != filter->cy) {
		if (filter->stage)
			gs_stagesurface_destroy(filter->stage);
		filter->stage =
			gs_stagesurface_create(filter->cx, filter->cy, GS_RGBA);
		filter->stage_w = filter->cx;
		filter->stage_h = filter->cy;
	}
	gs_texture_t *tex = gs_texrender_get_texture(slot.render);
	if (!tex)
		return;
	gs_stage_texture(filter->stage, tex);

	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(filter->stage, &ptr, &linesize)) {
		std::vector<uint8_t> buf((size_t)filter->cx * filter->cy * 4);
		for (uint32_t y = 0; y < filter->cy; ++y)
			memcpy(buf.data() + (size_t)y * filter->cx * 4,
			       ptr + (size_t)y * linesize, (size_t)filter->cx * 4);
		gs_stagesurface_unmap(filter->stage);
		if (filter->detector)
			filter->detector->submit(std::move(buf), (int)filter->cx,
						 (int)filter->cy, (int)filter->cx,
						 (int)filter->cy, now);
	}
}

/* --- main render --------------------------------------------------------- */

void censr_video_render(void *data, gs_effect_t *effect)
{
	auto *filter = static_cast<censr_filter *>(data);
	UNUSED_PARAMETER(effect);

	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!filter->target_valid || !target || !parent || filter->frames.empty()) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	uint64_t now = os_gettime_ns();

	/* Recycle the oldest slot, capture the current source into it, and move
	 * it to the back of the ring. */
	FrameSlot slot = filter->frames.front();
	filter->frames.pop_front();
	capture_into(filter, slot, target, parent, now);
	filter->frames.push_back(slot);

	/* ~5 Hz: read back the just-captured frame for detection. */
	if (now - filter->last_detect_ns >= CENSR_DETECT_INTERVAL_NS) {
		filter->last_detect_ns = now;
		readback_for_detection(filter, filter->frames.back(), now);
	}

	/* Display the oldest frame (~DELAY old) with its era's covers. */
	FrameSlot &disp = filter->frames.front();
	if (disp.has_content) {
		draw_delayed_frame(disp, filter->cx, filter->cy);
		draw_covers(covers_for(filter, disp.ts));
	} else {
		draw_black(filter->cx, filter->cy); /* buffer still filling */
	}

	if (now - filter->last_delaylog_ns >= 5000000000ull) {
		filter->last_delaylog_ns = now;
		if (disp.has_content)
			obs_log(LOG_INFO,
				"Censr: zero-leak buffer active, showing frame %.0f ms old (%zu frames)",
				(double)(now - disp.ts) / 1.0e6, filter->frames.size());
	}
}

/* --- lifecycle ----------------------------------------------------------- */

void *censr_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	auto *filter = new censr_filter();
	filter->context = source;

	/* Resolve the GPU OCR model paths in the plugin data dir (empty if not
	 * bundled -> the detector falls back to Windows OCR). */
	std::string det_model, rec_model;
	char *dp = obs_module_file("models/ch_PP-OCRv4_det_infer.onnx");
	char *rp = obs_module_file("models/ch_PP-OCRv4_rec_infer.onnx");
	if (dp) {
		det_model = dp;
		bfree(dp);
	}
	if (rp) {
		rec_model = rp;
		bfree(rp);
	}

	filter->detector = std::make_unique<censr::Detector>(
		[filter](uint64_t ts, const std::vector<censr::RectF> &boxes) {
			apply_detection(filter, ts, boxes);
		},
		det_model, rec_model);

	obs_log(LOG_INFO, "Censr Redaction filter created (zero-leak, %.1fs delay)",
		(double)CENSR_DELAY_NS / 1.0e9);
	return filter;
}

void censr_destroy(void *data)
{
	auto *filter = static_cast<censr_filter *>(data);

	filter->detector.reset();

	free_frames(filter);
	if (filter->stage) {
		obs_enter_graphics();
		gs_stagesurface_destroy(filter->stage);
		obs_leave_graphics();
	}

	obs_log(LOG_INFO, "Censr Redaction filter destroyed");
	delete filter;
}

obs_source_info make_censr_filter_info()
{
	obs_source_info info = {};
	info.id = CENSR_FILTER_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = censr_get_name;
	info.create = censr_create;
	info.destroy = censr_destroy;
	info.video_render = censr_video_render;
	info.video_tick = censr_video_tick;
	return info;
}

} // namespace

extern "C" void censr_register_filter(void)
{
	static obs_source_info censr_filter_info = make_censr_filter_info();
	obs_register_source(&censr_filter_info);
	obs_log(LOG_INFO, "registered filter '%s' (Censr Redaction)", CENSR_FILTER_ID);
}
