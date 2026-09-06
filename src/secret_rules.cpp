/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "secret_rules.h"

#include <cctype>
#include <cmath>
#include <regex>
#include <string>

namespace censr {

namespace {

/* Data-driven pattern rules. Kept as a small table so the full set can be
 * expanded later without touching logic. */
const std::regex &email_re()
{
	static const std::regex re(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
	return re;
}
const std::regex &aws_re()
{
	static const std::regex re(R"(AKIA[0-9A-Z]{16})");
	return re;
}
const std::regex &github_re()
{
	static const std::regex re(R"(gh[pousr]_[A-Za-z0-9]{20,})");
	return re;
}
const std::regex &sk_re()
{
	static const std::regex re(R"(sk-[A-Za-z0-9\-]{16,})");
	return re;
}
/* Secret-context label keyword (no trailing [:=] required, because OCR often
 * drops the "=value" part). To avoid matching prose like "nothing secret here",
 * a keyword only counts when the line ALSO looks like an assignment: it has a
 * ':'/'=' or an ALL-CAPS identifier (e.g. GITHUB_TOKEN, SUPPORT_EMAIL). */
const std::regex &label_re()
{
	static const std::regex re(
		R"(\b(pass(word|wd)?|pwd|secret|token|api[_\- ]?key|apikey|access[_\- ]?key|secret[_\- ]?key|private[_\- ]?key|client[_\- ]?secret|github|gitlab|bearer|authorization|credential|e[\- ]?mail)\b)",
		std::regex::icase);
	return re;
}
const std::regex &assignment_re()
{
	static const std::regex re(R"([:=])");
	return re;
}
const std::regex &upper_ident_re()
{
	static const std::regex re(R"([A-Z][A-Z0-9_]{3,})");
	return re;
}

/* Shannon entropy over the characters of s (bits/char). */
double shannon_entropy(const std::string &s)
{
	if (s.empty())
		return 0.0;
	int counts[256] = {0};
	for (unsigned char c : s)
		counts[c]++;
	double H = 0.0;
	const double n = (double)s.size();
	for (int i = 0; i < 256; ++i) {
		if (counts[i]) {
			double p = counts[i] / n;
			H -= p * std::log2(p);
		}
	}
	return H;
}

bool has_mixed_classes(const std::string &s)
{
	bool lower = false, upper = false, digit = false;
	for (unsigned char c : s) {
		if (c >= 'a' && c <= 'z')
			lower = true;
		else if (c >= 'A' && c <= 'Z')
			upper = true;
		else if (c >= '0' && c <= '9')
			digit = true;
	}
	return (lower + upper + digit) >= 2;
}

/* A word that looks like a high-entropy secret token. */
bool is_high_entropy_token(const std::string &w)
{
	if (w.size() < 20)
		return false;
	if (!has_mixed_classes(w))
		return false;
	return shannon_entropy(w) >= 3.2;
}

bool line_is_secret(const OcrLine &line, uint32_t mask)
{
	const std::string &t = line.text;
	if ((mask & CENSR_RULE_EMAILS) && std::regex_search(t, email_re()))
		return true;
	if (mask & CENSR_RULE_APIKEYS) {
		if (std::regex_search(t, aws_re()))
			return true;
		if (std::regex_search(t, github_re()))
			return true;
		if (std::regex_search(t, sk_re()))
			return true;
	}
	/* Label keyword only counts in an assignment-like context. */
	if ((mask & CENSR_RULE_LABELS) && std::regex_search(t, label_re()) &&
	    (std::regex_search(t, assignment_re()) ||
	     std::regex_search(t, upper_ident_re())))
		return true;
	if (!(mask & CENSR_RULE_APIKEYS))
		return false;
	/* High-entropy check on whitespace-separated tokens (not the whole
	 * line — the GPU engine returns a line as one "word", which would make
	 * ordinary long prose lines look high-entropy). */
	{
		size_t i = 0, n = t.size();
		while (i < n) {
			while (i < n && std::isspace((unsigned char)t[i]))
				++i;
			size_t j = i;
			while (j < n && !std::isspace((unsigned char)t[j]))
				++j;
			if (j > i && is_high_entropy_token(t.substr(i, j - i)))
				return true;
			i = j;
		}
	}
	return false;
}

} // namespace

std::vector<RectF> find_secret_boxes(const std::vector<OcrLine> &lines,
				     uint32_t rule_mask)
{
	std::vector<RectF> boxes;
	for (const auto &line : lines) {
		if (line.words.empty())
			continue;
		if (line_is_secret(line, rule_mask))
			boxes.push_back(line.box);
	}
	return boxes;
}

} // namespace censr
