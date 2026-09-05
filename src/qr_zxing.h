/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ocr_engine.h"
#include <vector>

namespace censr {

/* Detect QR codes / barcodes in a BGRA8 image; returns their bounding boxes in
 * the same detection-image pixel space. */
std::vector<RectF> detect_qr_boxes(const uint8_t *bgra, int w, int h);

} // namespace censr
