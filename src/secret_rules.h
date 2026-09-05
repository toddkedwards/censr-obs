/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ocr_engine.h"
#include <vector>

namespace censr {

/* Given OCR lines, return the boxes (in the same detection-image pixel space as
 * the OCR input) that should be covered because they contain a secret. v1 is
 * line-granular: a line matching any rule contributes its whole text box, so
 * non-matching lines (innocuous text) stay visible. */
std::vector<RectF> find_secret_boxes(const std::vector<OcrLine> &lines);

} // namespace censr
