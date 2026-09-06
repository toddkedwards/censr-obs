/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

Diagnostic reader for the virtual-camera frame stream. Attaches to the shared
memory a running censr-host publishes and reports the frame size and that the
sequence advances (i.e. live redacted frames are flowing). Stands in for the
real device backend until it exists. Usage: censr-vcam-probe [seconds]
*/
#include "shared_frame.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <windows.h>

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 3;

	censr::SharedFrameReader reader;
	for (int i = 0; i < 50 && !reader.open(); ++i)
		Sleep(100);
	if (!reader.open()) {
		std::printf("vcam-probe: no censr-host frame stream found "
			    "(is the host running?)\n");
		return 1;
	}

	std::vector<uint8_t> frame;
	uint32_t w = 0, h = 0;
	uint64_t seq = 0, first_seq = 0, last_seq = 0;
	int reads = 0;
	DWORD end = GetTickCount() + (DWORD)seconds * 1000;
	while (GetTickCount() < end) {
		if (reader.read(frame, w, h, seq)) {
			if (reads == 0) {
				first_seq = seq;
				std::printf("vcam-probe: stream is %ux%u BGRA\n", w,
					    h);
			}
			last_seq = seq;
			++reads;
		}
		Sleep(33);
	}

	uint64_t advanced = last_seq - first_seq;
	std::printf("vcam-probe: %d reads, sequence advanced by %llu over %ds\n",
		    reads, (unsigned long long)advanced, seconds);
	if (reads == 0 || advanced == 0) {
		std::printf("vcam-probe: FAIL — no live frames\n");
		return 1;
	}
	std::printf("vcam-probe: OK — live redacted frames are flowing\n");
	return 0;
}
