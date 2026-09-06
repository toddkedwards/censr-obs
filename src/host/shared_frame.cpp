/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shared_frame.h"

#include <cstdio>
#include <cstring>

namespace censr {

/* Kept free of censr-core so the (future) device backend can link this file
 * alone. Logs go straight to stderr. */
static void shm_warn(const char *what)
{
	fprintf(stderr, "[censr:warning] Censr host: %s (%lu)\n", what,
		GetLastError());
}

static size_t total_size(uint32_t w, uint32_t h)
{
	return sizeof(SharedFrameHeader) + (size_t)CENSR_SHM_BUFFERS * w * h * 4;
}

bool SharedFrameWriter::ensure(uint32_t w, uint32_t h)
{
	if (map_ && w == w_ && h == h_)
		return true;
	close();
	if (!w || !h)
		return false;

	size_t total = total_size(w, h);
	map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
				  (DWORD)(total >> 32), (DWORD)(total & 0xffffffff),
				  CENSR_SHM_NAME);
	if (!map_) {
		shm_warn("CreateFileMapping failed");
		return false;
	}
	base_ = (uint8_t *)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, total);
	if (!base_) {
		shm_warn("MapViewOfFile failed");
		CloseHandle(map_);
		map_ = nullptr;
		return false;
	}

	auto *hd = (SharedFrameHeader *)base_;
	memset(hd, 0, sizeof(*hd));
	hd->magic = CENSR_SHM_MAGIC;
	hd->version = CENSR_SHM_VERSION;
	hd->width = w;
	hd->height = h;
	hd->stride = w * 4;
	hd->frame_size = w * h * 4;
	hd->buffers = CENSR_SHM_BUFFERS;
	hd->latest = -1;
	hd->seq = 0;
	w_ = w;
	h_ = h;
	next_ = 0;
	fprintf(stderr,
		"[censr:info] Censr host: virtual-camera frame stream %ux%u ready\n",
		w, h);
	return true;
}

void SharedFrameWriter::write(const uint8_t *bgra, uint32_t w, uint32_t h,
			      size_t src_stride, uint64_t ts_ns)
{
	if (!base_ || w != w_ || h != h_)
		return;
	auto *hd = (SharedFrameHeader *)base_;

	/* Write into a buffer that is not the currently-published one. */
	uint32_t idx = next_;
	if ((LONG)idx == hd->latest)
		idx = (idx + 1) % CENSR_SHM_BUFFERS;
	uint8_t *dst = base_ + sizeof(SharedFrameHeader) +
		       (size_t)idx * hd->frame_size;
	const uint32_t row = hd->stride;
	for (uint32_t y = 0; y < h; ++y)
		memcpy(dst + (size_t)y * row, bgra + (size_t)y * src_stride, row);

	/* Publish: timestamp, then latest index, then bump seq (release order). */
	hd->timestamp_ns = (LONGLONG)ts_ns;
	MemoryBarrier();
	InterlockedExchange(&hd->latest, (LONG)idx);
	InterlockedIncrement64(&hd->seq);
	next_ = (idx + 1) % CENSR_SHM_BUFFERS;
}

void SharedFrameWriter::close()
{
	if (base_)
		UnmapViewOfFile(base_);
	if (map_)
		CloseHandle(map_);
	base_ = nullptr;
	map_ = nullptr;
	w_ = h_ = 0;
}

bool SharedFrameReader::open()
{
	close();
	map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, CENSR_SHM_NAME);
	if (!map_)
		return false;
	base_ = (uint8_t *)MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0);
	if (!base_) {
		CloseHandle(map_);
		map_ = nullptr;
		return false;
	}
	return true;
}

bool SharedFrameReader::read(std::vector<uint8_t> &out, uint32_t &w, uint32_t &h,
			     uint64_t &seq)
{
	if (!base_)
		return false;
	auto *hd = (SharedFrameHeader *)base_;
	if (hd->magic != CENSR_SHM_MAGIC || hd->frame_size == 0)
		return false;

	/* Tear-free: re-read seq around the copy; retry if the producer published
	 * mid-copy. */
	for (int attempt = 0; attempt < 8; ++attempt) {
		LONGLONG seq0 = hd->seq;
		LONG idx = hd->latest;
		if (idx < 0)
			return false;
		MemoryBarrier();
		w = hd->width;
		h = hd->height;
		uint32_t fs = hd->frame_size;
		const uint8_t *srcbuf = base_ + sizeof(SharedFrameHeader) +
					(size_t)idx * fs;
		out.resize(fs);
		memcpy(out.data(), srcbuf, fs);
		MemoryBarrier();
		if (hd->seq == seq0 && hd->latest == idx) {
			seq = (uint64_t)seq0;
			return true;
		}
	}
	return false;
}

void SharedFrameReader::close()
{
	if (base_)
		UnmapViewOfFile(base_);
	if (map_)
		CloseHandle(map_);
	base_ = nullptr;
	map_ = nullptr;
}

} // namespace censr
