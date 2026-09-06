/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/* Shared-memory frame transport for the virtual camera. censr-host writes the
 * finished (redacted) BGRA frames here; a virtual-camera device (DirectShow or
 * Media Foundation media source, Phase 2) reads the latest frame and hands it to
 * the app requesting a webcam. Single producer, one-or-more readers, triple-
 * buffered with a sequence number for tear-free reads. See
 * docs/roadmap-standalone-vcam.md. */

#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>

namespace censr {

constexpr uint32_t CENSR_SHM_MAGIC = 0x52534E43;   /* 'CNSR' */
constexpr uint32_t CENSR_SHM_VERSION = 1;
constexpr uint32_t CENSR_SHM_BUFFERS = 3;          /* triple buffer */
constexpr wchar_t CENSR_SHM_NAME[] = L"Local\\CensrVCamFrame";

/* Layout at the head of the mapping, followed by CENSR_SHM_BUFFERS frame
 * buffers of `frame_size` bytes each. All fields fixed-width for cross-process
 * use; `latest`/`seq` are updated with interlocked ops. */
struct SharedFrameHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride;      /* bytes per row = width*4 (BGRA8) */
	uint32_t frame_size;  /* stride*height */
	uint32_t buffers;
	volatile LONG latest; /* index of the most recently published buffer */
	volatile LONGLONG seq;/* increments on every publish */
	volatile LONGLONG timestamp_ns;
};

/* Single-producer writer. */
class SharedFrameWriter {
public:
	/* (Re)create the mapping if the frame size changed. */
	bool ensure(uint32_t w, uint32_t h);
	/* Publish one BGRA frame (src may have a wider stride). */
	void write(const uint8_t *bgra, uint32_t w, uint32_t h, size_t src_stride,
		   uint64_t ts_ns);
	void close();
	~SharedFrameWriter() { close(); }

private:
	HANDLE map_ = nullptr;
	uint8_t *base_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	uint32_t next_ = 0;
};

/* Reader (used by the device backend and by tests/probes). */
class SharedFrameReader {
public:
	bool open();
	/* Copy the latest frame into `out` (tear-free). Returns false if no
	 * mapping or no frame yet; sets w/h/seq on success. */
	bool read(std::vector<uint8_t> &out, uint32_t &w, uint32_t &h,
		  uint64_t &seq);
	void close();
	~SharedFrameReader() { close(); }

private:
	HANDLE map_ = nullptr;
	uint8_t *base_ = nullptr;
};

} // namespace censr
