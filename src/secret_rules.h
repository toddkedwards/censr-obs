/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ocr_engine.h"
#include <cstdint>
#include <vector>

namespace censr {

/* Secret rule categories (bit flags), toggled from the filter UI. */
enum CensrRuleFlags : uint32_t {
	CENSR_RULE_APIKEYS = 1u << 0, /* sk-/ghp_/AKIA + high-entropy tokens */
	CENSR_RULE_LABELS = 1u << 1,  /* password/secret/token=... lines */
	CENSR_RULE_EMAILS = 1u << 2,  /* email addresses */
	CENSR_RULE_QR = 1u << 3,      /* QR / barcodes (handled in the detector) */
	CENSR_RULE_ALL = 0xFu,
};

/* Given OCR lines, return the boxes (in the same detection-image pixel space as
 * the OCR input) that should be covered because they contain a secret in one of
 * the enabled categories.
 *   per_word == false: line-granular (a matching line contributes its whole box).
 *   per_word == true:  cover only the secret token(s) within the line, mapping
 *                      the matched character span to a sub-box proportionally. */
std::vector<RectF> find_secret_boxes(const std::vector<OcrLine> &lines,
				     uint32_t rule_mask, bool per_word);

} // namespace censr
