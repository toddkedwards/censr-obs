/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/*
 * Two-rate pipeline:
 *   - EVERY FRAME (render thread): draw opaque amber covers over the current
 *     cover set (cheap).
 *   - ~5 Hz (render thread): render the source into a downscaled texrender,
 *     stage it to CPU, and submit to the async detection worker. The worker
 *     (OCR + secret rules + QR) returns cover boxes, which are merged into the
 *     cover set with a TTL so they persist a beat and don't flicker.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "censr-filter.h"
#include "censr_detector.h"
#include "ocr_engine.h"
#include "plugin-support.h"

#define CENSR_FILTER_ID "censr_redaction"

static constexpr uint64_t CENSR_DETECT_INTERVAL_NS = 200000000ull; /* 5 Hz */
/* Detection runs at native width (capped); downscaling made dense monospace
 * secrets unreadable to Windows OCR. OCR is on the worker thread, so the cost
 * of full-res is fine at 5 Hz. */
static constexpr uint32_t CENSR_DETECT_WIDTH = 1920;               /* px */
static constexpr uint64_t CENSR_COVER_TTL_NS = 600000000ull;       /* 600 ms */
static constexpr float CENSR_COVER_PAD = 8.0f;                     /* src px */

namespace {

/* A single opaque cover, in source pixel coordinates (origin top-left). */
struct censr_box {
	float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
	uint64_t expire_ns = 0;
};

struct censr_filter {
	obs_source_t *context = nullptr;

	std::mutex boxes_mutex;
	std::vector<censr_box> boxes;

	std::unique_ptr<censr::Detector> detector;

	/* GPU readback resources (render thread only). */
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stage = nullptr;
	uint32_t stage_w = 0, stage_h = 0;
	uint64_t last_detect_ns = 0;

	uint64_t create_ns = 0;
	bool first_cover_logged = false;
};

float iou(const censr_box &a, const censr::RectF &b)
{
	float ix = std::max(a.x, b.x);
	float iy = std::max(a.y, b.y);
	float ix2 = std::min(a.x + a.w, b.x + b.w);
	float iy2 = std::min(a.y + a.h, b.y + b.h);
	float iw = ix2 - ix, ih = iy2 - iy;
	if (iw <= 0.f || ih <= 0.f)
		return 0.f;
	float inter = iw * ih;
	float uni = a.w * a.h + b.w * b.h - inter;
	return uni > 0.f ? inter / uni : 0.f;
}

/* Merge freshly detected source-space boxes into the cover set. Boxes that
 * overlap an existing cover refresh it (and its TTL); new ones are added.
 * Unrefreshed covers simply expire. Runs on the detector worker thread. */
void apply_detection(censr_filter *filter, const std::vector<censr::RectF> &boxes)
{
	uint64_t now = os_gettime_ns();
	uint64_t expire = now + CENSR_COVER_TTL_NS;

	std::lock_guard<std::mutex> lk(filter->boxes_mutex);
	for (const auto &nb : boxes) {
		censr::RectF padded{nb.x - CENSR_COVER_PAD, nb.y - CENSR_COVER_PAD,
				    nb.w + 2 * CENSR_COVER_PAD,
				    nb.h + 2 * CENSR_COVER_PAD};
		if (padded.x < 0)
			padded.x = 0;
		if (padded.y < 0)
			padded.y = 0;

		int best = -1;
		float best_iou = 0.3f;
		for (size_t i = 0; i < filter->boxes.size(); ++i) {
			float v = iou(filter->boxes[i], padded);
			if (v > best_iou) {
				best_iou = v;
				best = (int)i;
			}
		}
		if (best >= 0) {
			filter->boxes[best] = censr_box{padded.x, padded.y,
							padded.w, padded.h, expire};
		} else {
			filter->boxes.push_back(censr_box{padded.x, padded.y,
							  padded.w, padded.h,
							  expire});
		}
	}

	if (!filter->first_cover_logged && !boxes.empty()) {
		filter->first_cover_logged = true;
		double ms = (double)(now - filter->create_ns) / 1.0e6;
		obs_log(LOG_INFO,
			"Censr: first covers placed %.0f ms after filter start (%zu regions)",
			ms, boxes.size());
	}
}

const char *censr_get_name(void *)
{
	return "Censr Redaction";
}

void *censr_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	auto *filter = new censr_filter();
	filter->context = source;
	filter->create_ns = os_gettime_ns();

	filter->detector = std::make_unique<censr::Detector>(
		[filter](const std::vector<censr::RectF> &boxes) {
			apply_detection(filter, boxes);
		});

	obs_log(LOG_INFO, "Censr Redaction filter created");
	return filter;
}

void censr_destroy(void *data)
{
	auto *filter = static_cast<censr_filter *>(data);

	/* Stop the worker first so its callback can't touch us mid-teardown. */
	filter->detector.reset();

	obs_enter_graphics();
	if (filter->texrender)
		gs_texrender_destroy(filter->texrender);
	if (filter->stage)
		gs_stagesurface_destroy(filter->stage);
	obs_leave_graphics();

	obs_log(LOG_INFO, "Censr Redaction filter destroyed");
	delete filter;
}

void expire_boxes(censr_filter *filter, uint64_t now_ns)
{
	std::lock_guard<std::mutex> lk(filter->boxes_mutex);
	auto &v = filter->boxes;
	v.erase(std::remove_if(v.begin(), v.end(),
				[now_ns](const censr_box &b) {
					return b.expire_ns != 0 && b.expire_ns <= now_ns;
				}),
		v.end());
}

/* Draw every cover as a fully opaque amber rectangle (blending disabled so it
 * overwrites the pixels underneath). */
void draw_covers(censr_filter *filter)
{
	std::vector<censr_box> boxes;
	{
		std::lock_guard<std::mutex> lk(filter->boxes_mutex);
		boxes = filter->boxes;
	}
	if (boxes.empty())
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");

	struct vec4 amber;
	vec4_set(&amber, 1.0f, 0.749f, 0.0f, 1.0f);
	gs_effect_set_vec4(color_param, &amber);

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

/* ~5 Hz: render the source into a downscaled texrender, stage to CPU, submit to
 * the detector. Runs on the render thread. */
void do_readback(censr_filter *filter)
{
	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (!target || !parent)
		return;
	uint32_t sw = obs_source_get_base_width(target);
	uint32_t sh = obs_source_get_base_height(target);
	if (sw == 0 || sh == 0)
		return;

	uint32_t tw = std::min<uint32_t>(CENSR_DETECT_WIDTH, sw);
	uint32_t th = (uint32_t)((uint64_t)sh * tw / sw);
	if (th == 0)
		th = 1;

	if (!filter->texrender)
		filter->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!filter->stage || filter->stage_w != tw || filter->stage_h != th) {
		if (filter->stage)
			gs_stagesurface_destroy(filter->stage);
		filter->stage = gs_stagesurface_create(tw, th, GS_RGBA);
		filter->stage_w = tw;
		filter->stage_h = th;
	}

	gs_texrender_reset(filter->texrender);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (!gs_texrender_begin(filter->texrender, tw, th)) {
		gs_blend_state_pop();
		return;
	}

	uint32_t flags = obs_source_get_output_flags(target);
	bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	bool async = (flags & OBS_SOURCE_ASYNC) != 0;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);

	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);

	gs_texrender_end(filter->texrender);
	gs_blend_state_pop();

	gs_texture_t *tex = gs_texrender_get_texture(filter->texrender);
	if (!tex)
		return;
	gs_stage_texture(filter->stage, tex);

	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(filter->stage, &ptr, &linesize)) {
		std::vector<uint8_t> buf((size_t)tw * th * 4);
		for (uint32_t y = 0; y < th; ++y)
			memcpy(buf.data() + (size_t)y * tw * 4,
			       ptr + (size_t)y * linesize, (size_t)tw * 4);
		gs_stagesurface_unmap(filter->stage);

		if (filter->detector)
			filter->detector->submit(std::move(buf), (int)tw,
						 (int)th, (int)sw, (int)sh);
	}
}

void censr_video_render(void *data, gs_effect_t *effect)
{
	auto *filter = static_cast<censr_filter *>(data);

	obs_source_skip_video_filter(filter->context);

	uint64_t now = os_gettime_ns();
	if (now - filter->last_detect_ns >= CENSR_DETECT_INTERVAL_NS) {
		filter->last_detect_ns = now;
		do_readback(filter);
	}

	expire_boxes(filter, now);
	draw_covers(filter);

	UNUSED_PARAMETER(effect);
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
	return info;
}

} // namespace

extern "C" void censr_register_filter(void)
{
	static obs_source_info censr_filter_info = make_censr_filter_info();
	obs_register_source(&censr_filter_info);
	obs_log(LOG_INFO, "registered filter '%s' (Censr Redaction)", CENSR_FILTER_ID);
}
