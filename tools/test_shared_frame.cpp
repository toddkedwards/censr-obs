/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

Round-trip test for the virtual-camera shared-memory transport: write frames
with a known pattern, read them back tear-free, and check size/sequence/content.
Windows-only; built by CENSR_BUILD_TESTS.
*/
#include "../src/host/shared_frame.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace censr;

static int g_fail = 0;
#define CHECK(cond, msg)                                                      \
	do {                                                                 \
		if (!(cond)) {                                                \
			std::printf("FAIL: %s\n", msg);                      \
			++g_fail;                                            \
		}                                                            \
	} while (0)

int main()
{
	const uint32_t W = 8, H = 4;
	SharedFrameWriter writer;
	CHECK(writer.ensure(W, H), "writer.ensure");

	SharedFrameReader reader;
	CHECK(reader.open(), "reader.open");

	std::vector<uint8_t> frame((size_t)W * H * 4);
	std::vector<uint8_t> got;
	uint32_t rw = 0, rh = 0;
	uint64_t seq = 0, prev_seq = 0;

	for (int f = 1; f <= 3; ++f) {
		/* Fill with a frame-dependent pattern. */
		for (size_t i = 0; i < frame.size(); ++i)
			frame[i] = (uint8_t)((i * 7 + f * 31) & 0xff);
		writer.write(frame.data(), W, H, W * 4, (uint64_t)f * 1000);

		CHECK(reader.read(got, rw, rh, seq), "reader.read");
		CHECK(rw == W && rh == H, "dimensions match");
		CHECK(got.size() == frame.size(), "frame size match");
		CHECK(got == frame, "pixel content matches what was written");
		CHECK(seq > prev_seq, "sequence advances on each publish");
		prev_seq = seq;
	}

	/* A second reader can attach to the same live stream. */
	{
		SharedFrameReader reader2;
		CHECK(reader2.open(), "second reader.open");
		CHECK(reader2.read(got, rw, rh, seq), "second reader.read latest");
		CHECK(got == frame, "second reader sees the last frame");
	}

	if (g_fail == 0)
		std::printf("all shared_frame tests passed\n");
	return g_fail ? 1 : 0;
}
