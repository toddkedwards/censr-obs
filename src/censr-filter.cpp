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
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "censr-filter.h"
#include "censr_detector.h"
#include "ocr_engine.h"
#include "secret_rules.h"
#include "license.h"
#include "plugin-support.h"

#define CENSR_FILTER_ID "censr_redaction"

static constexpr uint64_t CENSR_DEFAULT_DELAY_MS = 1500;           /* zero-leak */
static constexpr uint64_t CENSR_DETECT_INTERVAL_NS = 200000000ull; /* 5 Hz */
static constexpr uint64_t CENSR_SNAPSHOT_KEEP_NS = 3000000000ull;  /* retain 3 s */
static constexpr float CENSR_COVER_PAD = 8.0f;                     /* src px */
/* At or above this delay the zero-leak guarantee is expected, so the display
 * fails *closed* (draws black) for any frame whose era detection hasn't reached
 * yet — cold start, engine swap, or a detection stall. Below it the user has
 * opted into low-latency/live output (see the delay-slider help), so we show
 * best-effort covers without black-gating. */
static constexpr uint64_t CENSR_GUARANTEE_MIN_DELAY_NS = 500000000ull; /* 500 ms */
/* amber (r=255,g=191,b=0) in OBS RGB int (0xBBGGRR). */
static constexpr uint32_t CENSR_DEFAULT_COLOR = 255u | (191u << 8);

#define S_DELAY "delay_ms"
#define S_COLOR "cover_color"
#define S_ENGINE "ocr_engine"
#define S_RULE_KEYS "rule_apikeys"
#define S_RULE_LABELS "rule_labels"
#define S_RULE_EMAILS "rule_emails"
#define S_RULE_QR "rule_qr"
#define S_PER_WORD "per_word"
#define S_LICENSE "license_key"

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

	std::mutex detector_mutex; /* guards detector during engine rebuild */
	std::unique_ptr<censr::Detector> detector;
	std::string det_model, rec_model;
	int engine_pref = 0; /* 0 auto, 1 gpu, 2 windows */
	uint32_t rule_mask = censr::CENSR_RULE_ALL;
	bool per_word = false;

	/* Settings (read on the render thread). */
	std::atomic<uint64_t> delay_ns{CENSR_DEFAULT_DELAY_MS * 1000000ull};
	std::atomic<uint32_t> cover_color{CENSR_DEFAULT_COLOR};

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

	gs_effect_t *round_effect = nullptr; /* rounded-rect cover shader */

	uint64_t last_delaylog_ns = 0;
};

/* Rounded-rectangle fill: opaque interior, corners rounded within the padding
 * margin (anti-aliased edge). */
static const char *CENSR_ROUND_EFFECT = R"(
uniform float4x4 ViewProj;
uniform float4 color;
uniform float2 box_size;
uniform float radius;

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData o;
	o.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	o.uv = v_in.uv;
	return o;
}

float4 PSRound(VertData v_in) : TARGET
{
	float2 p = v_in.uv * box_size;
	float2 hs = box_size * 0.5;
	float2 q = abs(p - hs) - (hs - radius);
	float d = length(max(q, float2(0.0, 0.0))) - radius;
	float a = 1.0 - smoothstep(-1.0, 1.0, d);
	return float4(color.rgb, color.a * a);
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSRound(v_in);
	}
}
)";

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

struct CoverSet {
	std::vector<censr_box> boxes;
	/* True once a detection snapshot at or after display_ts exists, i.e.
	 * detection has observed this frame's era. While false (detector cold,
	 * mid engine-swap, or momentarily behind) we cannot vouch for the frame. */
	bool detection_current = false;
};

/* Covers for the frame captured at display_ts. Detection only samples at ~5 Hz
 * while every frame is displayed, so a single "nearest snapshot" can predate an
 * appearing secret (leaking it) or lag a moving one. Instead union every
 * snapshot within one detection cadence of display_ts: an appearing secret's
 * first detection, a disappearing secret's last, and both endpoints of a moving
 * secret all fall inside the window, so the result over-covers (safe) rather
 * than under-covers. The window is widened by two frame intervals to absorb the
 * readback cadence jitter (a snapshot can land up to ~one frame late). */
CoverSet covers_for(censr_filter *filter, uint64_t display_ts)
{
	const uint64_t win = CENSR_DETECT_INTERVAL_NS + 2 * filter->interval_ns;
	CoverSet out;
	std::lock_guard<std::mutex> lk(filter->snap_mutex);
	for (const auto &s : filter->snapshots) {
		uint64_t dt = s.ts > display_ts ? s.ts - display_ts
						: display_ts - s.ts;
		if (dt <= win)
			out.boxes.insert(out.boxes.end(), s.boxes.begin(),
					 s.boxes.end());
		if (s.ts >= display_ts)
			out.detection_current = true;
	}
	return out;
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
	if (!filter->interval_ns)
		return 0;
	size_t n = (size_t)(filter->delay_ns.load() / filter->interval_ns);
	/* Keep at least one frame so capture/readback/display always run
	 * (delay 0 => ~live, ~one-frame latency). */
	return n < 1 ? 1 : n;
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
		/* Snapshots hold boxes in the OLD source pixel space; drop them so a
		 * post-resize frame can't be covered with stale coordinates (which
		 * would misplace covers and leave the secret's new position bare). */
		{
			std::lock_guard<std::mutex> lk(filter->snap_mutex);
			filter->snapshots.clear();
		}
		if (filter->stage) {
			obs_enter_graphics();
			gs_stagesurface_destroy(filter->stage);
			obs_leave_graphics();
			filter->stage = nullptr;
			filter->stage_w = filter->stage_h = 0;
		}
	}

	struct obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && ovi.fps_num)
		filter->interval_ns =
			(uint64_t)ovi.fps_den * 1000000000ull / ovi.fps_num;

	/* Resize when the output fps or the delay setting changes. */
	if (filter->target_valid && filter->interval_ns &&
	    filter->frames.size() != wanted_frames(filter))
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

void draw_covers(censr_filter *filter, const std::vector<censr_box> &boxes)
{
	if (boxes.empty())
		return;

	if (!filter->round_effect) {
		char *err = nullptr;
		filter->round_effect =
			gs_effect_create(CENSR_ROUND_EFFECT, "censr_round", &err);
		if (err) {
			obs_log(LOG_WARNING, "Censr round-rect effect error: %s", err);
			bfree(err);
		}
		if (!filter->round_effect)
			return;
	}

	/* Convert the picked sRGB color to linear (OBS composites in linear
	 * space) and force full alpha. */
	uint32_t rgb = filter->cover_color.load();
	struct vec4 col;
	vec4_from_rgba_srgb(&col, rgb | 0xFF000000u);

	gs_effect_t *eff = filter->round_effect;
	gs_eparam_t *p_color = gs_effect_get_param_by_name(eff, "color");
	gs_eparam_t *p_size = gs_effect_get_param_by_name(eff, "box_size");
	gs_eparam_t *p_radius = gs_effect_get_param_by_name(eff, "radius");

	/* Draw into the sRGB framebuffer so the linear color is encoded correctly
	 * on store and the anti-aliased corners blend in linear space; the
	 * interior is alpha=1 -> fully opaque. */
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	for (const auto &b : boxes) {
		float radius = std::min({CENSR_COVER_PAD, b.w * 0.35f, b.h * 0.35f});
		if (radius < 1.f)
			radius = 1.f;
		struct vec2 sz;
		vec2_set(&sz, b.w, b.h);
		gs_effect_set_vec4(p_color, &col);
		gs_effect_set_vec2(p_size, &sz);
		gs_effect_set_float(p_radius, radius);

		gs_matrix_push();
		gs_matrix_translate3f(b.x, b.y, 0.0f);
		while (gs_effect_loop(eff, "Draw"))
			gs_draw_sprite(nullptr, 0, (uint32_t)b.w, (uint32_t)b.h);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
}

/* Unlicensed watermark: a translucent amber badge in the bottom-right. Redaction
 * still runs fully (we never leak a secret to sell); this only nags until a key is
 * activated. Reuses the rounded-rect cover effect. */
void draw_watermark(censr_filter *filter)
{
	if (!filter->round_effect) {
		char *err = nullptr;
		filter->round_effect =
			gs_effect_create(CENSR_ROUND_EFFECT, "censr_round", &err);
		if (err) {
			obs_log(LOG_WARNING, "Censr round-rect effect error: %s", err);
			bfree(err);
		}
		if (!filter->round_effect)
			return;
	}

	const float W = 250.f, H = 40.f, M = 20.f;
	float x = (float)filter->cx - W - M;
	float y = (float)filter->cy - H - M;
	if (x < 0.f)
		x = 0.f;
	if (y < 0.f)
		y = 0.f;

	gs_effect_t *eff = filter->round_effect;
	struct vec4 col;
	vec4_set(&col, 1.0f, 0.749f, 0.0f, 0.72f); /* amber, translucent */
	struct vec2 sz;
	vec2_set(&sz, W, H);
	gs_effect_set_vec4(gs_effect_get_param_by_name(eff, "color"), &col);
	gs_effect_set_vec2(gs_effect_get_param_by_name(eff, "box_size"), &sz);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "radius"), 12.0f);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(eff, "Draw"))
		gs_draw_sprite(nullptr, 0, (uint32_t)W, (uint32_t)H);
	gs_matrix_pop();
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
		std::lock_guard<std::mutex> lk(filter->detector_mutex);
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

	if (!filter->target_valid || !target || !parent) {
		/* No usable source this frame; nothing is being redacted. */
		obs_source_skip_video_filter(filter->context);
		return;
	}
	if (filter->frames.empty()) {
		/* Ring not built yet. Fail *closed* — never pass the raw,
		 * undelayed source through a redaction filter. */
		draw_black(filter->cx, filter->cy);
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
	if (!disp.has_content) {
		draw_black(filter->cx, filter->cy); /* buffer still filling */
	} else {
		CoverSet cov = covers_for(filter, disp.ts);
		bool guarantee =
			filter->delay_ns.load() >= CENSR_GUARANTEE_MIN_DELAY_NS;
		if (guarantee && !cov.detection_current) {
			/* Detection hasn't reached this frame's era (cold start,
			 * engine swap, or a stall). Fail closed rather than risk
			 * showing an unscanned secret. */
			draw_black(filter->cx, filter->cy);
		} else {
			draw_delayed_frame(disp, filter->cx, filter->cy);
			draw_covers(filter, cov.boxes);
			if (!censr::license().active())
				draw_watermark(filter);
		}
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

censr::Detector::Engine engine_from_int(int v)
{
	if (v == 1)
		return censr::Detector::Engine::ONNX;
	if (v == 2)
		return censr::Detector::Engine::WINRT;
	return censr::Detector::Engine::AUTO;
}

uint32_t rules_from_settings(obs_data_t *s)
{
	uint32_t m = 0;
	if (obs_data_get_bool(s, S_RULE_KEYS))
		m |= censr::CENSR_RULE_APIKEYS;
	if (obs_data_get_bool(s, S_RULE_LABELS))
		m |= censr::CENSR_RULE_LABELS;
	if (obs_data_get_bool(s, S_RULE_EMAILS))
		m |= censr::CENSR_RULE_EMAILS;
	if (obs_data_get_bool(s, S_RULE_QR))
		m |= censr::CENSR_RULE_QR;
	return m;
}

std::unique_ptr<censr::Detector> make_detector(censr_filter *filter,
					       censr::Detector::Engine eng)
{
	auto d = std::make_unique<censr::Detector>(
		[filter](uint64_t ts, const std::vector<censr::RectF> &boxes) {
			apply_detection(filter, ts, boxes);
		},
		filter->det_model, filter->rec_model, eng);
	d->set_rules(filter->rule_mask);
	d->set_per_word(filter->per_word);
	return d;
}

void *censr_create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new censr_filter();
	filter->context = source;

	/* Resolve GPU OCR model paths in the plugin data dir (empty -> the
	 * detector falls back to Windows OCR). */
	char *dp = obs_module_file("models/ch_PP-OCRv4_det_infer.onnx");
	char *rp = obs_module_file("models/ch_PP-OCRv4_rec_infer.onnx");
	if (dp) {
		filter->det_model = dp;
		bfree(dp);
	}
	if (rp) {
		filter->rec_model = rp;
		bfree(rp);
	}

	filter->delay_ns.store((uint64_t)obs_data_get_int(settings, S_DELAY) *
			       1000000ull);
	filter->cover_color.store((uint32_t)obs_data_get_int(settings, S_COLOR));
	filter->rule_mask = rules_from_settings(settings);
	filter->per_word = obs_data_get_bool(settings, S_PER_WORD);
	filter->engine_pref = (int)obs_data_get_int(settings, S_ENGINE);

	filter->detector = make_detector(filter, engine_from_int(filter->engine_pref));

	obs_log(LOG_INFO, "Censr Redaction filter created");
	return filter;
}

void censr_update(void *data, obs_data_t *settings)
{
	auto *filter = static_cast<censr_filter *>(data);

	filter->delay_ns.store((uint64_t)obs_data_get_int(settings, S_DELAY) *
			       1000000ull);
	filter->cover_color.store((uint32_t)obs_data_get_int(settings, S_COLOR));
	filter->rule_mask = rules_from_settings(settings);
	filter->per_word = obs_data_get_bool(settings, S_PER_WORD);
	{
		std::lock_guard<std::mutex> lk(filter->detector_mutex);
		if (filter->detector) {
			filter->detector->set_rules(filter->rule_mask);
			filter->detector->set_per_word(filter->per_word);
		}
	}

	int engine = (int)obs_data_get_int(settings, S_ENGINE);
	if (engine != filter->engine_pref) {
		filter->engine_pref = engine;
		/* Build the new detector first (its OCR inits async on the
		 * worker), swap under the lock, then join the old one outside. */
		auto nd = make_detector(filter, engine_from_int(engine));
		std::unique_ptr<censr::Detector> old;
		{
			std::lock_guard<std::mutex> lk(filter->detector_mutex);
			old = std::move(filter->detector);
			filter->detector = std::move(nd);
		}
		old.reset();
		obs_log(LOG_INFO, "Censr: OCR engine set to %s",
			engine == 1 ? "GPU (ONNX)"
				    : (engine == 2 ? "Windows OCR" : "Auto"));
	}
}

void censr_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, S_DELAY, (long long)CENSR_DEFAULT_DELAY_MS);
	obs_data_set_default_int(s, S_COLOR, (long long)CENSR_DEFAULT_COLOR);
	obs_data_set_default_int(s, S_ENGINE, 0);
	obs_data_set_default_bool(s, S_RULE_KEYS, true);
	obs_data_set_default_bool(s, S_RULE_LABELS, true);
	obs_data_set_default_bool(s, S_RULE_EMAILS, true);
	obs_data_set_default_bool(s, S_RULE_QR, true);
	obs_data_set_default_bool(s, S_PER_WORD, false);
}

bool censr_activate_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	auto *filter = static_cast<censr_filter *>(data);
	if (filter && filter->context) {
		obs_data_t *s = obs_source_get_settings(filter->context);
		const char *key = obs_data_get_string(s, S_LICENSE);
		censr::license().activate(key ? key : "");
		obs_data_release(s);
	}
	obs_property_t *st = obs_properties_get(props, "license_status");
	if (st)
		obs_property_set_description(st, censr::license().status_text().c_str());
	return true; /* refresh the panel so the status line updates */
}

obs_properties_t *censr_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();

	obs_property_t *delay = obs_properties_add_int_slider(
		p, S_DELAY, "Zero-leak delay (ms)", 0, 2000, 50);
	obs_property_int_set_suffix(delay, " ms");
	obs_property_set_long_description(
		delay,
		"Output is held this long so detection finishes before a frame is "
		"shown; a secret is never displayed uncovered. 0 = live (lower "
		"latency, brief exposure on first appearance).");

	obs_properties_add_color(p, S_COLOR, "Cover color");

	obs_property_t *eng = obs_properties_add_list(p, S_ENGINE, "OCR engine",
						      OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(eng, "Auto (GPU, else Windows)", 0);
	obs_property_list_add_int(eng, "GPU (ONNX / DirectML)", 1);
	obs_property_list_add_int(eng, "Windows OCR", 2);

	obs_properties_add_bool(p, S_RULE_KEYS, "Redact API keys & tokens");
	obs_properties_add_bool(p, S_RULE_LABELS,
				"Redact password / secret / token lines");
	obs_properties_add_bool(p, S_RULE_EMAILS, "Redact email addresses");
	obs_properties_add_bool(p, S_RULE_QR, "Redact QR codes / barcodes");

	obs_property_t *pw = obs_properties_add_bool(
		p, S_PER_WORD, "Per-word redaction (hide only the secret token)");
	obs_property_set_long_description(
		pw,
		"Cover only the matched secret token instead of the whole line "
		"(e.g. hide the key value but leave the label visible).");

	/* --- License --------------------------------------------------------- */
	obs_properties_add_text(p, S_LICENSE, "Censr license key", OBS_TEXT_DEFAULT);
	obs_properties_add_button2(p, "activate_btn", "Activate license",
				   censr_activate_clicked, data);
	obs_property_t *lst = obs_properties_add_text(
		p, "license_status", censr::license().status_text().c_str(),
		OBS_TEXT_INFO);
	obs_property_set_long_description(
		lst,
		"Redaction always runs. Without a valid license an amber badge shows on "
		"your stream; activating a key removes it. Get a key at blackbeaconstudios.net.");

	return p;
}

void censr_destroy(void *data)
{
	auto *filter = static_cast<censr_filter *>(data);

	filter->detector.reset();

	free_frames(filter);
	obs_enter_graphics();
	if (filter->stage)
		gs_stagesurface_destroy(filter->stage);
	if (filter->round_effect)
		gs_effect_destroy(filter->round_effect);
	obs_leave_graphics();

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
	info.get_properties = censr_properties;
	info.get_defaults = censr_defaults;
	info.update = censr_update;
	return info;
}

} // namespace

extern "C" void censr_register_filter(void)
{
	/* Restore + re-validate any saved license. Empty dir => censr-core's shared
	 * per-user location, so activating in the plugin, the host, or a virtual
	 * camera licenses the whole suite. */
	censr::license().load("");

	static obs_source_info censr_filter_info = make_censr_filter_info();
	obs_register_source(&censr_filter_info);
	obs_log(LOG_INFO, "registered filter '%s' (Censr Redaction)", CENSR_FILTER_ID);
}
